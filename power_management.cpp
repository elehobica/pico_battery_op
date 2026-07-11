/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/
/* Except for 'recover_from_sleep' part, see comment for copyright */

#include "power_management.h"

//#include <cstdio>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pll.h"
#include "hardware/rosc.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/runtime_init.h"
#include "pico/stdlib.h"
#include "pico/sleep.h"
#include "pico/stdio_uart.h"
#include "pico/stdio_usb.h" // use lib/my_pico_stdio_usb/
#include "pico/util/queue.h"

// === Pin Settings for power management ===
// DC/DC mode selection Pin
static const uint32_t PIN_DCDC_PSM_CTRL = 23;
// USB Charge detect Pin
static const uint32_t PIN_USB_POWER_DETECT = 24;
// Power Keep Pin
static const uint32_t PIN_POWER_KEEP = 27;
// Power Switch
static const uint32_t PIN_POWER_SW = 28;
// User Switch
static const uint32_t PIN_USER_SW = 17;

// Battery Voltage Pin (GPIO29: ADC3) (Raspberry Pi Pico built-in circuit)
static const uint32_t PIN_BATT_LVL = 29;
static const uint32_t ADC_PIN_BATT_LVL = 3;

// ADC characteristics
static const uint32_t ADC_RESOLUTION = 12;   // 12-bit ADC (raw range 0 .. 2^12-1)
static const float ADC_REF_VOLTAGE = 3.3;    // [V] ADC reference voltage

// ADC Timer & frequency for Battery monitor
static repeating_timer_t timer;
const int TIMER_ADC_HZ = 20;
const int BATT_CHECK_INTERVAL_SEC = 5;

// Battery voltage
const float LOW_BATTERY_THRESHOLD = 2.9; // [V]
static float _bat_volt = 4.2; // [V]

// for preserving clock configuration
static uint32_t _scr;
static uint32_t _sleep_en0;
static uint32_t _sleep_en1;

// Configuration for button recognition
static const uint32_t RELEASE_IGNORE_COUNT = 8;
static const uint32_t LONG_PUSH_COUNT = 10;
static const uint32_t LONG_LONG_PUSH_COUNT = 30;

static const uint32_t NUM_BTN_HISTORY = 30;
static button_status_t button_prv[NUM_BTN_HISTORY] = {}; // initialized as HP_BUTTON_OPEN
static uint32_t button_repeat_count = LONG_LONG_PUSH_COUNT + 1; // to ignore first buttton press when power-on

// button event queue
static queue_t btn_evt_queue;
static const int QueueLength = 1;

// Power state machine
static const uint32_t DEFAULT_ANNOUNCE_MS = 3000;
static pm_config_t _cfg = { DEFAULT_ANNOUNCE_MS, DEFAULT_ANNOUNCE_MS, DEFAULT_ANNOUNCE_MS };
static pm_callbacks_t _cb = {};
static pm_state_t _state = PmStateIdle;
static pm_state_t _state_prev = PmStateIdle;
static absolute_time_t _state_entered_at;
static bool _boot = false; // true while the initial (boot) PmStateIdle is unresolved
static pm_pending_reason_t _pending = PmPendingNone;
static absolute_time_t _pending_deadline;

static void _start_serial()
{
    stdio_uart_init();
    stdio_usb_init(); // don't call multiple times without stdio_usb_deinit because of duplicated IRQ calls
}

static void _monitor_battery_voltage()
{
    // ADC Calibration Coefficients
    // ADC3 pin is connected to middle point of voltage divider 200Kohm + 100Kohm
    // coef_a: ADC pin voltage -> battery voltage gain (nominal divider ratio 3.0)
    // coef_b: offset [V]
    const float coef_a = 2.9917;
    const float coef_b = -0.020;
    adc_select_input(ADC_PIN_BATT_LVL);
    float adc_voltage = (float) adc_read() * ADC_REF_VOLTAGE / ((1 << ADC_RESOLUTION) - 1); // [V]
    _bat_volt = adc_voltage * coef_a + coef_b; // [V]
    //printf("Battery Voltage = %f (V)\n", _bat_volt);
}

static button_status_t _get_sw_status()
{
    button_status_t ret;
    if (gpio_get(PIN_POWER_SW) == false) {
        ret = ButtonPower;
    } else if (gpio_get(PIN_USER_SW) == false) {
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
        //printf("FIFO was full\n");
    }
    //printf("trigger_event: %d\n", static_cast<int>(button_action));
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

static int timer_init_battery_check()
{
    // negative timeout means exact delay (rather than delay between callbacks)
    if (!add_repeating_timer_us(-1000000 / TIMER_ADC_HZ, _timer_callback_adc, nullptr, &timer)) {
        //printf("Failed to add timer\n");
        return 0;
    }
    return 1;
}

void pm_init()
{
    // Power Keep Pin (Output)
    gpio_init(PIN_POWER_KEEP);
    gpio_set_dir(PIN_POWER_KEEP, GPIO_OUT);

    // Power Switch (Input)
    gpio_init(PIN_POWER_SW);
    gpio_pull_up(PIN_POWER_SW);
    gpio_set_dir(PIN_POWER_SW, GPIO_IN);

    // User Switch (Input)
    gpio_init(PIN_USER_SW);
    gpio_pull_up(PIN_USER_SW);
    gpio_set_dir(PIN_USER_SW, GPIO_IN);

    // USB Power detect Pin = Charge detect (Input)
    gpio_init(PIN_USB_POWER_DETECT);
    gpio_set_dir(PIN_USB_POWER_DETECT, GPIO_IN);

    // Battery Level Input (ADC)
    adc_init();
    adc_gpio_init(PIN_BATT_LVL);

    // DCDC PSM control
    // 0: PFM mode (best efficiency)
    // 1: PWM mode (improved ripple)
    gpio_init(PIN_DCDC_PSM_CTRL);
    gpio_set_dir(PIN_DCDC_PSM_CTRL, GPIO_OUT);
    // PSM control mode can be overwritten after pm_init()
    gpio_put(PIN_DCDC_PSM_CTRL, 0); // PWM mode for best efficiency

    // button event queue
    queue_init(&btn_evt_queue, sizeof(element_t), QueueLength);

    // Battery Check Timer start
    timer_init_battery_check();

    // Serial start
    _start_serial();
}

bool pm_usb_power_detected()
{
    return gpio_get(PIN_USB_POWER_DETECT);
}

void pm_set_power_keep(bool value)
{
    gpio_put(PIN_POWER_KEEP, value);
}

float pm_get_battery_voltage()
{
    return _bat_volt;
}

bool pm_get_low_battery()
{
    static bool low_battery = false; // never turn to false once true
    if (!low_battery && _bat_volt < LOW_BATTERY_THRESHOLD) {
        low_battery = true;
    }
    return low_battery;
}

bool pm_get_power_sw()
{
    // True if Low
    return !gpio_get(PIN_POWER_SW);
}

// === 'recover_from_sleep' part (start) ===================================
// great reference from 'recover_from_sleep'
// https://github.com/ghubcoder/PicoSleepDemo | https://ghubcoder.github.io/posts/awaking-the-pico/
static void _preserve_clock_before_sleep()
{
    _scr = scb_hw->scr;
    _sleep_en0 = clocks_hw->sleep_en0;
    _sleep_en1 = clocks_hw->sleep_en1;
}

static void _recover_clock_after_sleep()
{
    rosc_write(&rosc_hw->ctrl, ROSC_CTRL_ENABLE_BITS); //Re-enable ring Oscillator control
    scb_hw->scr = _scr;
    clocks_hw->sleep_en0 = _sleep_en0;
    clocks_hw->sleep_en1 = _sleep_en1;
    runtime_init_clocks(); // reset clocks
}
// === 'recover_from_sleep' part (end) ===================================

void pm_enter_dormant_and_wake()
{
    // === [1] Preparation for dormant ===
    bool psm = gpio_get(PIN_DCDC_PSM_CTRL);
    gpio_put(PIN_DCDC_PSM_CTRL, 0); // PFM mode for better efficiency
    stdio_usb_deinit(); // terminate usb cdc

    // === [2] goto dormant then wake up ===
    uint32_t ints = save_and_disable_interrupts(); // (+a)
    _preserve_clock_before_sleep(); // (+c)
    //--
    sleep_run_from_xosc();
    sleep_goto_dormant_until_pin(PIN_POWER_SW, true, false); // dormant until fall edge detected
    //--
    _recover_clock_after_sleep(); // (-c)
    restore_interrupts(ints); // (-a)

    // === [3] treatments after wake up ===
    _start_serial();
    gpio_put(PIN_DCDC_PSM_CTRL, psm); // recover PWM mode
    gpio_init(PIN_POWER_SW);  // restore GPIO setting for dormant pin
    gpio_pull_up(PIN_POWER_SW);
    gpio_set_dir(PIN_POWER_SW, GPIO_IN);

    // Ignore the wake-up Power switch push (and its release) so it is not recognized
    // as a button gesture (e.g. ButtonPowerSingle would re-enter dormant immediately).
    _reset_button_state();
}

void pm_reboot()
{
    watchdog_reboot(0, 0, PICO_STDIO_USB_RESET_RESET_TO_FLASH_DELAY_MS);
}

bool pm_is_caused_reboot()
{
    return watchdog_caused_reboot();
}

bool pm_get_btn_evt(button_action_t* btn_act)
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

void pm_clear_btn_evt()
{
    // queue doesn't work as intended when removing rest items after removed or poke once
    // Therefore set QueueLength = 1 at main.cpp instead of removing here
    /*
    int count = queue_get_level(btn_evt_queue);
    while (count) {
        element_t element;
        queue_remove_blocking(btn_evt_queue, &element);
        count--;
    }
    */
}

// === Power state machine =================================================
// Enter a stable state: enforce its power-keep invariant, timestamp it and
// notify the application. No-op if already in that state.
static void _set_state(pm_state_t new_state)
{
    if (new_state == _state) {
        return;
    }
    pm_state_t prev = _state;
    _state = new_state;
    _state_entered_at = get_absolute_time();
    // power-keep invariant: held while Active/Sleep, released in Idle.
    pm_set_power_keep(new_state != PmStateIdle);
    if (_cb.on_state_changed != nullptr) {
        _cb.on_state_changed(new_state, prev);
    }
}

static bool _pending_cancelable(pm_pending_reason_t reason)
{
    return (reason == PmPendingSleep) || (reason == PmPendingShutdown);
}

static void _begin_pending(pm_pending_reason_t reason, uint32_t announce_ms)
{
    _pending = reason;
    _pending_deadline = make_timeout_time_ms(announce_ms);
    if (_cb.on_pending != nullptr) {
        _cb.on_pending(reason);
    }
}

static void _do_dormant()
{
    // Let the application quiesce its peripherals (display, peripheral power)
    // before dormant. Re-enabled on the way back via on_state_changed(PmStateActive).
    if (_cb.on_before_dormant != nullptr) {
        _cb.on_before_dormant();
    }
    pm_enter_dormant_and_wake();
}

static void _commit_pending()
{
    pm_pending_reason_t reason = _pending;
    _pending = PmPendingNone;
    switch (reason) {
        case PmPendingSleep:
            // battery nap: latch stays held, dormant, wake back to Active
            _set_state(PmStateSleep);
            _do_dormant();
            _set_state(PmStateActive);
            break;
        case PmPendingShutdown:
        case PmPendingLowBattery:
            // release the latch; PmStateIdle then charges (USB) or powers off (no USB)
            _set_state(PmStateIdle);
            break;
        case PmPendingCharge:
            // charging in PmStateIdle: dormant, wake back to Active
            _do_dormant();
            _set_state(PmStateActive);
            break;
        default:
            break;
    }
}

void pm_start(const pm_callbacks_t* callbacks, const pm_config_t* config)
{
    if (callbacks != nullptr) {
        _cb = *callbacks;
    }
    if (config != nullptr) {
        _cfg = *config;
    }
    _pending = PmPendingNone;
    // The initial PmStateIdle is the boot boundary; pm_process() resolves it on
    // the first tick (USB -> charge, no USB -> run). The _boot flag scopes the
    // "PmStateIdle + no USB -> Active" rule to boot only, so a later shutdown
    // into PmStateIdle (no USB) powers off instead of looping back to Active.
    _boot = true;
    _state = PmStateIdle;
    _state_prev = PmStateIdle;
    _state_entered_at = get_absolute_time();
    pm_set_power_keep(false); // PmStateIdle invariant (latch released)
}

void pm_process()
{
    // While a pending (announce) phase is active, forward button events to the
    // application (so it can pm_cancel()) and auto-commit when the deadline hits.
    if (_pending != PmPendingNone) {
        button_action_t btn_act;
        if (pm_get_btn_evt(&btn_act)) {
            if (_cb.on_button_event != nullptr) {
                _cb.on_button_event(btn_act);
            }
        }
        pm_clear_btn_evt();
        if (_pending != PmPendingNone && time_reached(_pending_deadline)) {
            _commit_pending();
        }
        return;
    }

    switch (_state) {
        case PmStateActive: {
            if (pm_get_low_battery()) {
                _begin_pending(PmPendingLowBattery, _cfg.shutdown_announce_ms);
                break;
            }
            button_action_t btn_act;
            if (pm_get_btn_evt(&btn_act)) {
                switch (btn_act) {
                    case ButtonPowerSingle:
                        _begin_pending(PmPendingSleep, _cfg.sleep_announce_ms);
                        break;
                    case ButtonPowerLongLong:
                        _begin_pending(PmPendingShutdown, _cfg.shutdown_announce_ms);
                        break;
                    default:
                        // forward events not consumed as a power trigger
                        if (_cb.on_button_event != nullptr) {
                            _cb.on_button_event(btn_act);
                        }
                        break;
                }
            }
            pm_clear_btn_evt();
            break;
        }
        case PmStateIdle:
            // Reached as the boot boundary, or via shutdown / low-battery commit.
            //   USB present    : announce charging, then dormant.
            //   no USB & boot   : start running (assert power-keep).
            //   no USB & !boot  : post-shutdown -> the hardware is powering off.
            if (pm_usb_power_detected()) {
                _begin_pending(PmPendingCharge, _cfg.charge_announce_ms);
            } else if (_boot) {
                _set_state(PmStateActive);
            }
            _boot = false; // the boot boundary is handled once
            break;
        default:
            break;
    }
}

pm_state_t pm_get_state()
{
    return _state;
}

bool pm_get_pending(pm_pending_info_t* out)
{
    if (_pending == PmPendingNone) {
        return false;
    }
    if (out != nullptr) {
        out->reason = _pending;
        int64_t remaining_us = absolute_time_diff_us(get_absolute_time(), _pending_deadline);
        out->remaining_ms = (remaining_us > 0) ? (uint32_t)(remaining_us / 1000) : 0;
        out->cancelable = _pending_cancelable(_pending);
    }
    return true;
}

void pm_cancel()
{
    if (_pending != PmPendingNone && _pending_cancelable(_pending)) {
        _pending = PmPendingNone;
        // The stable state was unchanged during the pending, so nothing else to do.
    }
}

uint32_t pm_get_state_elapsed_ms()
{
    int64_t elapsed_us = absolute_time_diff_us(_state_entered_at, get_absolute_time());
    return (elapsed_us > 0) ? (uint32_t)(elapsed_us / 1000) : 0;
}
