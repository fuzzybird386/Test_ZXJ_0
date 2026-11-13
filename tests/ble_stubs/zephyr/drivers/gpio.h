#include <stdbool.h>
#define DT_PATH(x) 0
#define PIN_power_hold_gpios 0
#define PIN_red_gpios 1
#define PIN_green_gpios 2
struct gpio_dt_spec { unsigned pin; };
#define GPIO_DT_SPEC_GET(node,property) {PIN_##property}
#define GPIO_OUTPUT_ACTIVE 1
#define GPIO_OUTPUT_INACTIVE 0
#define GPIO_INPUT 2
extern int test_pins[3];
extern int64_t test_power_cut_at;
static inline bool gpio_is_ready_dt(const struct gpio_dt_spec *p) { (void)p;return true; }
static inline int gpio_pin_set_dt(const struct gpio_dt_spec *p,int v) {
    test_pins[p->pin]=v;if(p->pin==0 && !v)test_power_cut_at=test_now;return 0;
}
static inline int gpio_pin_configure_dt(const struct gpio_dt_spec *p,int mode) { return gpio_pin_set_dt(p,mode==GPIO_OUTPUT_ACTIVE); }
