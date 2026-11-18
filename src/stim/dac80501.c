#include "inc/dac80501.h"
#include "../inc/bsp.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dac80501, LOG_LEVEL_INF);

/* Register map */
#define DAC_REG_NOOP     0x00
#define DAC_REG_DEVID    0x01
#define DAC_REG_SYNC     0x02
#define DAC_REG_CONFIG   0x03
#define DAC_REG_GAIN     0x04
#define DAC_REG_TRIGGER  0x05
#define DAC_REG_STATUS   0x07
#define DAC_REG_DATA     0x08

/* CONFIG bits */
#define CONFIG_REF_PWDWN_BIT  (1u << 8)
#define CONFIG_DAC_PWDWN_BIT  (1u << 0)

/* GAIN bits */
#define GAIN_REF_DIV_2        (1u << 8)   /* internal ref ÷2 */
#define GAIN_BUFF_GAIN_2      (1u << 0)   /* output buffer ×2 */

/* TRIGGER bits */
#define TRIGGER_SOFT_RESET    0x000A      /* lower nibble 0xA → soft reset */

/* Output range (after gain ÷2 ×2 with internal ref) = 2.5 V FS, 16-bit. */
#define DAC_FULL_SCALE_MV     2500u
#define DAC_CENTER_MV         (DAC_FULL_SCALE_MV / 2u) /* 1250 */

static const struct device *gpio_p0;
static struct k_spinlock    bus_lock;

static inline void io_set(uint8_t pin, int v) { gpio_pin_set(gpio_p0, pin, v); }

static void bus_idle(void)
{
    io_set(PIN_DAC_SPI_CS_PIN,  1);  /* CS high */
    io_set(PIN_DAC_SPI_CLK_PIN, 0);  /* CPOL=0  */
    io_set(PIN_DAC_SPI_MOSI_PIN, 0);
}

/* Bit-bang one byte, mode 1 (CPOL=0, CPHA=1, MSB first). */
static void spi_xfer_byte(uint8_t b)
{
    for (int i = 7; i >= 0; --i) {
        /* CPHA=1 → leading edge (rising) is sample edge for the slave;
         * we drive MOSI right before the rising edge.
         */
        io_set(PIN_DAC_SPI_CLK_PIN, 0);
        io_set(PIN_DAC_SPI_MOSI_PIN, (b >> i) & 0x01);
        io_set(PIN_DAC_SPI_CLK_PIN, 1);
    }
    io_set(PIN_DAC_SPI_CLK_PIN, 0);
}

static int dac_write_reg(uint8_t reg, uint16_t data)
{
    if (!gpio_p0) {
        return -ENODEV;
    }
    k_spinlock_key_t key = k_spin_lock(&bus_lock);

    bus_idle();
    io_set(PIN_DAC_SPI_CS_PIN, 0);
    spi_xfer_byte(reg & 0x0F);            /* A[3:0] in lower nibble; upper 4 bits reserved=0 */
    spi_xfer_byte((data >> 8) & 0xFF);
    spi_xfer_byte(data & 0xFF);
    io_set(PIN_DAC_SPI_CS_PIN, 1);

    k_spin_unlock(&bus_lock, key);
    return 0;
}

int dac80501_init(void)
{
    int err;
    gpio_p0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_p0)) {
        return -ENODEV;
    }

    err  = gpio_pin_configure(gpio_p0, PIN_DAC_SPI_CLK_PIN,  GPIO_OUTPUT_INACTIVE);
    err |= gpio_pin_configure(gpio_p0, PIN_DAC_SPI_MOSI_PIN, GPIO_OUTPUT_INACTIVE);
    err |= gpio_pin_configure(gpio_p0, PIN_DAC_SPI_CS_PIN,   GPIO_OUTPUT_HIGH);
    if (err) {
        return -EIO;
    }
    bus_idle();

    /* Soft reset, then set GAIN to internal-ref ÷2 with output buffer ×2,
     * giving a 0..2.5 V output range. CONFIG 0 keeps DAC + REF powered.
     */
    (void)dac80501_soft_reset();
    k_msleep(2);
    (void)dac_write_reg(DAC_REG_CONFIG, 0x0000);
    (void)dac_write_reg(DAC_REG_GAIN,   GAIN_REF_DIV_2 | GAIN_BUFF_GAIN_2);
    (void)dac80501_set_zero();

    LOG_INF("DAC80501 ready (bit-banged on P0.%u/%u/%u)",
            PIN_DAC_SPI_CLK_PIN, PIN_DAC_SPI_MOSI_PIN, PIN_DAC_SPI_CS_PIN);
    return 0;
}

int dac80501_soft_reset(void)
{
    return dac_write_reg(DAC_REG_TRIGGER, TRIGGER_SOFT_RESET);
}

int dac80501_set_voltage_mv(uint16_t mv)
{
    if (mv > DAC_FULL_SCALE_MV) {
        mv = DAC_FULL_SCALE_MV;
    }
    /* code = mv / 2500 * 65535 */
    uint32_t code = ((uint32_t)mv * 65535u + (DAC_FULL_SCALE_MV / 2u)) / DAC_FULL_SCALE_MV;
    if (code > 0xFFFFu) code = 0xFFFFu;
    return dac_write_reg(DAC_REG_DATA, (uint16_t)code);
}

int dac80501_set_signal_uv(int32_t uv)
{
    /* signal range −1.25..+1.25 V → DAC range 0..2.5 V (offset +1.25 V). */
    if (uv > 1250000)  uv = 1250000;
    if (uv < -1250000) uv = -1250000;
    int32_t mv = (uv + 1250000) / 1000;   /* 0..2500 */
    return dac80501_set_voltage_mv((uint16_t)mv);
}

int dac80501_set_current_na(int32_t na)
{
    /* 1 V = 1 mA  →  1 µV = 1 nA. Linear identity. */
    return dac80501_set_signal_uv(na);
}

int dac80501_set_current_uA(int32_t uA)
{
    /* 1 µA = 1000 nA = 1000 µV (because 1 V = 1 mA). */
    return dac80501_set_signal_uv((int32_t)((int64_t)uA * 1000));
}

int dac80501_set_zero(void)
{
    return dac80501_set_voltage_mv(DAC_CENTER_MV);
}
