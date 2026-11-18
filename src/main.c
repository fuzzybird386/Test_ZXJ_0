#include <zephyr/kernel.h>
#include "inc/power.h"
#include "inc/led.h"
#include "inc/ble.h"
#include "method/inc/ec_method.h"
#include "method/inc/methods.h"
int main(void) {
 int rc = power_init(); if (rc) return rc;
 rc = led_init(); if (rc) return rc;
 ec_method_init(); methods_register_all();
 rc = ble_init(); if (rc) { led_pattern_error(true); return rc; }
 return 0;
}
