#include "ecw.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#define IO DT_PATH(zephyr_user)
static const struct gpio_dt_spec hold = GPIO_DT_SPEC_GET(IO, power_hold_gpios);
static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET(IO, red_gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(IO, green_gpios);
static const struct adc_dt_spec battery = ADC_DT_SPEC_GET(IO);
static bool battery_ready;

enum led_flag {
    LED_ERROR = BIT(0),
    LED_DISCONNECTED = BIT(1),
    LED_CONNECTED = BIT(2),
    LED_OFF = BIT(3),
    LED_ADVERTISING = BIT(4),
    LED_SHUTDOWN = BIT(5),
};
static atomic_t led_flags;
static int64_t led_boot_ms;
static atomic_t shutdown_ms;
static void led_update(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_work, led_update);

static void led_set_flags(atomic_val_t mask, atomic_val_t value)
{
    atomic_val_t previous;

    do {
        previous = atomic_get(&led_flags);
    } while (!atomic_cas(&led_flags, previous, (previous & ~mask) | value));
}

static void led_update(struct k_work *work)
{
    ARG_UNUSED(work);
    atomic_val_t flags = atomic_get(&led_flags);
    if (flags & LED_OFF) { return; }
    int64_t elapsed = k_uptime_get() - led_boot_ms;
    bool red_on = (flags & LED_ERROR) != 0;
    /* Waiting for the phone blinks from boot; a live link stays solid through
     * measurement. Shutdown overrides errors and both connection states. */
    bool green_on = (flags & LED_CONNECTED) ||
                    (!(flags & LED_CONNECTED) && elapsed % 1000 < 500);
    if (flags & LED_SHUTDOWN) {
        uint32_t elapsed_shutdown = k_uptime_get_32() - (uint32_t)atomic_get(&shutdown_ms);
        red_on = elapsed_shutdown % 500 < 250;
        green_on = false;
    }

    gpio_pin_set_dt(&red, red_on);
    gpio_pin_set_dt(&green, green_on);
    /* GPIO updates run outside BLE callbacks and the measurement loop. */
    k_work_reschedule(&led_work, K_MSEC(100));
}

int board_io_init(void)
{
    if (!gpio_is_ready_dt(&hold) || !gpio_is_ready_dt(&red) ||
        !gpio_is_ready_dt(&green)) { return -ENODEV; }
    /* Assert PIN_POWER_ON through D1 before AFE/BLE initialization so EN stays
     * high after the user releases the start button. */
    int rc = gpio_pin_configure_dt(&hold, IS_ENABLED(CONFIG_ECW_POWER_HOLD) ?
                                  GPIO_OUTPUT_ACTIVE : GPIO_INPUT);
    if (rc) { return rc; }
    rc = gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE);
    if (rc) { return rc; }
    rc = gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);
    if (rc) { return rc; }
    led_boot_ms = k_uptime_get();
    atomic_clear(&led_flags);
    k_work_schedule(&led_work, K_NO_WAIT);
    battery_ready = adc_is_ready_dt(&battery) && adc_channel_setup_dt(&battery) == 0;
    return 0;
}
void board_led_error(bool error)
{
    led_set_flags(LED_ERROR, error ? LED_ERROR : 0);
}
void board_led_connected(bool connected)
{
    /* Only BLE connection/disconnection callbacks change this state.
     * The initial zero state means that no connection has occurred yet. */
    led_set_flags(LED_CONNECTED | LED_DISCONNECTED | LED_ADVERTISING,
                  connected ? LED_CONNECTED : LED_DISCONNECTED);
}
void board_led_advertising(bool advertising)
{
    led_set_flags(LED_ADVERTISING, advertising ? LED_ADVERTISING : 0);
}
void board_led_shutdown(void)
{
    atomic_set(&shutdown_ms, (atomic_val_t)k_uptime_get_32());
    led_set_flags(LED_CONNECTED | LED_ADVERTISING | LED_SHUTDOWN, LED_SHUTDOWN);
    k_work_reschedule(&led_work, K_NO_WAIT);
}
int board_power_off(void)
{
    /* Called from the command thread after measurement and BLE have stopped. */
    struct k_work_sync sync;
    if (atomic_get(&led_flags) & LED_SHUTDOWN) {
        uint32_t elapsed = k_uptime_get_32() - (uint32_t)atomic_get(&shutdown_ms);
        if (elapsed < 3000) { k_msleep(3000 - elapsed); }
    }

    /* A running handler may reschedule itself while cancellation waits.
     * Any subsequent run must leave both LEDs off and stop rescheduling. */
    atomic_or(&led_flags, LED_OFF);
    k_work_cancel_delayable_sync(&led_work, &sync);
    gpio_pin_set_dt(&red, 0);
    gpio_pin_set_dt(&green, 0);
    /* D1 isolates this pin from EN. Actual power removal depends on EN
     * discharging and on SW1 being released, with no external supply. */
    int rc = gpio_pin_set_dt(&hold, 0);
    if (rc) {
        atomic_set(&led_flags, LED_ERROR | LED_DISCONNECTED);
        k_work_schedule(&led_work, K_NO_WAIT);
    }
    return rc;
}
int board_battery_mv(void)
{
    int16_t raw;
    struct adc_sequence seq = { .buffer = &raw, .buffer_size = sizeof(raw) };
    if (!battery_ready) { return -ENODEV; }
    int rc = adc_sequence_init_dt(&battery, &seq);
    if (rc) { return rc; }
    rc = adc_read_dt(&battery, &seq);
    if (rc) { return rc; }
    int32_t mv = raw;
    rc = adc_raw_to_millivolts_dt(&battery, &mv);
    /* R9=820k (top), R10=1M (bottom): VBAT = Vadc * 1.82. */
    return rc ? rc : (mv * 182 + 50) / 100;
}

