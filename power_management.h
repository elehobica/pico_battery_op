/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _button_status_t {
    ButtonOpen = 0,
    ButtonPower,
    ButtonUser
} button_status_t;

typedef enum {
    ButtonPowerSingle = 0,
    ButtonPowerDouble,
    ButtonPowerTriple,
    ButtonPowerLong,
    ButtonPowerLongLong,
    ButtonUserSingle,
    ButtonUserDouble,
    ButtonUserTriple,
    ButtonUserLong,
    ButtonUserLongLong,
    ButtonOthers
} button_action_t;

// using struct as an example, but primitive types can be used too
typedef struct element {
    button_action_t button_action;
} element_t;

// Power state managed by the power management state machine (pm_process()).
typedef enum _pm_state_t {
    PmStateNormal = 0,
    PmStateDeepSleep,
    PmStateShutdown,
    PmStateCharge
} pm_state_t;

// Application callbacks invoked by the power state machine.
// All members are optional (set to NULL to skip). They are called from
// pm_process() context (main-loop), never from an ISR.
typedef struct _pm_callbacks_t {
    // Called once right after the state machine transitions to a new state.
    void (*on_state_changed)(pm_state_t new_state, pm_state_t prev_state);
    // Called for button events that the power state machine does not consume
    // itself (e.g. ButtonUserSingle). Power gestures such as ButtonPowerSingle
    // / ButtonPowerLongLong are handled internally and not forwarded.
    void (*on_button_event)(button_action_t btn_act);
    // Called just before the library turns off peripheral power and enters
    // dormant, so the application can quiesce its own peripherals
    // (e.g. display_deinit()).
    void (*on_before_dormant)();
} pm_callbacks_t;

void pm_init();
void pm_set_power_keep(bool value);
bool pm_get_low_battery();
uint16_t pm_get_battery_voltage();
void pm_set_peripheral_power(bool value);
bool pm_get_peripheral_power();
bool pm_get_power_sw();
bool pm_usb_power_detected();
void pm_enter_dormant_and_wake();
void pm_reboot();
bool pm_is_caused_reboot();
bool pm_get_btn_evt(button_action_t* btn_act);
void pm_clear_btn_evt();

// === Power state machine ===
// Register callbacks and select the initial state from USB-plugged detection.
// Call once after pm_init().
void pm_start(const pm_callbacks_t* callbacks);
// Advance the power state machine by one tick. Call it periodically from the
// main loop (assumes an ~100 ms cadence for its dwell timing). This may block
// while dormant in DeepSleep / Charge states.
void pm_process();
// Current power state.
pm_state_t pm_get_state();
// Number of pm_process() ticks spent in the current state (useful for blink
// timing on the application side).
uint32_t pm_get_state_count();

#ifdef __cplusplus
}
#endif
