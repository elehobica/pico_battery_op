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

// Button gesture events reported to the application via on_button_event().
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

// Power state managed by the power management state machine (pm_process()).
//   PmStateIdle   : power-keep latch released. Boot boundary and shutdown target.
//                   With USB it charges (dormant); without USB the hardware
//                   powers off. Not a "running" state.
//   PmStateActive : latch held, running. A battery nap is a dormant episode that
//                   stays in PmStateActive; there is no separate "sleep" state.
typedef enum _pm_state_t {
    PmStateIdle = 0,
    PmStateActive
} pm_state_t;

// A "deferred action" is a power action scheduled now and run automatically
// after a grace delay, unless it is canceled (cancelable ones only) with
// pm_cancel_deferred() before its deadline.
typedef enum _pm_deferred_reason_t {
    PmDeferredNone = 0,
    PmDeferredSleep,        // PmStateActive -> dormant nap    (cancelable)
    PmDeferredShutdown,     // PmStateActive -> PmStateIdle     (cancelable)
    PmDeferredLowBattery,   // PmStateActive -> PmStateIdle     (NOT cancelable)
    PmDeferredCharge        // PmStateIdle(USB) -> dormant      (NOT cancelable)
} pm_deferred_reason_t;

typedef struct _pm_deferred_info_t {
    pm_deferred_reason_t reason;
    uint32_t           remaining_ms; // until it runs (for countdown / blink)
    bool               cancelable;
} pm_deferred_info_t;

// Grace delays for deferred actions. Pass to pm_start() (NULL = defaults, 3000 ms each).
typedef struct _pm_config_t {
    uint32_t sleep_defer_ms;    // before dormant nap
    uint32_t shutdown_defer_ms; // before releasing latch (shutdown / low battery)
    uint32_t charge_defer_ms;   // before dormant while charging
} pm_config_t;

// Application callbacks invoked by the power state machine.
// All members are optional (set to NULL to skip). They are called from
// pm_process() context (main-loop), never from an ISR.
typedef struct _pm_callbacks_t {
    // A state transition occurred (PmStateIdle <-> PmStateActive). Note: a
    // battery nap stays in PmStateActive, so it does NOT fire this callback.
    void (*on_state_changed)(pm_state_t new_state, pm_state_t prev_state);
    // A deferred action was scheduled (its grace delay began).
    void (*on_deferred)(pm_deferred_reason_t reason);
    // Button events not consumed by the state machine as a power trigger
    // (e.g. ButtonUserSingle). While a deferred action is pending, all button
    // events are forwarded here so the app can decide to pm_cancel_deferred() it.
    void (*on_button_event)(button_action_t btn_act);
    // Just before entering dormant (both nap and charging dormant); the app
    // quiesces its peripherals (e.g. display_deinit(), peripheral power off).
    void (*on_enter_dormant)();
    // Just after waking from dormant. The state is already PmStateActive.
    void (*on_exit_dormant)();
} pm_callbacks_t;

void pm_init();
void pm_set_power_keep(bool value);
bool pm_get_low_battery();
float pm_get_battery_voltage();
bool pm_get_power_sw();
bool pm_usb_power_detected();
void pm_reboot();
bool pm_is_caused_reboot();

// === Power state machine ===
// Register callbacks and grace-delay config, then select the initial state
// from USB-plugged detection. Call once after pm_init().
void pm_start(const pm_callbacks_t* callbacks, const pm_config_t* config);
// Advance the power state machine. Call it periodically from the main loop
// (timing uses absolute time, so the exact cadence is not critical). This may
// block while dormant (battery nap, or charging in PmStateIdle).
void pm_process();
// Current power state.
pm_state_t pm_get_state();
// Fill *out with the pending deferred action; returns false if none is pending.
bool pm_get_deferred(pm_deferred_info_t* out);
// Cancel the pending deferred action if it is cancelable. Returns true if one
// was actually canceled, false otherwise (none pending, or not cancelable).
bool pm_cancel_deferred();
// Milliseconds elapsed since the current state was entered (for blink timing).
uint32_t pm_get_state_elapsed_ms();

#ifdef __cplusplus
}
#endif
