#pragma once
#ifndef EC_PROXY_H__
#define EC_PROXY_H__

#include <stdint.h>
#include "../inc/ec_method.h"

/**
 * Per-method proxy interface (CV / CA / EIS / DPV / OCP).
 *
 * All proxies implement the unified runner interface:
 *   start()  — initialise hardware, return initial tick delay in µs.
 *   tick()   — advance state machine one step; called by the runner at the
 *               scheduled time.  Natural completion: clean up hardware before
 *               returning EC_TICK_DONE.
 *   stop()   — called ONLY on external abort; tears down hardware (runner
 *               pushes END frame).
 */

/* Board-level AD5941 calibration constants. */
#define EC_PROXY_SYSCLK_HZ      16000000.0f
#define EC_PROXY_ADCCLK_HZ      16000000.0f
#define EC_PROXY_RCAL_OHM       200.0f

/* Lazily ensures the AD5941 SPI/GPIO port is ready. Does not reset the AFE. */
int ec_proxy_bus_ready(void);

/*
 * Per-experiment AD5941 bring-up. Every AD5941-backed method must call this
 * before method-specific calibration/configuration:
 *   hard reset -> AD5940 init -> soft reset -> AD5940 init -> common INT route.
 */
int ec_proxy_bus_prepare_experiment(const char *tag);

/* CV — Cyclic Voltammetry. */
int              ec_proxy_cv_start(const uint8_t *params, uint16_t len,
                                   uint32_t *first_delay_us);
ec_tick_status_t ec_proxy_cv_tick(ec_tick_out_t *out);
void             ec_proxy_cv_stop(void);

/* CA — Chronoamperometry. */
int              ec_proxy_ca_start(const uint8_t *params, uint16_t len,
                                   uint32_t *first_delay_us);
ec_tick_status_t ec_proxy_ca_tick(ec_tick_out_t *out);
void             ec_proxy_ca_stop(void);

/* EIS — Electrochemical Impedance Spectroscopy. */
int              ec_proxy_eis_start(const uint8_t *params, uint16_t len,
                                    uint32_t *first_delay_us);
ec_tick_status_t ec_proxy_eis_tick(ec_tick_out_t *out);
void             ec_proxy_eis_stop(void);

/* DPV — Differential Pulse Voltammetry. */
int              ec_proxy_dpv_start(const uint8_t *params, uint16_t len,
                                    uint32_t *first_delay_us);
ec_tick_status_t ec_proxy_dpv_tick(ec_tick_out_t *out);
void             ec_proxy_dpv_stop(void);

/* OCP — Open-Circuit Potential. */
int              ec_proxy_ocp_start(const uint8_t *params, uint16_t len,
                                    uint32_t *first_delay_us);
ec_tick_status_t ec_proxy_ocp_tick(ec_tick_out_t *out);
void             ec_proxy_ocp_stop(void);

#endif /* EC_PROXY_H__ */
