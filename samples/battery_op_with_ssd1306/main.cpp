/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/

#include <cstdint>
#include <cstdio>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "ssd1306.h"
#include "pico_battery_op.h"

// SSD1306 OLED display pins
static const uint32_t PIN_I2C0_SDA      = 12;
static const uint32_t PIN_I2C0_SCL      = 13;
static const uint32_t PIN_SSD1306_POWER = 14;

static ssd1306_t disp;

static inline uint32_t _millis(void)
{
	return to_ms_since_boot(get_absolute_time());
}

void display_power_init()
{
    gpio_init(PIN_SSD1306_POWER);
    gpio_disable_pulls(PIN_SSD1306_POWER);
    gpio_set_dir(PIN_SSD1306_POWER, GPIO_OUT);
    gpio_put(PIN_SSD1306_POWER, 0);
}

void set_display_power(bool flag)
{
    if (flag) {
        gpio_put(PIN_SSD1306_POWER, 1);
    } else {
        gpio_put(PIN_SSD1306_POWER, 0);
    }
}

void display_init()
{
    i2c_init(i2c0, 400000);
    gpio_set_function(PIN_I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C0_SCL, GPIO_FUNC_I2C);
    gpio_disable_pulls(PIN_I2C0_SDA); // assume module has pull-up otherwise gpio_pull_up(PIN_I2C0_SDA);
    gpio_disable_pulls(PIN_I2C0_SCL); // assume module has pull-up otherwise gpio_pull_up(PIN_I2C0_SCL);

    disp.external_vcc=false;
    ssd1306_init(&disp, 128, 64, 0x3c, i2c0);
    ssd1306_poweron(&disp);
    ssd1306_clear(&disp);
}

void display_deinit()
{
    ssd1306_poweroff(&disp);
    ssd1306_deinit(&disp);
}

// === Power management callbacks (application side) =======================
// Power switch single push cancels a pending cancelable deferred action.
static void on_button_event(button_action_t btn_act)
{
    if (btn_act == ButtonPowerSingle) {
        pbo_cancel_deferred(); // abort a cancelable action (GO DORMANT / SHUTDOWN)
    }
}

// Quiesce the display and its power before the library enters dormant.
static void on_enter_dormant()
{
    display_deinit();
    set_display_power(false);

    // Minimize dormant current: put every GPIO except the library's reserved pins into the
    // lowest-leakage state. This board holds no output through dormant, so app_hold_mask is
    // 0; any pin that had to keep driving its level would go into that mask instead. The
    // application pins let go here (LED, display power, I2C) are re-initialized after wake
    // in on_exit_dormant().
    pbo_dormant_set_low_leakage(0);
}

// Restore the application pins the low-leakage sweep let go, re-enable display power, then
// re-initialize the display after waking.
static void on_exit_dormant()
{
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    display_power_init();      // re-init the display-power pad (clears the sweep)
    set_display_power(true);
    sleep_ms(100); // wait for ssd1306 power stable
    display_init();
}

int main()
{
    // LED Pin
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    display_power_init();

    // Start from defaults, then override only what this board needs.
    pbo_config_t config = pbo_get_default_config();
    config.sleep_defer_ms    = 3000;  // 3 s announce before a Sleep (dormant)
    config.shutdown_defer_ms = 3000;  // 3 s announce before shutdown / low battery
    config.charge_defer_ms   = 3000;  // 3 s announce before charging (dormant)
    config.callbacks.on_button_event  = on_button_event;
    config.callbacks.on_enter_dormant = on_enter_dormant;
    config.callbacks.on_exit_dormant  = on_exit_dormant;
    pbo_init(&config); // Serial terminal also starts from here
    printf("Battery Op. Demo\n");

    pbo_start();
    set_display_power(true);
    sleep_ms(100); // wait for ssd1306 power stable
    display_init();

    while (true) {
        // Monitor
        float battery_voltage = pbo_get_battery_voltage();

        // Power state machine (library side; may block while dormant)
        pbo_process();
        pbo_state_t power_state = pbo_get_state();
        pbo_deferred_info_t deferred;
        bool has_deferred = pbo_get_deferred(&deferred);
        bool blink = (_millis() / 500) % 2 == 0; // 1 s period, 50% duty

        char str[64];
        ssd1306_clear(&disp);
        ssd1306_draw_string(&disp, 8*0, 8*0, 1, (char*) "Battery Op. Demo");
        if (power_state == PboStateIdle) {
            // latch released: charging while USB is present
            if (pbo_get_usb_power_detected() && blink) {
                ssd1306_draw_string(&disp, 8*4, 8*4, 1, (char*) "Charging");
            }
        } else { // PboStateActive (running)
            if (pbo_get_usb_power_detected()) {
                ssd1306_draw_string(&disp, 8*0, 8*2, 1, (char*) "USB Power");
                sprintf(str, "VSYS: %4.2f V", battery_voltage);
            } else {
                ssd1306_draw_string(&disp, 8*0, 8*2, 1, (char*) "Battery Power");
                sprintf(str, "Battery: %4.2f V", battery_voltage);
            }
            ssd1306_draw_string(&disp, 8*0, 8*3, 1, str);
            // Announce the pending deferred power action.
            if (has_deferred && blink) {
                const char* msg = nullptr;
                switch (deferred.reason) {
                    case PboDeferredSleep:      msg = "GO DORMANT";  break;
                    case PboDeferredShutdown:   msg = "SHUTDOWN";    break;
                    case PboDeferredLowBattery: msg = "LOW BATTERY"; break;
                    default:                 break;
                }
                if (msg != nullptr) {
                    ssd1306_draw_string(&disp, 8*0, 8*6, 1, (char*) msg);
                }
            }
            uint32_t millis = _millis();
            uint32_t hour = millis / (1000 * 60 * 60);
            uint32_t min = (millis / (1000 * 60)) % 60;
            uint32_t sec = (millis / 1000) % 60;
            sprintf(str, "%d:%02d:%02d", hour, min, sec);
            ssd1306_draw_string(&disp, 8*6, 8*7, 1, str);
        }
        ssd1306_show(&disp);

        // Main Process (Do something here)
        if (power_state == PboStateActive && !has_deferred) {
            gpio_xor_mask(1UL << PICO_DEFAULT_LED_PIN);
        } else {
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
        }

        // Loop wait
        sleep_ms(100);
    }

    return 0;
}
