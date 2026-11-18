#include "inc/button.h"
#include "inc/bsp.h"
#include "inc/power.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(button, LOG_LEVEL_INF);

#define DEBOUNCE_MS    50u
#define LONG_PRESS_MS  2000u

static const struct device *button_gpio;
static struct gpio_callback button_cb;

static struct k_work_delayable long_press_work;
static struct k_work           short_press_work;

static volatile uint32_t press_start_ms;
static volatile bool     long_press_fired;

static button_press_cb_t s_short_cb;
static button_press_cb_t s_long_cb;

void button_set_short_press_cb(button_press_cb_t cb) { s_short_cb = cb; }
void button_set_long_press_cb(button_press_cb_t cb)  { s_long_cb  = cb; }

static void long_press_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    long_press_fired = true;
    LOG_INF("Long press → shutdown");
    if (s_long_cb) {
        s_long_cb();
    } else {
        power_shutdown();
    }
}

static void short_press_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    LOG_INF("Short press → reboot");
    if (s_short_cb) {
        s_short_cb();
    } else {
        power_request_reboot();
    }
}

static void button_irq_handler(const struct device *dev, struct gpio_callback *cb,
                                uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    if ((pins & BIT(PIN_BUTTON_HOME_PIN)) == 0u) {
        return;
    }

    int val = gpio_pin_get(button_gpio, PIN_BUTTON_HOME_PIN);
    if (val > 0) {
        long_press_fired = false;
        press_start_ms   = k_uptime_get_32();
        k_work_reschedule(&long_press_work, K_MSEC(LONG_PRESS_MS));
    } else {
        (void)k_work_cancel_delayable(&long_press_work);
        if (!long_press_fired) {
            uint32_t held_ms = k_uptime_get_32() - press_start_ms;
            if (held_ms >= DEBOUNCE_MS) {
                k_work_submit(&short_press_work);
            }
        }
    }
}

int button_init(void)
{
    int err;

    button_gpio = DEVICE_DT_GET(DT_NODELABEL(gpio1));
    if (!device_is_ready(button_gpio)) {
        return -ENODEV;
    }

    k_work_init_delayable(&long_press_work, long_press_work_handler);
    k_work_init(&short_press_work, short_press_work_handler);

    err = gpio_pin_configure(button_gpio, PIN_BUTTON_HOME_PIN,
                             GPIO_INPUT | GPIO_PULL_DOWN | GPIO_ACTIVE_HIGH);
    if (err) return err;

    err = gpio_pin_interrupt_configure(button_gpio, PIN_BUTTON_HOME_PIN,
                                       GPIO_INT_EDGE_BOTH);
    if (err) return err;

    gpio_init_callback(&button_cb, button_irq_handler, BIT(PIN_BUTTON_HOME_PIN));
    err = gpio_add_callback(button_gpio, &button_cb);
    if (err) return err;

    LOG_INF("Button ready: P%u.%u", PIN_BUTTON_HOME_PORT, PIN_BUTTON_HOME_PIN);
    return 0;
}
