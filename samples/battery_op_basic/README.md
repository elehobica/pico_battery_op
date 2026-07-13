# battery_op_basic

Minimal reference application for the [pico_battery_op](../../README.md) power-management library.
It uses the library with its **default configuration** and no extra devices: the only output is
the on-board LED blinking while the system is running. There is no display, no callbacks, and no
serial output. Written in plain C ([main.c](main.c)) to show the smallest possible integration.

## Supported board
* Raspberry Pi Pico
* Raspberry Pi Pico 2

## Hardware / schematic
No additional peripheral devices are needed. The mandatory external discrete power circuit assumed
by the library (external DC/DC EN control, battery voltage divider, USB detect) is still required;
see the schematics in the [battery_op_with_ssd1306](../battery_op_with_ssd1306/README.md) sample.
Because the configuration is left at its defaults, the power switch is on **GPIO28** and the
power-keep latch on **GPIO27** (the user switch is unused). See the
[GPIO assignments](../../README.md#gpio-assignments-for-rp2) in the library README.

## Behavior
| State | LED |
|-------|-----|
| `PboStateActive` (running) | blinks at 2 Hz (250 ms on / 250 ms off) |
| dormant (Sleep nap) | off |
| any other state | off |

A Sleep nap stays in `PboStateActive` while the CPU is dormant, so the sample turns the LED off in
the `on_enter_dormant` callback to keep it dark while sleeping, regardless of the blink phase.

The power switch drives the state machine exactly as the library defines (single push starts a
`Sleep` announce then dormant, long push starts a `Shutdown`, low battery latches a shutdown).
This sample does not render any of those announces; it only reflects the running state on the LED.

When a reset is released with USB not connected the board restarts from OFF (Stand-by); press the
power switch to run it. See [Boot / power-on behavior](../../README.md#boot--power-on-behavior).

## How the application integrates with the library
The sample takes the default config and drives `pbo_process()` each loop. Its only callback turns
the LED off before going dormant (see [main.c](main.c)):

```c
pbo_config_t config = pbo_get_default_config();
config.callbacks.on_enter_dormant = on_enter_dormant; // LED off before dormant
pbo_init(&config);
pbo_start();
while (true) {
    pbo_process();
    // blink the on-board LED at 2 Hz while pbo_get_state() == PboStateActive
    sleep_ms(50);
}
```

## How to build
The output binary is `battery_op_basic.uf2`. Using the Docker build (no local SDK needed):
```
$ cd samples/battery_op_basic
$ ../build_docker.sh          # both targets -> build/ , build2/
$ ../build_docker.sh pico     # rp2040 only  -> build/battery_op_basic.uf2
$ ../build_docker.sh pico2    # rp2350 only  -> build2/battery_op_basic.uf2
```
For local SDK builds and full details, see the repository README:
[How to build](../../README.md#how-to-build-with-docker-image).
