# Raspberry Pi Pico Battery Operation
![Scene1](doc/pico_battery_operation_breadboard.jpg)

## Overview
This is a sample project for battery operation of Raspberry Pi Pico.<br>
External circuit described in Schematic is mandatory to operate this project.

This project supports:
* power state transition for stand-by (RP2040 off), normal operation, deep sleep and shutdown.
* control of peripheral 3.3V power
* battery voltage monitor and low battery shutdown function
* USB plugged detection

## Supported Board and Peripheral Devices
* Raspberry Pi Pico
* SSD1306 OLED display 128x64 pixels
* TP4056 Project (Li-po battery charger)

## Power state description
### Power state transition diagram
![power_state_diagram](doc/power_state_diagram.png)

### Stand-by
* achieve very low power consumption by shutdown of DC/DC converter on Raspberry Pi Pico
* RP2040 and peripheral power are completely set to power-off
* power consumption is under 0.5 mW
* transition to Normal state by power switch long push (power-on by H/W circuit)
* transition to Charge state by detection of USB plugged (power-on by H/W circuit)

### Normal state
* in this project sample, LED is blinking in this state
* toggle peripheral power by user switch single push (in this project OLED display runs under peripheral power)
* detectable for power provided from battery or USB power
* transition to DeepSleep state by user switch long push
* transition to Shutdown state by power switch long push or low battery detected

### DeepSleep state
* achieved by dormant mode served in pico-sdk
* to disable peripheral power minimizes power consumption in DeepSleep state
* power consumption is around 5 mW (under peripheral power off)
* transition to Normal state by power switch single push

### Shutdown state
* goto Charge state in 3 seconds

### Charge state
* When USB plugged, indicate "Charging" display then enter and keep dormant to minimize charge current
* When USB unplugged, return to Stand-by
* transition to Normal state by power switch single push
* Firmware update is also available in this state

## Schematic
* optimized version with SMD devices

[RPi_Pico_battery_operation.pdf](doc/RPi_Pico_battery_operation.pdf)

* Alternative schematic for the case non-SMD devices are desirable for bread board test

[RPi_Pico_battery_operation_breadboard.pdf](doc/RPi_Pico_battery_operation_breadboard.pdf)

### Comments for schematic
* T1 switches battery power to be used only when USB is unplugged. Please refer to "Using a Battery Charger" section of [pico-datasheet.pdf](https://datasheets.raspberrypi.org/pico/pico-datasheet.pdf)
* T2 controls EN signal of DC/DC converter on Raspberry Pi Pico Board. To enable DC/DC converter, EN signal needs to be High by switching T2 off.
  T2 is on in Stand-by and gets off when power switch is pushed or USB is plugged or POWER_KEEP signal gets High.
* While USB unplugged, VBUS voltage can be around 0.8V by battery power through reverse current from Shottky diode on Raspberry Pi Pico Board.
  That is why voltage divider (R4 and R5) is needed at Gate of T4 to keep T4 off while USB unplugged.
* T1 and T5 (P-ch MOSFET) are used as Hi-side switches. To drive Raspberry Pi Pico and peripheral devices in stable, those P-ch MOSFETs should be choosed as low On-Resistance (~0.1ohm).

## How to build
* See ["Getting started with Raspberry Pi Pico"](https://datasheets.raspberrypi.org/pico/getting-started-with-pico.pdf)
* Build is confirmed only in Developer Command Prompt for VS 2019 and Visual Studio Code on Windows enviroment
* Put "pico-sdk", "pico-examples", "pico-extras" and "pico-playground" on the same level with this project folder.
```
> git clone -b master https://github.com/raspberrypi/pico-sdk.git
> cd pico-sdk
> git submodule update -i
> cd ..
> git clone -b master https://github.com/raspberrypi/pico-examples.git
> 
> git clone https://github.com/raspberrypi/pico-extras.git
> cd pico-extras
> git submodule update -i
> cd ..
> git clone https://github.com/raspberrypi/pico-playground.git
> 
> git clone -b main https://github.com/elehobica/pico_battery_op.git
```
* Lanuch "Developer Command Prompt for VS 2019"
```
> cd pico_battery_op
> mkdir build
> cd build
> cmake -G "NMake Makefiles" ..
> nmake
```
* Put "pico_battery_op.uf2" on RPI-RP2 drive
