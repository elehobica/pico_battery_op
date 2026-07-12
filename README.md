# pico_battery_op — Power Management Library for Raspberry Pi Pico / Pico 2

[![Build](https://github.com/elehobica/pico_battery_op/actions/workflows/build-binaries.yml/badge.svg)](https://github.com/elehobica/pico_battery_op/actions/workflows/build-binaries.yml)

## Overview
`pico_battery_op` is a power-management library (C API, prefixed `pm_*`) for battery-operated
Raspberry Pi Pico / Pico 2 (RP2040 / RP2350) designs. It implements a compact power state
machine on top of a **mandatory external discrete power circuit**, and provides:

* Power state machine (`Idle` / `Active`) driven by a physical power switch
* RP2 dormant-mode sleep with clock preserve / restore
* Power-keep latch control that holds the external DC/DC enabled
* Battery-voltage monitor with latching low-battery detection
* USB-plugged (charge) detection
* Button gesture recognition (single / double / triple / long / long-long)
* A **deferred action** mechanism (grace delay, auto-run, cancelable) that the application paces
* Callback-based integration, so display / peripherals / product UX stay in the application

The library owns **only** power management. Presentation (OLED, LED), peripheral-power control
and product-specific UX live in the application. See the reference sample
[samples/battery_op_with_ssd1306](samples/battery_op_with_ssd1306/README.md).

## Required external circuit
The library assumes a specific discrete power-management circuit (external DC/DC EN control,
battery voltage divider, USB detect, etc.). It is **mandatory** — many code paths depend on the
wiring. A concrete schematic and a breadboard example are provided with the reference sample:
see [samples/battery_op_with_ssd1306/README.md](samples/battery_op_with_ssd1306/README.md).

### GPIO assignments (library-owned)
| Signal | GPIO | Direction | Note |
|--------|------|-----------|------|
| Power switch      | 28 | in (pull-up) | state transitions + dormant wake (falling edge) |
| User switch       | 17 | in (pull-up) | recognized and forwarded to the app as button events |
| Power-keep latch  | 27 | out          | holds external DC/DC EN |
| USB power detect  | 24 | in           | charge / USB-plugged detection |
| DC/DC PSM control | 23 | out          | PFM (efficiency) / PWM (ripple) mode select |
| Battery level ADC | 29 | ADC3         | via 200k / 100k divider |

Peripheral 3.3 V power control is **not** part of the library; the application owns it
(the sample uses GPIO20, open-drain, active-low). Pin constants live as `static const` in
[power_management.cpp](power_management.cpp).

## Power state model

**Stand-by (hardware)** — the RP2 DC/DC is off and the firmware is not running. The board is
powered on by the external H/W circuit (power-switch long push, or USB plug). In firmware this
condition is represented only at its boundary, as `PmStateIdle`.

### State transition model
![power state model](doc/power_state_model.png)

### States (`pm_state_t`)
| State | power-keep | Meaning |
|-------|-----------|---------|
| `PmStateIdle`   | released | Boot boundary and shutdown target. With USB → charging (dormant); without USB → hardware powers off. Not a running state (CPU is dormant/off). |
| `PmStateActive` | held     | Running. A battery nap is a dormant episode that stays in `PmStateActive` — there is no separate sleep state. |

### Deferred actions (`pm_deferred_reason_t`)
Every "wait, then perform a terminal power action" is modeled as a **deferred action**: scheduled
now and **run automatically** after a grace delay, unless canceled. Delays come from `pm_config_t`
(default 3000 ms each) and are measured with absolute time (no fixed loop-cadence assumption).

| Reason | Trigger | Run action | Cancelable |
|---|---|---|---|
| `PmDeferredSleep`      | power single push (in `Active`)  | dormant nap → `Active` | yes |
| `PmDeferredShutdown`   | power long push (in `Active`)    | release latch → `Idle` | yes |
| `PmDeferredLowBattery` | low battery (in `Active`)        | release latch → `Idle` | no |
| `PmDeferredCharge`     | entering `Idle` with USB present | dormant → `Active` | no |

While a deferred action is pending, the library forwards button events to `on_button_event`, so the
application can call `pm_cancel_deferred()` (e.g. a second power push aborts a `Sleep` / `Shutdown`).

## API

### Lifecycle
| Function | Description |
|---|---|
| `void pm_init()` | Hardware init. Call first. |
| `void pm_start(const pm_callbacks_t* cb, const pm_config_t* cfg)` | Register callbacks and grace-delay config (`cfg = NULL` → defaults). Selects the initial state from USB detection. |
| `void pm_process()` | Advance the state machine. Call periodically from the main loop (may block while dormant). |

### Query / control
| Function | Description |
|---|---|
| `pm_state_t pm_get_state()` | Current state. |
| `bool pm_get_deferred(pm_deferred_info_t* out)` | Pending deferred action (reason / remaining_ms / cancelable); `false` if none. |
| `bool pm_cancel_deferred()` | Cancel the pending deferred action if cancelable; returns whether one was canceled. |
| `uint32_t pm_get_state_elapsed_ms()` | Milliseconds since the current state was entered (blink timing). |
| `float pm_get_battery_voltage()` | Battery voltage in volts. |
| `bool pm_get_low_battery()` | Latches `true` once below threshold. |
| `bool pm_usb_power_detected()` | USB-plugged detection. |
| `void pm_set_power_keep(bool)` | Manual latch control (normally managed by the state machine). |
| `void pm_reboot()` / `bool pm_is_caused_reboot()` | Watchdog reboot helpers. |

### Callbacks (`pm_callbacks_t`, all optional)
| Callback | When | Typical use |
|---|---|---|
| `on_state_changed(new, prev)` | after an `Idle` ↔ `Active` transition (a nap stays `Active`, so it does not fire) | react to entering `Idle` (e.g. persist state before power-off) |
| `on_deferred(reason)` | a deferred action was scheduled (grace delay began) | start rendering the announcement |
| `on_button_event(btn)` | events not consumed as a power trigger (e.g. `ButtonUserSingle`), and all events while a deferred action is pending | product features / call `pm_cancel_deferred()` |
| `on_enter_dormant()` | just before dormant (nap or charging) | quiesce peripherals (display off, peripheral power off) |
| `on_exit_dormant()` | just after waking (state already `Active`) | restore peripherals (peripheral power on) |

All callbacks run in `pm_process()` (main-loop) context — never in an ISR.

## Using the library in your own project
The library is an `INTERFACE` CMake target. From a sample/app `CMakeLists.txt`:
```cmake
add_subdirectory(../.. pico_battery_op)          # this library
target_link_libraries(${PROJECT_NAME} pico_battery_op)
```
Minimal main loop:
```c
pm_init();
pm_start(&callbacks, NULL);          // NULL config -> default grace delays
while (true) {
    pm_process();                    // library runs the power state machine
    // render UI from pm_get_state() / pm_get_deferred()
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
