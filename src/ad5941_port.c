#include "ecw.h"
#include "ad5940.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#define IO DT_PATH(zephyr_user)
static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(spi00));
static const struct gpio_dt_spec cs = GPIO_DT_SPEC_GET(IO, afe_cs_gpios);
static const struct gpio_dt_spec reset = GPIO_DT_SPEC_GET(IO, afe_reset_gpios);
static const struct gpio_dt_spec irq = GPIO_DT_SPEC_GET(IO, afe_int_gpios);
static const struct spi_config config = {
    .frequency = 2000000, .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
};
static int io_error;
static void remember(int rc) { if (rc < 0 && !io_error) { io_error = rc; } }
int afe_port_error(void) { return io_error; }
void afe_port_clear_error(void) { io_error = 0; }
int afe_port_init(void)
{
    if (!device_is_ready(bus) || !gpio_is_ready_dt(&cs) ||
        !gpio_is_ready_dt(&reset) || !gpio_is_ready_dt(&irq)) { return -ENODEV; }
    int rc = gpio_pin_configure_dt(&cs, GPIO_OUTPUT_INACTIVE);
    if (rc) { return rc; }
    rc = gpio_pin_configure_dt(&reset, GPIO_OUTPUT_INACTIVE);
    if (rc) { return rc; }
    return gpio_pin_configure_dt(&irq, GPIO_INPUT);
}
void AD5940_CsClr(void) { remember(gpio_pin_set_dt(&cs, 1)); k_busy_wait(1); }
void AD5940_CsSet(void) { remember(gpio_pin_set_dt(&cs, 0)); k_busy_wait(1); }
void AD5940_RstClr(void) { remember(gpio_pin_set_dt(&reset, 1)); }
void AD5940_RstSet(void) { remember(gpio_pin_set_dt(&reset, 0)); }
void AD5940_Delay10us(uint32_t time)
{
    while (time >= 100) { k_msleep(1); time -= 100; }
    if (time) { k_busy_wait(time * 10); }
}
void AD5940_ReadWriteNBytes(unsigned char *tx, unsigned char *rx, unsigned long length)
{
    struct spi_buf tb = { .buf = tx, .len = length };
    struct spi_buf rb = { .buf = rx, .len = length };
    struct spi_buf_set ts = { .buffers = &tb, .count = 1 };
    struct spi_buf_set rs = { .buffers = &rb, .count = 1 };
    if (io_error) { memset(rx, 0, length); return; }
    int rc = spi_transceive(bus, &config, &ts, &rs);
    if (rc) { memset(rx, 0, length); remember(rc); }
}
uint32_t AD5940_GetMCUIntFlag(void)
{
    int level = gpio_pin_get_dt(&irq);
    remember(level);
    return level > 0;
}
uint32_t AD5940_ClrMCUIntFlag(void) { return 0; }
uint32_t AD5940_MCUResourceInit(void *cfg) { ARG_UNUSED(cfg); return 0; }
