#pragma once
#ifndef POWER_H__
#define POWER_H__

#include <stdbool.h>

/**
 * Soft power control. The button physically powers the SoC up; this
 * subsystem latches the rail high (PIN_POWER_ON) and releases it on
 * shutdown.
 */

int  power_init(void);                  /* call as early as possible */
void power_latch_on(void);              /* drive PIN_POWER_ON high   */
void power_shutdown(void);              /* release rail (no return)  */
void power_request_reboot(void);        /* sys_reboot()              */

bool power_stim_rail_is_on(void);
int  power_stim_rail_set(bool enable);  /* PIN_STIM_PWR_EN           */

#endif /* POWER_H__ */
