#include "inc/led.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
static const struct device *p1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static bool connected, disabled, phase, disconnecting;
static void blink(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(blink_work, blink);
static void set(unsigned pin, bool on) { gpio_pin_set(p1, pin, !on); }
static void blink(struct k_work *w) {
 ARG_UNUSED(w);
 if (disabled) return;
 phase = !phase;
 set(7, !disconnecting && (connected || phase));
 if (disconnecting) set(8, phase);
 k_work_reschedule(&blink_work, K_MSEC(250));
}
int led_init(void) {
 if (!device_is_ready(p1)) return -ENODEV;
 int rc = gpio_pin_configure(p1, 7, GPIO_OUTPUT_HIGH);
 if (!rc) rc = gpio_pin_configure(p1, 8, GPIO_OUTPUT_HIGH);
 if (!rc) k_work_reschedule(&blink_work, K_NO_WAIT);
 return rc;
}
void led_red_set(bool on) { if (!disconnecting) set(8, on); }
void led_red_blink(uint8_t times, uint16_t period_ms) { ARG_UNUSED(times); ARG_UNUSED(period_ms); led_red_set(true); }
void led_off_all(void) { disabled = true; k_work_cancel_delayable(&blink_work); set(7,false); set(8,false); }
void led_set_ble_connected(bool value) {
 connected = value;
 if (!value) disconnecting = true;
 phase = false;
 k_work_reschedule(&blink_work, K_NO_WAIT);
}
void led_set_ec_running(bool running) { ARG_UNUSED(running); }
void led_pattern_boot(void) {}
void led_pattern_error(bool latch) { ARG_UNUSED(latch); led_red_set(true); }
