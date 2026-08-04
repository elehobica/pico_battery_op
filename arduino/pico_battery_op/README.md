# pico_battery_op (Arduino library)

Arduino port of the [pico_battery_op](https://github.com/elehobica/pico_battery_op) power-management
library for battery-operated Raspberry Pi Pico / Pico 2.

The library API and the power state model are documented in the
[main repository README](https://github.com/elehobica/pico_battery_op#readme). This file only covers
what is specific to the Arduino build.

## Requirements

- **[arduino-pico](https://github.com/earlephilhower/arduino-pico) core 5.7.0 or later.**
  The library uses `pico_low_power`, which needs Pico SDK 2.3.0; arduino-pico adopted that SDK in
  5.7.0. Older cores will fail to build.
- The **mandatory external power circuit** described in the main repository. Without it the power
  state machine cannot work.

## Generated sources (when working from a clone of this repository)

`src/pico_battery_op.h` and `src/pico_battery_op.cpp` are **exact copies** of the files in the
repository root, which holds the single source of truth. They are therefore not tracked by git.
After cloning (or after editing the originals), generate them with the script at the repository
root:

```
$ ./copy_to_arduino.sh
```

The vendored pico-extras sources under `src/pbo_vendor/` are patched copies and *are* tracked, so
they need no regeneration.

## Installation

This library is not yet in the Arduino Library Manager index. Install it manually:

- copy the `pico_battery_op` folder into your sketchbook `libraries/` folder, or
- zip the folder and use `Sketch > Include Library > Add .ZIP Library...`

Then open `File > Examples > pico_battery_op > battery_op_basic`.

## Vendored sources

The Arduino core does not ship **pico-extras**, which provides the RP2 dormant implementation this
library depends on (`sleep_run_from_xosc()`, `sleep_goto_dormant_until_pin()`, `sleep_power_up()`).
Those sources are therefore vendored into `src/`:

| File in `src/pbo_vendor/` | Origin |
|---|---|
| `pbo_sleep.c` | pico-extras `sdk-2.3.0` `src/rp2_common/pico_sleep/sleep.c` |
| `pbo_sleep.h` | pico-extras `sdk-2.3.0` `src/rp2_common/pico_sleep/include/pico/sleep.h` |
| `pbo_rosc.h`  | pico-sdk `2.3.0` `src/rp2_common/hardware_rosc/include/hardware/rosc.h` |

They keep their original copyright headers and are BSD-3-Clause (Raspberry Pi (Trading) Ltd.),
compatible with this project's BSD-2-Clause.

**Why `pbo_rosc.h`.** `pbo_sleep.c` calls `rosc_disable()` / `rosc_set_dormant()` / `rosc_restart()`.
Their object code is linked (via `pico_low_power` -> `hardware_rosc`), but the Arduino core does
**not** put `hardware_rosc` on the compile include path, so `hardware/rosc.h` is not found. `pbo_rosc.h`
is a verbatim copy of that header supplying the declarations; the functions keep their real names
because they resolve to the single core implementation (they are not our code, so there is nothing to
duplicate). The unrelated `hardware_rosc_extra` from pico-extras is **not** vendored: nothing in
`pbo_sleep.*` uses it.

**Namespacing.** Arduino links all libraries of a sketch together, so two libraries bundling the
same pico-extras sources would collide with `multiple definition` errors, and a shared
`pico/sleep.h` include path could resolve to the wrong copy. To prevent both, the vendored files
are kept under the unique `pbo_vendor/` folder (never on a shared include path) and **every global
function that `pbo_sleep.*` defines is prefixed with `pbov_`** (for example `sleep_power_up` ->
`pbov_sleep_power_up`), with the include guard renamed too. `pico_battery_op.cpp` maps the prefixed
names back to the SDK names locally, so this library can safely coexist with other RP2040 sleep /
dormant libraries. (The `rosc_*` declarations in `pbo_rosc.h` are intentionally *not* prefixed, as
noted above.)

Everything else these files need is already provided by the core and confirmed present on the
include path: `hardware_clocks`, `hardware_irq`, `hardware_gpio`, `hardware_xosc`, `hardware_pll`,
`hardware_sync`, `pico_runtime_init`, `pico_platform`, `pico_aon_timer`, `pico_low_power`,
`hardware_structs`, `hardware_regs`, and `hardware_powman` (RP2350 only).

## Serial / USB behavior

Under the Arduino core the USB CDC stack is owned by the core, and `pico_stdio_usb` /
`pico_stdio_uart` are not linked. The library therefore skips its own stdio init/deinit when
`ARDUINO` is defined; use the core's `Serial` object as usual.

For the same reason, `setup_default_uart()` (called by the vendored `pbo_sleep.c` after clock
changes) is not linked either, so those two calls are guarded with `#if !defined(ARDUINO)`. This is
the only local patch to the vendored pico-extras code beyond the `pbov_` renaming.

Note that dormant mode stops the clocks, so the USB connection drops while the board is in Sleep or
Charging. After waking, the host normally re-enumerates the device, but the serial monitor may need
to be reopened.

## License

BSD-2-Clause (see the main repository), except the vendored pico-extras files noted above, which are
BSD-3-Clause.
