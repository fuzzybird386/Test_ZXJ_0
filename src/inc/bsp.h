#pragma once
#ifndef BSP_H__
#define BSP_H__

#include <hal/nrf_gpio.h>

/*
 * Single-channel ECW board pinout (现电路图.pdf, 2026-11-11).
 * Pin numbering uses NRF_GPIO_PIN_MAP(port, pin).
 *
 * Helper convention used by drivers in this repo:
 *   <NAME>_PORT  : GPIO port index (0/1/2)  → DT_NODELABEL(gpioN)
 *   <NAME>_PIN   : pin index inside the port
 */

/* ── Power latch ─────────────────────────────────────────────────────── */
#define PIN_POWER_ON_PORT      0
#define PIN_POWER_ON_PIN       2
#define PIN_POWER_ON           NRF_GPIO_PIN_MAP(PIN_POWER_ON_PORT, PIN_POWER_ON_PIN)

/* ── Status LEDs ─────────────────────────────────────────────────────── */
#define PIN_LED_RED_PORT       1
#define PIN_LED_RED_PIN        8
#define PIN_LED_RED            NRF_GPIO_PIN_MAP(PIN_LED_RED_PORT, PIN_LED_RED_PIN)

#define PIN_LED_GREEN_PORT     1
#define PIN_LED_GREEN_PIN      7
#define PIN_LED_GREEN          NRF_GPIO_PIN_MAP(PIN_LED_GREEN_PORT, PIN_LED_GREEN_PIN)

/* ── Battery monitor ─────────────────────────────────────────────────── */
/* NCS 3.4 SAADC API uses NRF_SAADC_AIN7 for this pin */
#define PIN_ADC_BATTERY_PORT   1  /* P1.14 */
#define PIN_ADC_BATTERY_PIN    14

/* Voltage divider on board: VBAT --820kΩ-- ADC --1MΩ-- GND
 *   V_adc = V_bat * 1e6 / (820e3 + 1e6) = V_bat * 1000 / 1820
 */
#define BATTERY_DIVIDER_NUM    1820
#define BATTERY_DIVIDER_DEN    1000

/* ── AD5941 SPI (Zephyr SPIM bus, see DTS spi00) ─────────────────────── */
#define PIN_AD5941_SCK_PORT    2
#define PIN_AD5941_SCK_PIN     6
#define PIN_AD5941_MOSI_PORT   2
#define PIN_AD5941_MOSI_PIN    8
#define PIN_AD5941_MISO_PORT   2
#define PIN_AD5941_MISO_PIN    9
#define PIN_AD5941_CS_PORT     2
#define PIN_AD5941_CS_PIN      10
#define PIN_AD5941_RESET_PORT  2
#define PIN_AD5941_RESET_PIN   4
#define PIN_AD5941_RESET       NRF_GPIO_PIN_MAP(PIN_AD5941_RESET_PORT, PIN_AD5941_RESET_PIN)
/* AD5941 GP0 INT line (active low). Wire matches DTS `int-gpios`. */
#define PIN_AD5941_INT_PORT    2
#define PIN_AD5941_INT_PIN     3
#define PIN_AD5941_INT         NRF_GPIO_PIN_MAP(PIN_AD5941_INT_PORT, PIN_AD5941_INT_PIN)

#endif /* BSP_H__ */
