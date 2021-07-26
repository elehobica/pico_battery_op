/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/

#include <cstdint>
#include <cstdio>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "lib/pico-ssd1306/ssd1306.h"
#include "power_management.h"

typedef enum {
    NormalMode = 0,
    DormantMode,
    ShutdownMode,
    ChargeMode
} power_mode_t;

static ssd1306_t disp;
static power_mode_t power_mode = NormalMode;
static bool peri_power_prev = false;

static inline uint32_t _millis(void)
{
	return to_ms_since_boot(get_absolute_time());
}

void display_init()
{
    const uint32_t PIN_I2C1_SDA = 2;
    const uint32_t PIN_I2C1_SCL = 3;

    i2c_init(i2c1, 400000);
    gpio_set_function(PIN_I2C1_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C1_SCL, GPIO_FUNC_I2C);
    gpio_disable_pulls(PIN_I2C1_SDA); // assume module has pull-up otherwise gpio_pull_up(PIN_I2C1_SDA);
    gpio_disable_pulls(PIN_I2C1_SCL); // assume module has pull-up otherwise gpio_pull_up(PIN_I2C1_SCL);

    disp.external_vcc=false;
    ssd1306_init(&disp, 128, 64, 0x3c, i2c1);
    ssd1306_poweron(&disp);
    ssd1306_clear(&disp);
}

void display_deinit()
{
    ssd1306_poweroff(&disp);
    ssd1306_deinit(&disp);
}

int main()
{
    int mode_count = 0;

    // LED Pin
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    pm_init(); // Serial terminal also starts from here
    printf("Battery Op. Demo\n");

    sleep_ms(100);
    if (pm_usb_power_detected()) {
        power_mode = ChargeMode;
    } else {
        power_mode = NormalMode;
    }
    pm_set_peripheral_power(true);

    sleep_ms(250);

    while (true) {
        // Monitor
        uint16_t battery_voltage = pm_get_battery_voltage();

        // Control
        button_action_t btn_act;
        if (pm_get_btn_evt(&btn_act)) {
            switch (btn_act) {
                case ButtonPowerLong:
                    if (power_mode == ChargeMode) {
                        power_mode = NormalMode;
                        mode_count = 0;
                    }
                    break;
                case ButtonPowerLongLong:
                    if (power_mode == NormalMode) {
                        power_mode = ShutdownMode;
                        mode_count = 0;
                    }
                    break;
                case ButtonUserLongLong:
                    if (power_mode == NormalMode) {
                        power_mode = DormantMode;
                        mode_count = 0;
                    }
                    break;
                case ButtonUserSingle:
                    if (power_mode == NormalMode) {
                        pm_set_peripheral_power(!pm_get_peripheral_power());
                    }
                    break;
                default:
                    break;
            }
        }
        if (power_mode == NormalMode && pm_get_low_battery()) {
            power_mode = ShutdownMode;
            mode_count = 0;
        }
        if (power_mode == NormalMode && mode_count == 0) {
            pm_set_power_keep(true);
            pm_set_peripheral_power(true);
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
        } else if (power_mode == DormantMode && mode_count >= 30) {
            if (pm_get_peripheral_power()) {
                display_deinit();
                pm_set_peripheral_power(false);
            }
            peri_power_prev = false;
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
            pm_enter_dormant_and_wake();
            pm_set_peripheral_power(true);
            power_mode = NormalMode;
            mode_count = 0;
        } else if (power_mode == ShutdownMode && mode_count >= 30) {
            pm_set_power_keep(false);
            power_mode = ChargeMode;
            mode_count = 0;
        } else if (power_mode == ChargeMode) {
            pm_set_power_keep(false);
            if (mode_count >= 30) {
                pm_set_peripheral_power(false);
            }
        }
        mode_count++;

        // Display (SSD1306 powered by Peripheral Power)
        bool peri_power = pm_get_peripheral_power();
        if (!peri_power_prev && peri_power) {
            sleep_ms(100); // wait for ssd1306 power stable
            display_init();
        } else if (peri_power_prev && !peri_power) {
            display_deinit();
        }
        if (peri_power) {
            char str[64];
            ssd1306_clear(&disp);
            ssd1306_draw_string(&disp, 8*0, 8*0, 1, (char *) "Battery Op. Demo");
            if (power_mode == ChargeMode) {
                if (pm_usb_power_detected()) {
                    if ((mode_count % 10) < 5) {
                        ssd1306_draw_string(&disp, 8*4, 8*4, 1, (char *) "Charging");
                    }
                }
            } else {
                if (pm_usb_power_detected()) {
                    ssd1306_draw_string(&disp, 8*0, 8*2, 1, (char *) "USB Power");
                    sprintf(str, "VSYS: %4.2f V", (float) battery_voltage / 1000.0);
                } else {
                    ssd1306_draw_string(&disp, 8*0, 8*2, 1, (char *) "Battery Power");
                    sprintf(str, "Battery: %4.2f V", (float) battery_voltage / 1000.0);
                }
                ssd1306_draw_string(&disp, 8*0, 8*3, 1, str);
                if (peri_power) {
                    ssd1306_draw_string(&disp, 8*0, 8*4, 1, (char *) "Peri. Power: ON");
                } else {
                    ssd1306_draw_string(&disp, 8*0, 8*4, 1, (char *) "Peri. Power: OFF");
                }
                if (power_mode == DormantMode) {
                    if ((mode_count % 10) < 5) {
                        ssd1306_draw_string(&disp, 8*0, 8*6, 1, (char *) "GO DORMANT");
                    }
                } else if (power_mode == ShutdownMode) {
                    if ((mode_count % 10) < 5) {
                        if (pm_get_low_battery()) {
                            ssd1306_draw_string(&disp, 8*0, 8*6, 1, (char *) "LOW BATTERY");
                        } else {
                            ssd1306_draw_string(&disp, 8*0, 8*6, 1, (char *) "SHUTDOWN");
                        }
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
        }
        peri_power_prev = peri_power;
        pm_clear_btn_evt();

        // Main Process (Do something here)
        if (power_mode == NormalMode) {
            gpio_xor_mask(1UL<<PICO_DEFAULT_LED_PIN);
        } else {
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
        }

        // Loop wait
        sleep_ms(100);
    }

    return 0;
}
