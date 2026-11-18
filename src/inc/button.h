#pragma once

/** Callback type for button press events. */
typedef void (*button_press_cb_t)(void);

int button_init(void);

/** Register callback for a short press (press < 2 s). */
void button_set_short_press_cb(button_press_cb_t cb);

/** Register callback for a long press (press ≥ 2 s). */
void button_set_long_press_cb(button_press_cb_t cb);
