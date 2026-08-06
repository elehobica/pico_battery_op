# battery_op_with_ssd1306

![Scene1](doc/battery_op_with_ssd1306_breadboard.jpg)

Reference application for the [pico_battery_op](../../README.md) power-management library. On top of
the library's power state machine it adds an SSD1306 OLED status display, application display-power
control, an LED activity indicator, and the power-button UX. All of this is **application** code -
the library itself knows nothing about the display, the LED, or its power.

For the power state model, button mapping, deferred actions and the library API, see the
[library README](../../README.md). This document covers only what is specific to this sample.

## Supported board and peripheral devices
* Raspberry Pi Pico / Pico 2
* SSD1306 OLED display, 128x64 pixels, I2C address `0x3c`
* Single-cell Li-ion / Li-po battery with USB charging

## Pin assignments
Application-side GPIOs used by this sample, on top of the library / PCB pins (see the library's
[GPIO assignments](../../README.md#gpio-assignments-for-rp2)).

| Signal | GPIO | Direction | Note |
|--------|------|-----------|------|
| Display power | 14 | out (push-pull) | display power enable, high = on |
| I2C0 SDA      | 12 | I2C | SSD1306 data |
| I2C0 SCL      | 13 | I2C | SSD1306 clock |
| On-board LED  | `PICO_DEFAULT_LED_PIN` | out | activity blink while running |

This sample does not wire a user switch.

## Hardware / schematic
This sample requires the mandatory external discrete power circuit assumed by the library
(external DC/DC EN control, battery voltage divider, USB detect, display power switch).

* Optimized version with SMD devices - [doc/battery_op_with_ssd1306_schematic.pdf](doc/battery_op_with_ssd1306_schematic.pdf)

## Display power (application-owned)
Display power is controlled by the application on **GPIO14** (push-pull output, **high = on**). The
SSD1306 runs under it. The application turns display power on at startup and after waking from
dormant, and off together with the display before the library goes dormant.

## Display screens
| State | Display |
|-------|---------|
| `PboStateActive` | title `Battery Op. Demo`, the power source (`USB Power` / `Battery Power`) and its voltage, and an uptime clock. During an announce phase the bottom area blinks `GO DORMANT` / `SHUTDOWN` / `LOW BATTERY`. |
| `PboStateIdle` (USB present) | blinking `Charging`. |

The on-board LED blinks while running (`PboStateActive` with no pending announce); it is off otherwise.

Button behavior follows the library's [default power-button mapping](../../README.md#power-button-mapping)
(double push -> Sleep, long-long push -> Shutdown), with 3 s announce windows; a single push during a
cancelable announce cancels it.

## How the application integrates with the library
The sample registers three callbacks and drives `pbo_process()` each loop (see [main.cpp](main.cpp)):

| Callback | Application action |
|---|---|
| `on_button_event` | power single push -> `pbo_cancel_deferred()` (abort a pending cancelable announce) |
| `on_enter_dormant` | `display_deinit()`, display power **off**, then `pbo_dormant_set_low_leakage(0)` to lower dormant current |
| `on_exit_dormant` | re-init the released pins, display power **on**, then `display_init()` after waking |

```c
display_power_init();
pbo_config_t config = pbo_get_default_config();
config.sleep_defer_ms    = 3000;               // 3 s announce windows (library default is 0)
config.shutdown_defer_ms = 3000;
config.charge_defer_ms   = 3000;
config.callbacks.on_button_event  = on_button_event;
config.callbacks.on_enter_dormant = on_enter_dormant;
config.callbacks.on_exit_dormant  = on_exit_dormant;
pbo_init(&config);
pbo_start();
set_display_power(true);
sleep_ms(100);                                 // wait for the SSD1306 power to stabilize
display_init();
while (true) {
    pbo_process();
    // render SSD1306 from pbo_get_state() / pbo_get_deferred()
    sleep_ms(100);
}
```

### Dormant low-leakage
`on_enter_dormant()` calls `pbo_dormant_set_low_leakage(0)` to put every GPIO except the library's
reserved pins into the lowest-leakage state, minimizing current while dormant. The sweep is
destructive, so `on_exit_dormant()` re-initializes the application pins it let go (LED, display
power, I2C) and calls `display_init()` to re-allocate the display buffer and re-configure the panel.
See [Low-power (dormant) tuning](../../README.md#low-power-dormant-tuning) in the library README.

## How to build
The output binary is `battery_op_with_ssd1306.uf2`. Using the Docker build (no local SDK needed):
```
$ cd samples/battery_op_with_ssd1306
$ ../build_docker.sh          # both targets -> build/ , build2/
$ ../build_docker.sh pico     # rp2040 only  -> build/battery_op_with_ssd1306.uf2
$ ../build_docker.sh pico2    # rp2350 only  -> build2/battery_op_with_ssd1306.uf2
```
For local SDK builds and full details, see the repository README:
[How to build](../../README.md#how-to-build-with-docker-image).
