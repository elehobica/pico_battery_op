/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/

#include "pico/stdlib.h"
#include "pico_battery_op.h"

// LED blink rate: 1 Hz on a fresh startup, 2 Hz after resuming from dormant mode.
static bool resumed_from_sleep = false;

// Turn the LED off just before the library enters dormant mode (a Sleep keeps
// PboStateActive). This keeps the LED dark while dormant, regardless of the
// blink phase at that moment.
static void on_enter_dormant(void)
{
    gpio_put(PICO_DEFAULT_LED_PIN, false);
}

// Just after waking from dormant mode; switch the blink rate to 2 Hz.
static void on_exit_dormant(void)
{
    resumed_from_sleep = true;
}

int main(void)
{
    // On-board LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Power management with the default configuration (no extra devices, no serial
    // output). Callbacks handle the LED around dormant mode.
    pbo_config_t config = pbo_get_default_config();
    config.callbacks.on_enter_dormant = on_enter_dormant;
    config.callbacks.on_exit_dormant = on_exit_dormant;
    pbo_init(&config);
    pbo_start();

    while (true) {
        // Advance the power state machine (may block while dormant).
        pbo_process();

        // Blink the on-board LED while running: 1 Hz (500 ms on / 500 ms off) after a
        // fresh startup, 2 Hz (250 ms on / 250 ms off) after resuming from dormant mode.
        // Keep it off in any other state.
        if (pbo_get_state() == PboStateActive) {
            uint32_t half_ms = resumed_from_sleep ? 250 : 500;
            bool on = (to_ms_since_boot(get_absolute_time()) / half_ms) % 2 == 0;
            gpio_put(PICO_DEFAULT_LED_PIN, on);
        } else {
            gpio_put(PICO_DEFAULT_LED_PIN, false);
        }

        sleep_ms(50);
    }

    return 0;
}
