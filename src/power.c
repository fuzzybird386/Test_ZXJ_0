#include "inc/power.h"
#include "inc/bsp.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
static const struct device *p0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
int power_init(void) {
 if (!device_is_ready(p0)) return -ENODEV;
 return gpio_pin_configure(p0, PIN_POWER_ON_PIN, GPIO_OUTPUT_HIGH);
}
void power_latch_on(void) { gpio_pin_set(p0, PIN_POWER_ON_PIN, 1); }
void power_shutdown(void) {
 gpio_pin_set(p0, PIN_POWER_ON_PIN, 0);
 sys_poweroff(); /* Also stops execution if SW1 or a debugger keeps VDD alive. */
}
void power_request_reboot(void) { sys_reboot(SYS_REBOOT_COLD); }
bool power_stim_rail_is_on(void) { return false; }
int power_stim_rail_set(bool enable) { return enable ? -ENOTSUP : 0; }
