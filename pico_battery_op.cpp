/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/

#include "pico_battery_op.h"

#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/regs/io_bank0.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#if defined(ARDUINO)
// The Arduino core does not ship pico-extras, so the pico_sleep sources are vendored into this
// library. Their global symbols are prefixed with pbov_ so they cannot clash with another library
// that bundles the same pico-extras sources; map them back to the SDK names used below.
#include "pbo_vendor/pbo_sleep.h"
#define sleep_run_from_xosc          pbov_sleep_run_from_xosc
#define sleep_goto_dormant_until_pin pbov_sleep_goto_dormant_until_pin
#define sleep_power_up               pbov_sleep_power_up
#else
#include "pico/sleep.h"
#endif
#if !defined(ARDUINO)
// Under the Arduino core the USB CDC / Serial stack is owned by the core, and
// pico_stdio_uart / pico_stdio_usb are not linked, so these are excluded there.
#include "pico/stdio_uart.h"
#include "pico/stdio_usb.h"
#endif
#include "pico/util/queue.h"

// Debug print. Enabled only when PBO_DPRINT is defined (e.g. -DPBO_DPRINT at build
// time); otherwise it compiles to nothing (the default: no code, no output).
#ifdef PBO_DPRINT
#include <cstdio>
#define pbo_dprintf(...) printf(__VA_ARGS__)
#else
#define pbo_dprintf(...) ((void)0)
#endif

// === Internal types (not exposed to the application) ===
// Raw switch status used by the button-gesture classifier.
typedef enum _button_status_t {
    ButtonOpen = 0,
    ButtonPower,
    ButtonUser
} button_status_t;

// Element type of the internal button-event queue.
typedef struct element {
    button_action_t button_action;
} element_t;

// === Pin Settings for power management ===
// Fixed pins (not configurable).
// DC/DC mode selection Pin
static const uint32_t PIN_DCDC_PSM_CTRL = 23;
// USB Charge detect Pin
static const uint32_t PIN_USB_POWER_DETECT = 24;

// Default assignments for the configurable pins (see pbo_config_t / pbo_get_default_config()).
static const uint32_t DEFAULT_PIN_POWER_KEEP = 27; // Power Keep Pin
static const uint32_t DEFAULT_PIN_POWER_SW = 28;   // Power Switch
static const uint32_t DEFAULT_PIN_USER_SW = PBO_PIN_UNUSED; // User Switch (unused by default)

// Battery Voltage Pin (GPIO29: ADC3) (Raspberry Pi Pico built-in circuit)
static const uint32_t PIN_BATT_LVL = 29;
static const uint32_t ADC_PIN_BATT_LVL = 3;  // ADC3

// ADC characteristics
static const uint32_t ADC_RESOLUTION = 12;   // 12-bit ADC (raw range 0 .. 2^12-1)
static const float ADC_REF_VOLTAGE = 3.3;    // [V] ADC reference voltage

// ADC Timer & frequency for Battery monitor
static repeating_timer_t timer;
const int TIMER_ADC_HZ = 20;
const int BATT_CHECK_INTERVAL_SEC = 5;

// Battery voltage
// Initial placeholder held until the first ADC sample (~5 s after boot). It must
// stay above DEFAULT_LOW_BATTERY_THRESHOLD so the low-battery latch does not
// false-trigger before a real measurement (Li-ion nominal full charge).
static const float DEFAULT_BATT_VOLTAGE = 4.2; // [V]
static float _bat_volt = DEFAULT_BATT_VOLTAGE; // [V]

// Default battery monitor parameters (see pbo_config_t).
// ADC3 pin is connected to middle point of voltage divider 200Kohm + 100Kohm.
static const float DEFAULT_BATT_CALIB_COEF_A = 2.9917; // scale ADC pin voltage -> battery voltage (nominal divider ratio 3.0)
static const float DEFAULT_BATT_CALIB_COEF_B = -0.020; // constant offset added after scaling [V]
static const float DEFAULT_LOW_BATTERY_THRESHOLD = 2.9; // [V]

// Delay before the reboot watchdog fires (pbo_reboot()). Formerly borrowed from the
// pico_stdio_usb internal PICO_CONFIG PICO_STDIO_USB_RESET_RESET_TO_FLASH_DELAY_MS
// (default 100 ms), which is no longer visible to application code in newer Pico SDKs;
// use a local constant so pbo_reboot() stays SDK-version robust.
static const uint32_t REBOOT_DELAY_MS = 100;

// Configuration for button recognition
static const uint32_t RELEASE_IGNORE_COUNT = 8;
static const uint32_t LONG_PUSH_COUNT = 20;      // 20 ticks / 20 Hz = 1 s
static const uint32_t LONG_LONG_PUSH_COUNT = 40; // 40 ticks / 20 Hz = 2 s

static const uint32_t NUM_BTN_HISTORY = 30;
static button_status_t button_prv[NUM_BTN_HISTORY] = {}; // initialized as HP_BUTTON_OPEN
static uint32_t button_repeat_count = LONG_LONG_PUSH_COUNT + 1; // to ignore first buttton press when power-on

// button event queue
static queue_t btn_evt_queue;
static const int QueueLength = 1;

// Power state machine
static const uint32_t DEFAULT_DEFER_MS = 0; // no delay by default (deferred actions run on the next pbo_process())
// Active configuration (overwritten by pbo_init()). Defaults live in a single place,
// pbo_get_default_config(), so there is only one place to maintain them.
static pbo_config_t _cfg = pbo_get_default_config();
static pbo_callbacks_t _cb = {};
static pbo_state_t _state = PboStateIdle;
static pbo_state_t _state_prev = PboStateIdle;
static absolute_time_t _state_entered_at;
static bool _boot = false; // true while the initial (boot) PboStateIdle is unresolved
static bool _boot_run = false; // whether to come up running at boot (set in pbo_init, applied by pbo_process)
static pbo_deferred_reason_t _deferred = PboDeferredNone;
static absolute_time_t _defer_deadline;

// =========================================================================
// Internal (static) functions
// =========================================================================

static void _start_serial()
{
#if !defined(ARDUINO)
    stdio_uart_init();
    stdio_usb_init(); // don't call multiple times without stdio_usb_deinit because of duplicated IRQ calls
#endif
}

static void _set_power_keep(bool value)
{
    gpio_put(_cfg.pin_power_keep, value);
}

static void _monitor_battery_voltage()
{
    // ADC calibration coefficients come from the config (see pbo_config_t / DEFAULT_BATT_CALIB_COEF_*).
    adc_select_input(ADC_PIN_BATT_LVL);
    float adc_voltage = (float) adc_read() * ADC_REF_VOLTAGE / ((1 << ADC_RESOLUTION) - 1); // [V]
    _bat_volt = adc_voltage * _cfg.batt_calib_coef_a + _cfg.batt_calib_coef_b; // [V]
    pbo_dprintf("Battery Voltage = %f (V)\n", _bat_volt);
}

static bool _get_low_battery()
{
    static bool low_battery = false; // never turn to false once true
    if (!low_battery && _bat_volt < _cfg.low_battery_threshold) {
        low_battery = true;
    }
    return low_battery;
}

static button_status_t _get_sw_status()
{
    button_status_t ret;
    if (gpio_get(_cfg.pin_power_sw) == false) {
        ret = ButtonPower;
    } else if (_cfg.pin_user_sw != PBO_PIN_UNUSED && gpio_get(_cfg.pin_user_sw) == false) {
        ret = ButtonUser;
    } else {
        ret = ButtonOpen;
    }
    return ret;
}

static int _count_clicks(button_status_t target_status)
{
    int i;
    int detected_fall = 0;
    int count = 0;
    for (i = 0; i < 4; i++) {
        if (button_prv[i] != ButtonOpen) {
            return 0;
        }
    }
    for (i = 4; i < NUM_BTN_HISTORY; i++) {
        if (detected_fall == 0 && button_prv[i-1] == ButtonOpen && button_prv[i] == target_status) {
            detected_fall = 1;
        } else if (detected_fall == 1 && button_prv[i-1] == target_status && button_prv[i] == ButtonOpen) {
            count++;
            detected_fall = 0;
        }
    }
    if (count > 0) {
        for (i = 0; i < NUM_BTN_HISTORY; i++) button_prv[i] = ButtonOpen;
    }
    return count;
}

static void _trigger_event(button_action_t button_action)
{
    element_t element = {
        .button_action = button_action
    };
    if (!queue_try_add(&btn_evt_queue, &element)) {
        pbo_dprintf("FIFO was full\n");
    }
    pbo_dprintf("trigger_event: %d\n", static_cast<int>(button_action));
    return;
}

static void _update_button_action()
{
    int i;
    button_status_t button = _get_sw_status();
    if (button == ButtonOpen) {
        // Ignore button release after long push
        if (button_repeat_count > LONG_PUSH_COUNT) {
            for (i = 0; i < NUM_BTN_HISTORY; i++) {
                button_prv[i] = ButtonOpen;
            }
            button = ButtonOpen;
        }
        button_repeat_count = 0;
        if (button_prv[RELEASE_IGNORE_COUNT] == ButtonPower) { // Power Switch release
            int center_clicks = _count_clicks(ButtonPower); // must be called once per tick because button_prv[] status has changed
            switch (center_clicks) {
                case 1:
                    _trigger_event(ButtonPowerSingle);
                    break;
                case 2:
                    _trigger_event(ButtonPowerDouble);
                    break;
                case 3:
                    _trigger_event(ButtonPowerTriple);
                    break;
                default:
                    break;
            }
        } else if (button_prv[RELEASE_IGNORE_COUNT] == ButtonUser) { // User Switch release
            int center_clicks = _count_clicks(ButtonUser); // must be called once per tick because button_prv[] status has changed
            switch (center_clicks) {
                case 1:
                    _trigger_event(ButtonUserSingle);
                    break;
                case 2:
                    _trigger_event(ButtonUserDouble);
                    break;
                case 3:
                    _trigger_event(ButtonUserTriple);
                    break;
                default:
                    break;
            }
        }
    } else if (button_repeat_count == LONG_PUSH_COUNT) { // long push
        if (button == ButtonPower) {
            _trigger_event(ButtonPowerLong);
            button_repeat_count++; // only once and step to longer push event
        } else if (button == ButtonUser) {
            _trigger_event(ButtonUserLong);
            button_repeat_count++; // only once and step to longer push event
        }
    } else if (button_repeat_count == LONG_LONG_PUSH_COUNT) { // long long push
        if (button == ButtonPower) {
            _trigger_event(ButtonPowerLongLong);
        } else if (button == ButtonUser) {
            _trigger_event(ButtonUserLongLong);
        }
        button_repeat_count++; // only once and step to longer push event
    } else if (button == button_prv[0]) {
        button_repeat_count++;
    }
    // Button status shift
    for (i = NUM_BTN_HISTORY-2; i >= 0; i--) {
        button_prv[i+1] = button_prv[i];
    }
    button_prv[0] = button;
}

// Reset button recognition so that a currently-held press is ignored until released.
// Used at power-on and after dormant wake (both are triggered by a Power switch push,
// whose release must not be recognized as a button gesture).
static void _reset_button_state()
{
    for (int i = 0; i < NUM_BTN_HISTORY; i++) {
        button_prv[i] = ButtonOpen;
    }
    button_repeat_count = LONG_LONG_PUSH_COUNT + 1; // ignore the ongoing press until release
    // drain any pending button event
    element_t element;
    while (queue_try_remove(&btn_evt_queue, &element)) {}
}

static bool _timer_callback_adc(repeating_timer_t* rt) {
    static int count = 0;
    _update_button_action();
    if (count % (TIMER_ADC_HZ * BATT_CHECK_INTERVAL_SEC) == TIMER_ADC_HZ * BATT_CHECK_INTERVAL_SEC - 1) {
        _monitor_battery_voltage();
    }
    count++;
    return true; // keep repeating
}

static int _timer_init_battery_check()
{
    // negative timeout means exact delay (rather than delay between callbacks)
    if (!add_repeating_timer_us(-1000000 / TIMER_ADC_HZ, _timer_callback_adc, nullptr, &timer)) {
        pbo_dprintf("Failed to add timer\n");
        return 0;
    }
    return 1;
}

static bool _get_btn_evt(button_action_t* btn_act)
{
    int count = queue_get_level(&btn_evt_queue);
    if (count) {
        element_t element;
        queue_remove_blocking(&btn_evt_queue, &element);
        *btn_act = element.button_action;
        return true;
    }
    return false;
}

static void _clear_btn_evt()
{
    // queue doesn't work as intended when removing rest items after removed or poke once
    // Therefore set QueueLength = 1 instead of removing here
    /*
    int count = queue_get_level(btn_evt_queue);
    while (count) {
        element_t element;
        queue_remove_blocking(btn_evt_queue, &element);
        count--;
    }
    */
}

static void _enter_dormant_and_wake()
{
    // === [1] Preparation for dormant ===
    bool psm = gpio_get(PIN_DCDC_PSM_CTRL);
    gpio_put(PIN_DCDC_PSM_CTRL, 0); // PFM mode for better efficiency
#if !defined(ARDUINO)
    stdio_usb_deinit(); // terminate usb cdc
#endif

    // === [2] goto dormant then wake up ===
    // Clock preserve/restore is handled by the Pico SDK: sleep_run_from_xosc() switches the
    // clocks to the dormant source and sleep_power_up() restores them on wake. This replaces
    // the formerly ported 'recover_from_sleep' block and matches the pico-extras
    // 'hello_dormant' example.
    uint32_t ints = save_and_disable_interrupts(); // (+a)
    sleep_run_from_xosc();
    // go to dormant until the Power switch is pushed (fall edge detected)
    sleep_goto_dormant_until_pin(_cfg.pin_power_sw, true, false);

    // ---------------
    // --- Dormant ---
    // ---------------

    // wake up from here (Power switch push)
    sleep_power_up(); // restore clocks / oscillators after dormant
    restore_interrupts(ints); // (-a)

    // === [3] treatments after wake up ===
    _start_serial();
    gpio_put(PIN_DCDC_PSM_CTRL, psm); // recover PWM mode
    gpio_init(_cfg.pin_power_sw);  // restore GPIO setting for dormant pin
    gpio_pull_up(_cfg.pin_power_sw);
    gpio_set_dir(_cfg.pin_power_sw, GPIO_IN);

    // Ignore the wake-up Power switch push (and its release) so it is not recognized
    // as a button gesture (e.g. ButtonPowerSingle would re-enter dormant immediately).
    _reset_button_state();
}

// === Power state machine =================================================
// Enter a stable state: enforce its power-keep invariant, timestamp it and
// notify the application. No-op if already in that state.
static void _set_state(pbo_state_t new_state)
{
    if (new_state == _state) {
        return;
    }
    pbo_state_t prev = _state;
    _state = new_state;
    _state_entered_at = get_absolute_time();
    // power-keep invariant: held while Active, released in Idle.
    _set_power_keep(new_state == PboStateActive);
    if (_cb.on_state_changed != nullptr) {
        _cb.on_state_changed(new_state, prev);
    }
}

static bool _deferred_cancelable(pbo_deferred_reason_t reason)
{
    return (reason == PboDeferredSleep) || (reason == PboDeferredShutdown);
}

static void _begin_defer(pbo_deferred_reason_t reason, uint32_t defer_ms)
{
    _deferred = reason;
    _defer_deadline = make_timeout_time_ms(defer_ms);
    if (_cb.on_deferred != nullptr) {
        _cb.on_deferred(reason);
    }
}

// Enter dormant mode and resume running. Shared by Sleep and Charging: the power-keep latch
// (held for Sleep, released for Charging) is already set by the current state's invariant, so
// this touches only the callbacks.
static void _dormant_and_resume()
{
    if (_cb.on_enter_dormant != nullptr) {
        _cb.on_enter_dormant();
    }
    _enter_dormant_and_wake();             // blocks until the Power switch
    _set_state(PboStateActive);             // resume running (no-op if already Active)
    if (_cb.on_exit_dormant != nullptr) {
        _cb.on_exit_dormant();
    }
}

static void _run_deferred()
{
    pbo_deferred_reason_t reason = _deferred;
    _deferred = PboDeferredNone;
    switch (reason) {
        case PboDeferredSleep:    // enter dormant from PboStateActive (Sleep, latch held)
        case PboDeferredCharge:   // enter dormant from PboStateIdle (Charging, latch released)
            _dormant_and_resume();
            break;
        case PboDeferredShutdown:
        case PboDeferredLowBattery:
            // release the latch; PboStateIdle then runs Charging (USB) or powers off (no USB)
            _set_state(PboStateIdle);
            break;
        default:
            break;
    }
}

// Map a POWER-switch gesture to its configured power action (see pbo_config_t).
// User gestures (and anything else) return PboActionNone, i.e. forward to the app.
static pbo_power_action_t _power_action_for(button_action_t btn_act)
{
    switch (btn_act) {
        case ButtonPowerSingle:   return _cfg.power_action_single;
        case ButtonPowerDouble:   return _cfg.power_action_double;
        case ButtonPowerTriple:   return _cfg.power_action_triple;
        case ButtonPowerLong:     return _cfg.power_action_long;
        case ButtonPowerLongLong: return _cfg.power_action_longlong;
        default:                  return PboActionNone;
    }
}

// =========================================================================
// Public functions (declaration order follows pico_battery_op.h)
// =========================================================================

pbo_config_t pbo_get_default_config()
{
    pbo_config_t cfg = {
        DEFAULT_PIN_POWER_KEEP,        // pin_power_keep
        DEFAULT_PIN_POWER_SW,          // pin_power_sw
        DEFAULT_PIN_USER_SW,           // pin_user_sw
        DEFAULT_DEFER_MS,              // sleep_defer_ms
        DEFAULT_DEFER_MS,              // shutdown_defer_ms
        DEFAULT_DEFER_MS,              // charge_defer_ms
        PboActionNone,                 // power_action_single
        PboActionSleep,                // power_action_double
        PboActionNone,                 // power_action_triple
        PboActionNone,                 // power_action_long
        PboActionShutdown,             // power_action_longlong
        DEFAULT_BATT_CALIB_COEF_A,     // batt_calib_coef_a
        DEFAULT_BATT_CALIB_COEF_B,     // batt_calib_coef_b
        DEFAULT_LOW_BATTERY_THRESHOLD, // low_battery_threshold
        {}                             // callbacks
    };
    return cfg;
}

void pbo_init(const pbo_config_t* config)
{
    _cfg = (config != nullptr) ? *config : pbo_get_default_config();
    _cb = _cfg.callbacks;

    // Power Switch (Input) - also read below for the boot POWER_KEEP decision.
    gpio_init(_cfg.pin_power_sw);
    gpio_pull_up(_cfg.pin_power_sw);
    gpio_set_dir(_cfg.pin_power_sw, GPIO_IN);

    // USB Power detect Pin = Charge detect (Input) - also read for the boot decision.
    gpio_init(PIN_USB_POWER_DETECT);
    gpio_set_dir(PIN_USB_POWER_DETECT, GPIO_IN);

    // Power Keep Pin (Output). Decide whether to come up running and drive POWER_KEEP to
    // that level BEFORE enabling output, so it is never pulsed low. On a warm reset while
    // still powered, a low pulse would command the board's own DC/DC off and can brown-out
    // / hang it.
    //   USB present -> release (Charging path).
    //   no USB      -> come up running only if the power switch is held (a real power-on);
    //                  otherwise release POWER_KEEP -> hardware Stand-by (power off). So a
    //                  reset released with no USB always restarts from OFF, regardless of
    //                  the prior state. (A powered-off board cannot boot at all, so RESET
    //                  can never turn it on - only the power switch or USB can.)
    if (gpio_get(PIN_USB_POWER_DETECT)) {
        _boot_run = false;
    } else {
        _boot_run = (gpio_get(_cfg.pin_power_sw) == false); // switch held == real power-on
    }
    gpio_init(_cfg.pin_power_keep);
    gpio_put(_cfg.pin_power_keep, _boot_run); // set level while still input (no low glitch)
    gpio_set_dir(_cfg.pin_power_keep, GPIO_OUT);

    // User Switch (Input) - skipped when not wired (PBO_PIN_UNUSED)
    if (_cfg.pin_user_sw != PBO_PIN_UNUSED) {
        gpio_init(_cfg.pin_user_sw);
        gpio_pull_up(_cfg.pin_user_sw);
        gpio_set_dir(_cfg.pin_user_sw, GPIO_IN);
    }

    // Battery Level Input (ADC)
    adc_init();
    adc_gpio_init(PIN_BATT_LVL);

    // DCDC PSM control
    // 0: PFM mode (best efficiency)
    // 1: PWM mode (improved ripple)
    gpio_init(PIN_DCDC_PSM_CTRL);
    gpio_set_dir(PIN_DCDC_PSM_CTRL, GPIO_OUT);
    // PSM control mode can be overwritten after pbo_init()
    gpio_put(PIN_DCDC_PSM_CTRL, 0); // PWM mode for best efficiency

    // button event queue
    queue_init(&btn_evt_queue, sizeof(element_t), QueueLength);

    // Battery Check Timer start
    _timer_init_battery_check();

    // Serial start
    _start_serial();
}

float pbo_get_battery_voltage()
{
    return _bat_volt;
}

bool pbo_get_usb_power_detected()
{
    return gpio_get(PIN_USB_POWER_DETECT);
}

void pbo_reboot()
{
    watchdog_reboot(0, 0, REBOOT_DELAY_MS);
}

bool pbo_is_caused_reboot()
{
    return watchdog_caused_reboot();
}

uint32_t pbo_get_dormant_reserved_pin_mask()
{
    // GPIOs the library uses that an application low-leakage sweep must never touch: the
    // wake pin (pin_power_sw) and the DC/DC power-keep latch (pin_power_keep) must retain
    // their state through dormant, and the remaining library-owned pins are simply left
    // as-is (the library does not sweep its own pins).
    uint32_t mask = (1u << _cfg.pin_power_keep)
                  | (1u << _cfg.pin_power_sw)
                  | (1u << PIN_DCDC_PSM_CTRL)
                  | (1u << PIN_USB_POWER_DETECT)
                  | (1u << PIN_BATT_LVL);
    if (_cfg.pin_user_sw != PBO_PIN_UNUSED) {
        mask |= (1u << _cfg.pin_user_sw);
    }
    return mask;
}

void pbo_dormant_set_low_leakage(uint32_t app_hold_mask)
{
    // Put every GPIO that is neither reserved by the library nor held by the application
    // into the lowest-leakage state (pulls off, input buffer off, output driver off) to
    // minimize current while dormant. This operation is destructive and does not save the
    // previous pad state, so the application must re-initialize any pin it let go (i.e. not
    // in app_hold_mask) after wake - typically in on_exit_dormant(). Call this just before
    // entering dormant, typically from on_enter_dormant().
    //
    // This mirrors the SDK low_power_set_pins_low_leakage_exclude_mask(), reimplemented here so the
    // library does not depend on pico_low_power. Linking pico_low_power would drag in its
    // Pstate / persistent-data code, which fails to link under the Arduino core on RP2350 (the
    // core's linker script omits __persistent_data_start__/__persistent_data_end__).
    //
    // NOTE: Pico / Pico 2 have <= 30 GPIOs, so this 32-bit mask covers all of bank 0; the loop is
    // capped at 32 to keep the shift well-defined on any larger package.
    const uint32_t exclude = pbo_get_dormant_reserved_pin_mask() | app_hold_mask;
    const uint32_t n = (NUM_BANK0_GPIOS < 32) ? NUM_BANK0_GPIOS : 32;
    for (uint32_t i = 0; i < n; i++) {
        if (exclude & (1u << i)) continue;
        gpio_disable_pulls(i);
        gpio_set_input_enabled(i, false);
        gpio_set_oeover(i, IO_BANK0_GPIO0_CTRL_OEOVER_VALUE_DISABLE);
    }
}

void pbo_start()
{
    // Config and callbacks were already taken by pbo_init().
    _deferred = PboDeferredNone;
    // The initial PboStateIdle is the boot boundary; pbo_process() resolves it on
    // the first tick (USB -> Charging; no USB -> run only if the power switch was held at
    // boot, see pbo_init/_boot_run). The _boot flag scopes that rule to boot only, so a
    // later shutdown into PboStateIdle (no USB) powers off instead of re-running.
    _boot = true;
    _state = PboStateIdle;
    _state_prev = PboStateIdle;
    _state_entered_at = get_absolute_time();
    // POWER_KEEP was already set to its correct boot level by pbo_init() (glitch-free);
    // do not drive it low here (a low pulse can brown-out the board on a warm reset).
}

void pbo_process()
{
    // While a deferred action is pending, forward button events to the
    // application (so it can pbo_cancel_deferred()) and run it at the deadline.
    if (_deferred != PboDeferredNone) {
        button_action_t btn_act;
        if (_get_btn_evt(&btn_act)) {
            if (_cb.on_button_event != nullptr) {
                _cb.on_button_event(btn_act);
            }
        }
        _clear_btn_evt();
        if (_deferred != PboDeferredNone && time_reached(_defer_deadline)) {
            _run_deferred();
        }
        return;
    }

    switch (_state) {
        case PboStateActive: {
            if (_get_low_battery()) {
                _begin_defer(PboDeferredLowBattery, _cfg.shutdown_defer_ms);
                break;
            }
            button_action_t btn_act;
            if (_get_btn_evt(&btn_act)) {
                switch (_power_action_for(btn_act)) {
                    case PboActionSleep:
                        _begin_defer(PboDeferredSleep, _cfg.sleep_defer_ms);
                        break;
                    case PboActionShutdown:
                        _begin_defer(PboDeferredShutdown, _cfg.shutdown_defer_ms);
                        break;
                    case PboActionNone:
                    default:
                        // forward gestures not mapped to a power action
                        if (_cb.on_button_event != nullptr) {
                            _cb.on_button_event(btn_act);
                        }
                        break;
                }
            }
            _clear_btn_evt();
            break;
        }
        case PboStateIdle:
            // Reached as the boot boundary, or via shutdown / low-battery commit.
            //   USB present     : announce Charging, then dormant.
            //   no USB & boot    : run only if the power switch was held at boot
            //                      (_boot_run); otherwise POWER_KEEP released -> Stand-by.
            //   no USB & !boot   : post-shutdown -> the hardware is powering off.
            if (pbo_get_usb_power_detected()) {
                _begin_defer(PboDeferredCharge, _cfg.charge_defer_ms);
            } else if (_boot && _boot_run) {
                _set_state(PboStateActive);
            }
            _boot = false; // the boot boundary is handled once
            break;
        default:
            break;
    }
}

pbo_state_t pbo_get_state()
{
    return _state;
}

bool pbo_get_deferred(pbo_deferred_info_t* out)
{
    if (_deferred == PboDeferredNone) {
        return false;
    }
    if (out != nullptr) {
        out->reason = _deferred;
        int64_t remaining_us = absolute_time_diff_us(get_absolute_time(), _defer_deadline);
        out->remaining_ms = (remaining_us > 0) ? (uint32_t)(remaining_us / 1000) : 0;
        out->cancelable = _deferred_cancelable(_deferred);
    }
    return true;
}

bool pbo_cancel_deferred()
{
    if (_deferred != PboDeferredNone && _deferred_cancelable(_deferred)) {
        _deferred = PboDeferredNone;
        // The stable state was unchanged while pending, so nothing else to do.
        return true;
    }
    return false;
}

uint32_t pbo_get_state_elapsed_ms()
{
    int64_t elapsed_us = absolute_time_diff_us(_state_entered_at, get_absolute_time());
    return (elapsed_us > 0) ? (uint32_t)(elapsed_us / 1000) : 0;
}
