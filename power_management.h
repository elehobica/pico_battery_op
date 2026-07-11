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
//   PmStateIdle   : power-keep latch released. Boot boundary and shutdown target.
//              With USB present it charges (dormant); without USB the hardware
//              powers off. Not a "running" state (the CPU is dormant/off here).
//   PmStateActive : latch held, fully running.
//   PmStateSleep  : latch held, dormant battery nap; wakes by Power switch -> Active.
typedef enum _pm_state_t {
    PmStateIdle = 0,
    PmStateActive,
    PmStateSleep
} pm_state_t;

// A "pending transition" is an announce/grace phase before a terminal power
// action is committed. The application paces it (renders the announcement) and
// may cancel it when cancelable. It auto-commits when its deadline elapses.
typedef enum _pm_pending_reason_t {
    PmPendingNone = 0,
    PmPendingSleep,       // PmStateActive -> dormant       (announce e.g. "GO DORMANT"),  cancelable
    PmPendingShutdown,    // PmStateActive -> PmStateIdle         (announce e.g. "SHUTDOWN"),    cancelable
    PmPendingLowBattery,  // PmStateActive -> PmStateIdle         (announce e.g. "LOW BATTERY"), NOT cancelable
    PmPendingCharge       // PmStateIdle(USB) -> dormant     (announce e.g. "Charging"),    NOT cancelable
} pm_pending_reason_t;

typedef struct _pm_pending_info_t {
    pm_pending_reason_t reason;
    uint32_t            remaining_ms; // until auto-commit (for countdown / blink)
    bool                cancelable;
} pm_pending_info_t;

// Announce durations for the pending phases. Pass to pm_start() (NULL = defaults).
typedef struct _pm_config_t {
    uint32_t sleep_announce_ms;    // "GO DORMANT" before dormant
    uint32_t shutdown_announce_ms; // "SHUTDOWN" / "LOW BATTERY" before releasing latch
    uint32_t charge_announce_ms;   // "Charging" before dormant in PmStateIdle
} pm_config_t;

// Application callbacks invoked by the power state machine.
// All members are optional (set to NULL to skip). They are called from
// pm_process() context (main-loop), never from an ISR.
typedef struct _pm_callbacks_t {
    // Called once right after the state machine transitions to a new state.
    void (*on_state_changed)(pm_state_t new_state, pm_state_t prev_state);
    // Called when a pending (announce) phase begins.
    void (*on_pending)(pm_pending_reason_t reason);
    // Called for button events not consumed by the state machine as a trigger
    // (e.g. ButtonUserSingle). While a pending is active, all button events are
    // forwarded here so the application can decide to pm_cancel() it.
    void (*on_button_event)(button_action_t btn_act);
    // Called just before the library enters dormant, so the application can
    // quiesce its own peripherals (e.g. display_deinit(), peripheral power off).
    void (*on_before_dormant)();
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
// Register callbacks and announce-duration config, then select the initial
// state from USB-plugged detection. Call once after pm_init().
void pm_start(const pm_callbacks_t* callbacks, const pm_config_t* config);
// Advance the power state machine. Call it periodically from the main loop
// (timing uses absolute time, so the exact cadence is not critical). This may
// block while dormant (PmStateSleep / charging in PmStateIdle).
void pm_process();
// Current power state.
pm_state_t pm_get_state();
// Fill *out with the active pending info; returns false if none is pending.
bool pm_get_pending(pm_pending_info_t* out);
// Cancel the current pending if it is cancelable (no effect otherwise).
void pm_cancel();
// Milliseconds elapsed since the current state was entered (for blink timing).
uint32_t pm_get_state_elapsed_ms();

#ifdef __cplusplus
}
#endif
