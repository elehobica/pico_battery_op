/*------------------------------------------------------/
/ Copyright (c) 2021, Elehobica
/ Released under the BSD-2-Clause
/ refer to https://opensource.org/licenses/BSD-2-Clause
/------------------------------------------------------*/

#ifndef _POWER_MANAGEMENT_H_
#define _POWER_MANAGEMENT_H_

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
bool pm_get_btn_evt(button_action_t *btn_act);
void pm_clear_btn_evt();

#ifdef __cplusplus
}
#endif

#endif // _POWER_MANAGEMENT_H_