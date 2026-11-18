#pragma once
#ifndef EC_PROXY_LPAMP_H__
#define EC_PROXY_LPAMP_H__

#include <stdint.h>

#include "../../../lib/ad5940/ad5940.h"

#define EC_LPAMP_VZERO_MV       1300 /* 0.2 V offset + code32 * 34.38 mV */
#define EC_LPAMP_VZERO_CODE6    32
#define EC_LPAMP_ADC_PGA        ADCPGA_1P5
#define EC_LPAMP_ADC_REF_V      1.82f

uint16_t ec_lpamp_mV_to_dac12(int16_t e_mV);
void ec_lpamp_write_dac(int16_t e_mV);

/**
 * Select the LPTIA current-range gear (see enum ec_rtia_gear_t in ec_params.h).
 * Must be called BEFORE ec_lpamp_calibrate_and_configure(). Out-of-range gears
 * fall back to the default (gear 0, 200 Ω). Affects CV / CA / DPV.
 */
void ec_lpamp_set_rtia_gear(uint8_t gear);

int ec_lpamp_calibrate_and_configure(const char *tag);
float ec_lpamp_rtia_ohm(void);

#endif /* EC_PROXY_LPAMP_H__ */

void ec_lpamp_shutdown(void);
