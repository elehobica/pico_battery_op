/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/
/* Except for 'recover_from_sleep' part, see comment for copyright */

#include "power_management.h"

#include <cstdio>

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
static const uint32_t PIN_POWER_KEEP = 19;
// Power Switch
static const uint32_t PIN_POWER_SW = 21;
// User Switch
static const uint32_t PIN_USER_SW = 17;
// Peripheral Power Enable (Active Low)
static const uint32_t PIN_PERI_POWER_ENB = 20;

// Battery Voltage Pin (GPIO29: ADC3) (Raspberry Pi Pico built-in circuit)
static const uint32_t PIN_BATT_LVL = 29;
static const uint32_t ADC_PIN_BATT_LVL = 3;

// ADC Timer & frequency for Battery monitor
static repeating_timer_t timer;
const int TIMER_ADC_HZ = 20;
const int BATT_CHECK_INTERVAL_SEC = 5;

// Battery voltage
const uint16_t LOW_BATTERY_THRESHOLD = 2900;
static uint16_t _bat_mv = 4200;

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

static void _start_serial()
{
    stdio_uart_init();
    stdio_usb_init(); // don't call multiple times without stdio_usb_deinit because of duplicated IRQ calls
}

static void _monitor_battery_voltage()
{
    // ADC Calibration Coefficients
    // ADC3 pin is connected to middle point of voltage divider 200Kohm + 100Kohm
    const int16_t coef_a = 9875;
    const int16_t coef_b = -20;
    adc_select_input(ADC_PIN_BATT_LVL);
    uint16_t result = adc_read();
    _bat_mv = result * coef_a / (1<<12) + coef_b;
    //printf("Battery Voltage = %d (mV)\n", bat_mv);
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

static bool _timer_callback_adc(repeating_timer_t *rt) {
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
    // Periphearal Power Enable (Active Low) (Open Drain) (external pull up)
    gpio_init(PIN_PERI_POWER_ENB);
    gpio_disable_pulls(PIN_PERI_POWER_ENB);
    //pm_set_peripheral_power(false);
    pm_set_peripheral_power(true);

    // Power Keep Pin (Output)
    gpio_init(PIN_POWER_KEEP);
    gpio_set_dir(PIN_POWER_KEEP, GPIO_OUT);

    // Power Switch (Input)
    gpio_init(PIN_POWER_SW);
    gpio_disable_pulls(PIN_POWER_SW);
    gpio_set_dir(PIN_POWER_SW, GPIO_IN);

    // User Switch (Input)
    gpio_init(PIN_USER_SW);
    gpio_pull_up(PIN_USER_SW);
    gpio_set_dir(PIN_USER_SW, GPIO_IN);

    // USB Power detect Pin = Charge detect (Input)
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

uint16_t pm_get_battery_voltage()
{
    return _bat_mv;
}

bool pm_get_low_battery()
{
    static bool low_battery = false; // never turn to false once true
    if (!low_battery && _bat_mv < LOW_BATTERY_THRESHOLD) {
        low_battery = true;
    }
    return low_battery;
}

void pm_set_peripheral_power(bool value)
{
    // Open Drain, Active Low
    if (value) {
        gpio_put(PIN_PERI_POWER_ENB, 0);
        gpio_set_dir(PIN_PERI_POWER_ENB, GPIO_OUT); 
    } else {
        gpio_put(PIN_PERI_POWER_ENB, 0);
        gpio_set_dir(PIN_PERI_POWER_ENB, GPIO_IN); 
    }
}

bool pm_get_peripheral_power()
{
    // True if GPIO_OUT
    return gpio_get_dir(PIN_PERI_POWER_ENB);
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
}

void pm_reboot()
{
    watchdog_reboot(0, 0, PICO_STDIO_USB_RESET_RESET_TO_FLASH_DELAY_MS);
}

bool pm_is_caused_reboot()
{
    return watchdog_caused_reboot();
}

bool pm_get_btn_evt(button_action_t *btn_act)
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
