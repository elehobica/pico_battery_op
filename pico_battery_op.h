/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/

#pragma once

#include <stdint.h>
#include <stdbool.h>

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

// Power state managed by the power management state machine (pbo_process()).
//   PboStateIdle   : power-keep latch released. Boot boundary and shutdown target.
//                   With USB it charges (dormant); without USB the hardware
//                   powers off. Not a "running" state.
//   PboStateActive : latch held, running. A battery nap is a dormant episode that
//                   stays in PboStateActive; there is no separate "sleep" state.
typedef enum _pbo_state_t {
    PboStateIdle = 0,
    PboStateActive
} pbo_state_t;

// A "deferred action" is a power action scheduled now and run automatically
// after a delay, unless it is canceled (cancelable ones only) with
// pbo_cancel_deferred() before its deadline.
typedef enum _pbo_deferred_reason_t {
    PboDeferredNone = 0,
    PboDeferredSleep,        // PboStateActive -> dormant nap    (cancelable)
    PboDeferredShutdown,     // PboStateActive -> PboStateIdle     (cancelable)
    PboDeferredLowBattery,   // PboStateActive -> PboStateIdle     (NOT cancelable)
    PboDeferredCharge        // PboStateIdle(USB) -> dormant      (NOT cancelable)
} pbo_deferred_reason_t;

typedef struct _pbo_deferred_info_t {
    pbo_deferred_reason_t reason;
    uint32_t           remaining_ms; // until it runs (for countdown / blink)
    bool               cancelable;
} pbo_deferred_info_t;

// Sentinel for pbo_config_t::pin_user_sw meaning "no user switch wired".
// (GPIO0 therefore cannot be used as the user switch.)
#define PBO_PIN_UNUSED 0u

// Application callbacks invoked by the power state machine.
// All members are optional (set to NULL to skip). They are called from
// pbo_process() context (main-loop), never from an ISR.
typedef struct _pbo_callbacks_t {
    // A state transition occurred (PboStateIdle <-> PboStateActive). Note: a
    // battery nap stays in PboStateActive, so it does NOT fire this callback.
    void (*on_state_changed)(pbo_state_t new_state, pbo_state_t prev_state);
    // A deferred action was scheduled (its delay began).
    void (*on_deferred)(pbo_deferred_reason_t reason);
    // Button events not consumed by the state machine as a power trigger
    // (e.g. ButtonUserSingle). While a deferred action is pending, all button
    // events are forwarded here so the app can decide to pbo_cancel_deferred() it.
    void (*on_button_event)(button_action_t btn_act);
    // Just before entering dormant (both nap and charging dormant); the app
    // quiesces its peripherals (e.g. display_deinit(), peripheral power off).
    void (*on_enter_dormant)();
    // Just after waking from dormant. The state is already PboStateActive.
    void (*on_exit_dormant)();
} pbo_callbacks_t;

// Configuration passed to pbo_init(). Obtain defaults from pbo_get_default_config(),
// override only the members you need, then pass it in (NULL = all defaults).
typedef struct _pbo_config_t {
    // Configurable GPIO pin assignments (other pins are fixed in the .cpp).
    uint32_t pin_power_keep;    // software latch holding the DC/DC enabled (default 27)
    uint32_t pin_power_sw;      // power switch / dormant wake pin (default 28)
    uint32_t pin_user_sw;       // user switch, PBO_PIN_UNUSED if not wired (default PBO_PIN_UNUSED)
    // Delays for deferred actions.
    uint32_t sleep_defer_ms;    // before dormant nap
    uint32_t shutdown_defer_ms; // before releasing latch (shutdown / low battery)
    uint32_t charge_defer_ms;   // before dormant while charging
    // Battery ADC calibration (linear fit):
    //   battery_voltage[V] = adc_pin_voltage * batt_calib_coef_a + batt_calib_coef_b.
    // The ADC pin reads the battery through a 200k/100k divider (nominal ratio 3.0).
    float batt_calib_coef_a;        // scale from ADC pin voltage to battery voltage,
                                    // ideally the divider ratio, trimmed by measurement (default 2.9917)
    float batt_calib_coef_b;        // constant offset added after scaling, compensating divider/ADC bias [V] (default -0.020)
    float low_battery_threshold;    // low-battery latch threshold [V] (default 2.9)
    // Application callbacks (all optional; see pbo_callbacks_t).
    pbo_callbacks_t callbacks;
} pbo_config_t;

// Return a config filled with default pin assignments, delays and
// (NULL) callbacks. Override only the members you need, then pass to pbo_init().
pbo_config_t pbo_get_default_config();
// Initialize the hardware from the given config (NULL = all defaults). Applies
// the pin assignments, so it must run before any other pbo_* call. Call first.
void pbo_init(const pbo_config_t* config);
float pbo_get_battery_voltage();
bool pbo_get_usb_power_detected();
void pbo_reboot();
bool pbo_is_caused_reboot();

// === Power state machine ===
// Start the state machine: select the initial state from USB-plugged detection.
// Call once after pbo_init() (which already took the config and callbacks).
void pbo_start();
// Advance the power state machine. Call it periodically from the main loop
// (timing uses absolute time, so the exact cadence is not critical). This may
// block while dormant (battery nap, or charging in PboStateIdle).
void pbo_process();
// Current power state.
pbo_state_t pbo_get_state();
// Fill *out with the pending deferred action; returns false if none is pending.
bool pbo_get_deferred(pbo_deferred_info_t* out);
// Cancel the pending deferred action if it is cancelable. Returns true if one
// was actually canceled, false otherwise (none pending, or not cancelable).
bool pbo_cancel_deferred();
// Milliseconds elapsed since the current state was entered (for blink timing).
uint32_t pbo_get_state_elapsed_ms();

#ifdef __cplusplus
}
#endif
