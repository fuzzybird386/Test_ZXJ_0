#pragma once
#ifndef LED_H__
#define LED_H__

#include <stdint.h>
#include <stdbool.h>

/* Active-low GPIO LEDs. Boot: green blinking. Connected: green steady.
 * Disconnect: red blinking until the connection module powers off at 4 seconds. */
int led_init(void);

/* Red LED — GPIO. */
void led_red_set(bool on);
void led_red_blink(uint8_t times, uint16_t period_ms);

/* Cancel all patterns and go to disconnected-idle state. */
void led_off_all(void);

/* Green LED state machine — call on BLE connect/disconnect. */
void led_set_ble_connected(bool connected);

/* Green LED state machine — call when EC method starts/stops. */
void led_set_ec_running(bool running);

/* Pre-baked patterns. */
void led_pattern_boot(void);          /* pattern starts in led_init */
void led_pattern_error(bool latch);   /* red on (latched) or blink               */

#endif /* LED_H__ */
