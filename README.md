# pico_battery_op - Power Management Library for Raspberry Pi Pico / Pico 2

[![Build](https://github.com/elehobica/pico_battery_op/actions/workflows/build-binaries.yml/badge.svg)](https://github.com/elehobica/pico_battery_op/actions/workflows/build-binaries.yml)

## Overview
`pico_battery_op` is a power-management library (C API, prefixed `pbo_*`) for battery-operated
Raspberry Pi Pico / Pico 2 (RP2040 / RP2350) designs. It implements a compact power state
machine on top of a **mandatory external discrete power circuit**, and provides:

* Power state machine (`Idle` / `Active`) driven by a physical power switch
* RP2 dormant-mode sleep with clock preserve / restore
* Power-keep latch control that holds the external DC/DC enabled
* Battery-voltage monitor with latching low-battery detection
* USB-plugged (charge) detection
* Button gesture recognition (single / double / triple / long / long-long)
* A **deferred action** mechanism (delay, auto-run, cancelable) that the application paces
* Callback-based integration, so display / peripherals / product UX stay in the application

The library owns **only** power management. Presentation (OLED, LED), peripheral-power control
and product-specific UX live in the application. See the reference sample
[samples/battery_op_with_ssd1306](samples/battery_op_with_ssd1306/README.md).

## Required external circuit
The library assumes a specific discrete power-management circuit (external DC/DC EN control,
battery voltage divider, USB detect, etc.). It is **mandatory** - many code paths depend on the
wiring. A concrete schematic and a breadboard example are provided with the reference sample:
see [samples/battery_op_with_ssd1306/README.md](samples/battery_op_with_ssd1306/README.md).

### GPIO assignments (library-owned)
| Signal | GPIO (default) | Direction | Note |
|--------|------|-----------|------|
| pin_power_sw         | 28         | in (pull-up) | state transitions + dormant wake (falling edge) |
| pin_power_keep       | 27         | out          | holds external DC/DC EN |
| pin_user_sw          | *unused*   | in (pull-up) | forwarded to the app as button events |
| PIN_USB_POWER_DETECT | 24 (fixed) | in           | charge / USB-plugged detection |
| PIN_DCDC_PSM_CTRL    | 23 (fixed) | out          | DC/DC control PFM (efficiency) / PWM (ripple) mode select |
| PIN_BATT_LVL         | 29 (fixed) | ADC3         | Battery level ADC input via 200k / 100k divider |

The three switch / power-keep pins are configurable at runtime through `pbo_config_t`
(see `pbo_get_default_config()`); the remaining pins are fixed as `static const` in
[pico_battery_op.cpp](pico_battery_op.cpp). Peripheral 3.3 V power control is **not** part of
the library; the application owns it (the sample uses GPIO20, open-drain, active-low).

## Power state model

**Stand-by (hardware)** - the RP2 DC/DC is off and the firmware is not running. The board is
powered on by the external H/W circuit (power-switch long push, or USB plug). In firmware this
condition is represented only at its boundary, as `PboStateIdle`.

### State transition model
![power state model](doc/power_state_model.png)

### States (`pbo_state_t`)
| State | power-keep | Meaning |
|-------|-----------|---------|
| `PboStateIdle`   | released | Boot boundary and shutdown target. With USB → charging (dormant); without USB → hardware powers off. Not a running state (CPU is dormant/off). |
| `PboStateActive` | held     | Running. A battery nap is a dormant episode that stays in `PboStateActive` - there is no separate sleep state. |

### Deferred actions (`pbo_deferred_reason_t`)
Every "wait, then perform a terminal power action" is modeled as a **deferred action**: scheduled
now and **run automatically** after a delay, unless canceled. Delays come from `pbo_config_t`
(default 0 ms, so the action runs on the next `pbo_process()`; set non-zero for an announce window,
as the ssd1306 sample does) and are measured with absolute time (no fixed loop-cadence assumption).

| Reason | Trigger | Run action | Cancelable |
|---|---|---|---|
| `PboDeferredSleep`      | power single push (in `Active`)  | dormant nap → `Active` | yes |
| `PboDeferredShutdown`   | power long push (in `Active`)    | release latch → `Idle` | yes |
| `PboDeferredLowBattery` | low battery (in `Active`)        | release latch → `Idle` | no |
| `PboDeferredCharge`     | entering `Idle` with USB present | dormant → `Active` | no |

While a deferred action is pending, the library forwards button events to `on_button_event`, so the
application can call `pbo_cancel_deferred()` (e.g. a second power push aborts a `Sleep` / `Shutdown`).

## API

### Lifecycle
| Function | Description |
|---|---|
| `pbo_config_t pbo_get_default_config()` | Return a config with default pins, delays and (NULL) callbacks. Override only what you need, then pass to `pbo_init()`. |
| `void pbo_init(const pbo_config_t* cfg)` | Hardware init from `cfg` (pins / delays / callbacks; `cfg = NULL` → defaults). Applies pin assignments, so call it first. |
| `void pbo_start()` | Start the state machine (config and callbacks were already taken by `pbo_init()`); selects the initial state from USB detection. |
| `void pbo_process()` | Advance the state machine. Call periodically from the main loop (may block while dormant). |

### Configuration (`pbo_config_t`)
Obtain a fully-populated struct from `pbo_get_default_config()`, override only the members you need,
then pass it to `pbo_init()` (passing `NULL` uses all defaults).

| Member | Type | Default | Description |
|---|---|---|---|
| `pin_power_keep`    | `uint32_t`       | `27`            | Power-keep latch GPIO - see [GPIO assignments](#gpio-assignments-library-owned). |
| `pin_power_sw`      | `uint32_t`       | `28`            | Power switch / dormant-wake GPIO - see [GPIO assignments](#gpio-assignments-library-owned). |
| `pin_user_sw`       | `uint32_t`       | `PBO_PIN_UNUSED` | User switch GPIO; `PBO_PIN_UNUSED` (0) means not wired (so GPIO0 cannot be the user switch). |
| `sleep_defer_ms`    | `uint32_t`       | `0`             | Delay in milliseconds for `PboDeferredSleep` (0 = run immediately) - see [Deferred actions](#deferred-actions-pbo_deferred_reason_t). |
| `shutdown_defer_ms` | `uint32_t`       | `0`             | Delay in milliseconds for `PboDeferredShutdown` / `PboDeferredLowBattery` (0 = run immediately). |
| `charge_defer_ms`   | `uint32_t`       | `0`             | Delay in milliseconds for `PboDeferredCharge` (0 = run immediately). |
| `batt_calib_coef_a` | `float`          | `2.9917`        | Battery ADC calibration scale in the linear fit `battery_voltage[V] = adc_pin_voltage * batt_calib_coef_a + batt_calib_coef_b`. Ideally the divider ratio (200k/100k → 3.0), trimmed by measurement. |
| `batt_calib_coef_b` | `float`          | `-0.020`        | Battery ADC calibration offset [V] added after scaling, compensating divider/ADC bias (see `batt_calib_coef_a`). |
| `low_battery_threshold` | `float`      | `2.9`           | Battery voltage [V] below which the low-battery flag latches (triggers `PboDeferredLowBattery`). |
| `callbacks`         | `pbo_callbacks_t` | all `NULL`      | Application callbacks - see [Callbacks](#callbacks-pbo_callbacks_t-all-optional). |

### Callbacks (`pbo_callbacks_t`, all optional)
| Callback | When | Typical use |
|---|---|---|
| `on_state_changed(new, prev)` | after an `Idle` ↔ `Active` transition (a nap stays `Active`, so it does not fire) | react to entering `Idle` (e.g. persist state before power-off) |
| `on_deferred(reason)` | a deferred action was scheduled (delay began) | start rendering the announcement |
| `on_button_event(btn)` | events not consumed as a power trigger (e.g. `ButtonUserSingle`), and all events while a deferred action is pending | product features / call `pbo_cancel_deferred()` |
| `on_enter_dormant()` | just before dormant (nap or charging) | quiesce peripherals (display off, peripheral power off) |
| `on_exit_dormant()` | just after waking (state already `Active`) | restore peripherals (peripheral power on) |

All callbacks run in `pbo_process()` (main-loop) context - never in an ISR.

### Query / control
| Function | Description |
|---|---|
| `pbo_state_t pbo_get_state()` | Get current state. |
| `bool pbo_get_deferred(pbo_deferred_info_t* out)` | Get pending deferred action (reason / remaining_ms / cancelable); `false` if none. |
| `bool pbo_cancel_deferred()` | Cancel the pending deferred action if cancelable; returns whether one was canceled. |
| `uint32_t pbo_get_state_elapsed_ms()` | Get milliseconds since the current state was entered (blink timing). |
| `float pbo_get_battery_voltage()` | Get battery voltage in volts. |
| `bool pbo_get_usb_power_detected()` | Get USB power detected. |
| `void pbo_reboot()` / `bool pbo_is_caused_reboot()` | Watchdog reboot helpers. |

## Using the library in your own project
The library is an `INTERFACE` CMake target. From a sample/app `CMakeLists.txt`:
```cmake
add_subdirectory(../.. pico_battery_op)          # this library
target_link_libraries(${PROJECT_NAME} pico_battery_op)
```
Minimal main loop:
```c
pbo_config_t config = pbo_get_default_config();
config.pin_user_sw = 17;             // override only what your board needs
config.callbacks.on_button_event = on_button_event;
pbo_init(&config);                    // NULL -> all defaults
pbo_start();
while (true) {
    pbo_process();                    // library runs the power state machine
    // render UI from pbo_get_state() / pbo_get_deferred()
    sleep_ms(100);
}
```
See [samples/battery_op_with_ssd1306](samples/battery_op_with_ssd1306/README.md) for a complete example.

## How to build with docker image
* Builds the firmware inside [pico-sdk-dev-docker:sdk-2.1.1-1.0.0](https://hub.docker.com/r/elehobica/pico-sdk-dev-docker) (same image used by CI). Requires Docker; no local Pico SDK setup is needed.
* `samples/build_docker.sh` drives the container build. The sample to build is taken from the current directory, so run it from inside the sample folder you want to build (`samples/xxxx`).
```
$ git clone -b main https://github.com/elehobica/pico_battery_op.git
$ cd pico_battery_op/samples/xxxx
$ ../build_docker.sh           # build both targets (default)
$ ../build_docker.sh pico      # build only Pico / Pico W      -> build/xxxx.uf2
$ ../build_docker.sh pico2     # build only Pico 2 / Pico 2 W  -> build2/xxxx.uf2
```
* Outputs: `build/xxxx.uf2` (Pico / Pico W), `build2/xxxx.uf2` (Pico 2 / Pico 2 W)
* Download "*.uf2" on RPI-RP2 or RP2350 drive

## How to build in local
* See ["Getting started with Raspberry Pi Pico"](https://datasheets.raspberrypi.org/pico/getting-started-with-pico.pdf)
* Put "pico-sdk", "pico-examples" and "pico-extras" on the same level with this project folder.
* Set environmental variables for PICO_SDK_PATH, PICO_EXTRAS_PATH and PICO_EXAMPLES_PATH
* Confirmed with Pico SDK 2.1.1
```
> git clone -b 2.1.1 https://github.com/raspberrypi/pico-sdk.git
> cd pico-sdk
> git submodule update -i
> cd ..
> git clone -b sdk-2.1.1 https://github.com/raspberrypi/pico-examples.git
>
> git clone -b sdk-2.1.1 https://github.com/raspberrypi/pico-extras.git
> 
> git clone -b main https://github.com/elehobica/pico_battery_op.git
```
### Windows
* Build is confirmed in Developer Command Prompt for VS 2022 and Visual Studio Code on Windows environment
* Confirmed with cmake-3.27.2-windows-x86_64 and gcc-arm-none-eabi-10.3-2021.10-win32
* Lanuch "Developer Command Prompt for VS 2022"
```
> cd pico_battery_op\samples\xxxx
> mkdir build && cd build
> cmake -G "NMake Makefiles" ..  ; (for Raspberry Pi Pico 1 series)
> cmake -G "NMake Makefiles" -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..  ; (for Raspberry Pi Pico 2)
> nmake
```
* Put "*.uf2" on RPI-RP2 or RP2350 drive
### Linux
* Build is confirmed with [pico-sdk-dev-docker:sdk-2.1.1-1.0.0]( https://hub.docker.com/r/elehobica/pico-sdk-dev-docker)
* Confirmed with cmake-3.22.1 and arm-none-eabi-gcc (15:10.3-2021.07-4) 10.3.1
```
$ cd pico_battery_op/samples/xxxx
$ mkdir build && cd build
$ cmake ..  # (for Raspberry Pi Pico 1 series)
$ cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..  # (for Raspberry Pi Pico 2)
$ make -j4
```
* Download "*.uf2" on RPI-RP2 or RP2350 drive
