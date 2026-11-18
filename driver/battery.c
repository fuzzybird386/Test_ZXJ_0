#include "battery.h"

#include <stdint.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>

#include "../src/inc/bsp.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

/* P1.14/AIN7, 820k:1M divider. NCS supplies the correct 900 mV reference.
 * Percentage is an approximate CR2032 voltage scale, not a fuel gauge.
 * Change thresholds if the actual fitted cell is a different chemistry. */
#define ADC_RESOLUTION_BITS 12
#ifndef BAT_FULL_MV
#define BAT_FULL_MV 3000
#endif
#ifndef BAT_EMPTY_MV
#define BAT_EMPTY_MV 2400
#endif
static const struct device *adc_dev;
static bool s_initialized;

#if DT_NODE_HAS_STATUS(DT_INST(0, nordic_nrf_saadc), okay)

static int16_t sample_buf;
static struct adc_channel_cfg ch_cfg = {
    .gain             = ADC_GAIN_1_4,
    .reference        = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
    .channel_id       = 0,
#if defined(CONFIG_ADC_CONFIGURABLE_INPUTS)
    /* NCS 3.4 uses logical AIN indices, not raw PSELP register encoding. */
    .input_positive   = NRF_SAADC_AIN7,
#endif
};

static struct adc_sequence sequence = {
    .channels    = BIT(0),
    .buffer      = &sample_buf,
    .buffer_size = sizeof(sample_buf),
    .resolution  = ADC_RESOLUTION_BITS,
};

#endif

int battery_init(void)
{
    if (s_initialized) return 0;

#if DT_NODE_HAS_STATUS(DT_INST(0, nordic_nrf_saadc), okay)
    adc_dev = DEVICE_DT_GET_ONE(nordic_nrf_saadc);
    if (!device_is_ready(adc_dev)) {
        return -ENODEV;
    }
    int err = adc_channel_setup(adc_dev, &ch_cfg);
    if (err) {
        return err;
    }
    s_initialized = true;
#else
    LOG_WRN("SAADC node disabled; battery readings stubbed");
#endif
    return 0;
}

int battery_refresh(battery_measurement_t *out)
{
    if (!out) return -EINVAL;

#if DT_NODE_HAS_STATUS(DT_INST(0, nordic_nrf_saadc), okay)
    if (!s_initialized || !adc_dev) {
        return -EIO;
    }
    int err = adc_read(adc_dev, &sequence);
    if (err) {
        return -EIO;
    }
    int32_t raw = sample_buf;
    if (raw < 0) raw = 0;
    int32_t v_adc_mv = raw;
    err = adc_raw_to_millivolts(adc_ref_internal(adc_dev), ch_cfg.gain,
                               ADC_RESOLUTION_BITS, &v_adc_mv);
    if (err) return err;
    int32_t v_bat_mv = (v_adc_mv * BATTERY_DIVIDER_NUM) / BATTERY_DIVIDER_DEN;

    int pct;
    if      (v_bat_mv >= BAT_FULL_MV)  pct = 100;
    else if (v_bat_mv <= BAT_EMPTY_MV) pct = 0;
    else pct = (v_bat_mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);

    out->level_pct = (uint8_t)pct;
#else
    out->level_pct = 100u;
#endif
    return 0;
}
