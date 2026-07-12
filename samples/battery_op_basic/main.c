/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/

#include "pico/stdlib.h"
#include "power_management.h"

// Turn the LED off just before the library goes dormant (e.g. a Sleep nap, which
// stays in PmStateActive). This ensures the LED is dark while sleeping, regardless
// of the blink phase at that moment.
static void on_enter_dormant(void)
{
    gpio_put(PICO_DEFAULT_LED_PIN, false);
}

int main(void)
{
    // On-board LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Power management with the default configuration (no extra devices, no serial
    // output). The only callback turns the LED off before going dormant.
    pm_config_t config = pm_get_default_config();
    config.callbacks.on_enter_dormant = on_enter_dormant;
    pm_init(&config);
    pm_start();

    while (true) {
        // Advance the power state machine (may block while dormant).
        pm_process();

        // While running, blink the on-board LED at 2 Hz (250 ms on / 250 ms off);
        // keep it off in any other state.
        if (pm_get_state() == PmStateActive) {
            bool on = (to_ms_since_boot(get_absolute_time()) / 250) % 2 == 0;
            gpio_put(PICO_DEFAULT_LED_PIN, on);
        } else {
            gpio_put(PICO_DEFAULT_LED_PIN, false);
        }

        sleep_ms(50);
    }

    return 0;
}
