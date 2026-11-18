/*
 * AD5940/5941 → Zephyr port layer.
 *
 * The AD5940 library (lib/ad5940/ad5940.c) is platform-agnostic and expects
 * the integrator to supply the symbols declared in its "Library_Interface"
 * group. This file fulfils that contract on top of Zephyr:
 *
 *   - SPI access: Zephyr SPI (spi00, see board DTS). We drive CS by hand
 *                 from a GPIO line so the AD5940 lib's per-byte CS toggling
 *                 is honoured exactly.
 *   - Reset:      GPIO line on P2.4 (active low).
 *   - Interrupt:  GPIO line on P2.3 (active low). Polled via
 *                 `AD5940_GetMCUIntFlag` for now; the GPIO callback latches
 *                 a flag we expose to the application loop.
 *   - Delay:      `k_busy_wait` for short waits (10..1000 µs scale).
 *
 * SPI mode: AD5940 uses SPI mode 0, MSB-first, ≤16 MHz. We default to 8 MHz
 * (matches the DTS spi-max-frequency on the ad5941 child).
 */

#include "ad5941_port.h"
#include "../inc/bsp.h"

#include "../../lib/ad5940/ad5940.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ad5941_port, LOG_LEVEL_INF);

#define SPI_NODE   DT_NODELABEL(spi00)
#define GPIO2_NODE DT_NODELABEL(gpio2)

static const struct device *spi_dev;
static const struct device *gpio2_dev;

static struct spi_config spi_cfg = {
    .frequency = 8000000U,
    .operation = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8),
    .slave     = 0,
    /* No cs initializer: static zero means manual CS via GPIO. */
};

static volatile uint32_t s_int_flag;
static struct gpio_callback s_int_cb;
static bool s_int_irq_enabled;
static int s_spi_error;
int ad5941_port_error(void) { return s_spi_error; }
void ad5941_port_clear_error(void) { s_spi_error = 0; }

static void int_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
    ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);
    s_int_flag = 1U;
}

int ad5941_port_init(void)
{
    spi_dev   = DEVICE_DT_GET(SPI_NODE);
    gpio2_dev = DEVICE_DT_GET(GPIO2_NODE);

    if (!device_is_ready(spi_dev) || !device_is_ready(gpio2_dev)) {
        LOG_ERR("spi00 or gpio2 not ready");
        return -ENODEV;
    }

    int err = gpio_pin_configure(gpio2_dev, PIN_AD5941_CS_PIN, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("AD5941 CS gpio_pin_configure failed: %d", err);
        return err;
    }

    err = gpio_pin_configure(gpio2_dev, PIN_AD5941_RESET_PIN, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("AD5941 RESET gpio_pin_configure failed: %d", err);
        return err;
    }

    err = gpio_pin_configure(gpio2_dev, PIN_AD5941_INT_PIN, GPIO_INPUT | GPIO_PULL_UP);
    if (err) {
        LOG_ERR("AD5941 INT gpio_pin_configure failed: %d", err);
        return err;
    }

    s_int_irq_enabled = false;
    err = gpio_pin_interrupt_configure(gpio2_dev, PIN_AD5941_INT_PIN,
                                       GPIO_INT_EDGE_FALLING);
    if (err == -ENOTSUP) {
        LOG_WRN("AD5941 INT interrupt unsupported on P2.%u, fallback to polling",
                PIN_AD5941_INT_PIN);
    } else if (err) {
        LOG_ERR("AD5941 INT gpio_pin_interrupt_configure failed: %d", err);
        return err;
    } else {
        gpio_init_callback(&s_int_cb, int_isr, BIT(PIN_AD5941_INT_PIN));
        err = gpio_add_callback(gpio2_dev, &s_int_cb);
        if (err) {
            LOG_ERR("AD5941 gpio_add_callback failed: %d", err);
            return err;
        }
        s_int_irq_enabled = true;
    }

    /* Hard reset pulse (≥1 µs low). */
    gpio_pin_set(gpio2_dev, PIN_AD5941_RESET_PIN, 0);
    k_busy_wait(50);
    gpio_pin_set(gpio2_dev, PIN_AD5941_RESET_PIN, 1);
    k_msleep(2);

    s_int_flag = 0;
    LOG_INF("AD5941 port ready (CS=P2.%u INT=P2.%u RST=P2.%u, int_mode=%s)",
            PIN_AD5941_CS_PIN, PIN_AD5941_INT_PIN, PIN_AD5941_RESET_PIN,
            s_int_irq_enabled ? "irq" : "poll");
    return 0;
}

/* ── AD5940 lib hooks ────────────────────────────────────────────────── */

void AD5940_CsClr(void) { if (gpio2_dev) gpio_pin_set(gpio2_dev, PIN_AD5941_CS_PIN, 0); }
void AD5940_CsSet(void) { if (gpio2_dev) gpio_pin_set(gpio2_dev, PIN_AD5941_CS_PIN, 1); }

void AD5940_RstClr(void) { if (gpio2_dev) gpio_pin_set(gpio2_dev, PIN_AD5941_RESET_PIN, 0); }
void AD5940_RstSet(void) { if (gpio2_dev) gpio_pin_set(gpio2_dev, PIN_AD5941_RESET_PIN, 1); }

void AD5940_Delay10us(uint32_t time)
{
    /* time is in 10-µs ticks. */
    if (time == 0) return;
    if (time < 100) {
        k_busy_wait(time * 10U);
    } else {
        k_msleep((time + 99U) / 100U);
    }
}

void AD5940_ReadWriteNBytes(unsigned char *pSendBuffer,
                            unsigned char *pRecvBuff,
                            unsigned long length)
{
    if (!spi_dev || length == 0) {
        return;
    }
    const struct spi_buf txb = { .buf = pSendBuffer, .len = length };
    const struct spi_buf rxb = { .buf = pRecvBuff,   .len = length };
    const struct spi_buf_set tx = { .buffers = &txb, .count = 1 };
    const struct spi_buf_set rx = { .buffers = &rxb, .count = 1 };

    int err = spi_transceive(spi_dev, &spi_cfg, &tx, &rx);
    if (err) {
        s_spi_error = err;
        LOG_ERR("AD5940 SPI xfer err=%d", err);
    }
}

uint32_t AD5940_GetMCUIntFlag(void)
{
    if (s_int_irq_enabled) {
        return s_int_flag;
    }

    if (!gpio2_dev) {
        return 0;
    }

    int v = gpio_pin_get(gpio2_dev, PIN_AD5941_INT_PIN);
    if (v < 0) {
        return 0;
    }
    /* AD5941 INT is active low. */
    return (v == 0) ? 1U : 0U;
}

uint32_t AD5940_ClrMCUIntFlag(void)
{
    s_int_flag = 0;
    return 0;
}

uint32_t AD5940_MCUResourceInit(void *pCfg)
{
    ARG_UNUSED(pCfg);
    return (uint32_t)ad5941_port_init();
}

/* GPIO bridge stubs — examples seldom use these. */
void     AD5940_MCUGpioWrite(uint32_t data)             { ARG_UNUSED(data); }
uint32_t AD5940_MCUGpioRead(uint32_t pins)              { ARG_UNUSED(pins); return 0; }
void     AD5940_MCUGpioCtrl(uint32_t pins, BoolFlag en) { ARG_UNUSED(pins); ARG_UNUSED(en); }
