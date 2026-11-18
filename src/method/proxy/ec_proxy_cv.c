/*
 * CV proxy — Cyclic Voltammetry using the AD5940 LP amperometric loop.
 *
 * References:
 *   [1] AD5940 datasheet, Rev. G, Analog Devices
 *       https://www.analog.com/media/en/technical-documentation/data-sheets/AD5940-5941.pdf
 *   [2] AD5940 library (ad5940.h / ad5940.c) — LP loop APIs, struct definitions
 *       https://github.com/analogdevicesinc/ad5940lib
 *   [3] AD5940 example: RampTest (LPDAC ramp + LPTIA amperometry)
 *       https://github.com/analogdevicesinc/ad5940-examples/tree/master/examples/AD5940_Ramp
 *   [4] AD5940 example: ChronoAmperometric (LP loop steady-state)
 *       https://github.com/analogdevicesinc/ad5940-examples/tree/master/examples/AD5940_ChronoAmperometric
 *
 * E = V(WE)-V(RE) = Vzero-Vbias. Vzero code32 is approximately 1.3 V.
 * DAC conversion and code compensation follow ADI RampTest.c in ec_proxy_lpamp.c.
 * The selected RTIA is measured with the same RLOAD used during acquisition.
 *
 * Current equation (sign: current into WE is positive):
 *   v_diff = AD5940_ADCCode2Volt(code, EC_LPAMP_ADC_PGA, 1.82f)  [V]
 *   I_nA   = -(int32_t)(v_diff / calibrated_rtia * 1e9f)
 *
 * Triangle walk:
 *   first cycle:  e_begin → e_vertex1 → e_vertex2
 *   next cycles:  e_vertex2 → e_vertex1 → e_vertex2
 *   final segment: e_vertex2 → e_end, only when e_end differs from e_vertex2.
 * Each step fires one ADC conversion, waits for SINC2RDY, emits sample.
 */

#include "ec_proxy.h"
#include "ec_proxy_lpamp.h"
#include "../../inc/ble_aabb.h"
#include "../inc/ec_method.h"
#include "../inc/ec_params.h"
#include "../../mux/mux.h"
#include "../../../lib/ad5940/ad5940.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ec_proxy_cv, LOG_LEVEL_INF);

/* ---- state --------------------------------------------------------------- */
static ec_cv_params_t s_p;
static int16_t        s_e_now_mV;
static uint16_t s_cycle;
static uint8_t s_segment; /* 0 -> vertex1, 1 -> vertex2, 2 -> end */
static uint32_t       s_timeout_cnt;
static uint32_t       s_sample_cnt;

typedef struct {
    int32_t  current_nA;
    uint32_t raw_code;
    int32_t  v_diff_uV;
} cv_current_sample_t;

/* ---- helpers ------------------------------------------------------------- */

/**
 * Trigger one ADC conversion via AFECtrl.
 *
 * The LPDAC potential is NOT written here: it must already have been applied
 * (and allowed to settle for one scan period) by the previous tick or by
 * start(). Sampling immediately after a potential step would capture the
 * capacitive charging transient instead of the settled faradaic current,
 * which distorts the voltammogram.
 */
static void cv_start_conversion(void)
{
    /* Start one ADC conversion. */
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
    AD5940_ADCConvtCtrlS(bTRUE);
}

/** Read SINC2 result and convert to current_nA. Polls until ready. */
static int cv_read_current(cv_current_sample_t *sample)
{
    /* Wait for SINC2 ready flag (poll, max 20 ms). */
    uint32_t t0 = k_uptime_get_32();
    while (!AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_SINC2RDY) &&
           !AD5940_INTCTestFlag(AFEINTC_1, AFEINTSRC_SINC2RDY)) {
        if (k_uptime_get_32() - t0 > 20) {
            s_timeout_cnt++;
            if ((s_timeout_cnt & 0x0F) == 1U) {
                uint32_t f0 = AD5940_INTCGetFlag(AFEINTC_0);
                uint32_t f1 = AD5940_INTCGetFlag(AFEINTC_1);
                uint32_t afe = AD5940_ReadReg(REG_AFE_AFECON);
                LOG_WRN("CV: SINC2RDY timeout (cnt=%u f0=0x%08x f1=0x%08x AFECON=0x%08x)",
                        (unsigned)s_timeout_cnt, (unsigned)f0, (unsigned)f1, (unsigned)afe);
            } else {
                LOG_WRN("CV: SINC2RDY timeout");
            }
            AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
            AD5940_ADCConvtCtrlS(bFALSE);
            return -ETIMEDOUT;
        }
    }
    s_timeout_cnt = 0;
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
    AD5940_ADCConvtCtrlS(bFALSE);

    uint32_t code = AD5940_ReadAfeResult(AFERESULT_SINC2);
    float v_diff  = AD5940_ADCCode2Volt(code, EC_LPAMP_ADC_PGA, EC_LPAMP_ADC_REF_V);
    /* Current sign: positive = current into WE (reduction). */
    sample->raw_code = code;
    sample->v_diff_uV = (int32_t)lroundf(v_diff * 1000000.0f);
    sample->current_nA = -(int32_t)lroundf(v_diff / ec_lpamp_rtia_ohm() * 1e9f);
    return 0;
}

/* ---- tick ---------------------------------------------------------------- */

ec_tick_status_t ec_proxy_cv_tick(ec_tick_out_t *out)
{
    cv_start_conversion();
    cv_current_sample_t current = {0};
    int err = cv_read_current(&current);
    if (err) {
        out->frame_type = EC_FRAME_ERROR;
        out->payload[0] = (uint8_t)err;
        out->len = 1u;
        AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_SINC2NOTCH |
                        AFECTRL_ADCCNV, bFALSE);
        ec_lpamp_shutdown();
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3201u,
                             "CV ADC timeout at %d mV", (int)s_e_now_mV);
        return EC_TICK_DONE;
    }

    struct __packed {
        int16_t potential_mV;
        int32_t current_nA;
    } *sample = (void *)out->payload;
    sample->potential_mV = s_e_now_mV;
    sample->current_nA   = current.current_nA;
    out->len        = (uint16_t)sizeof(*sample);
    out->frame_type = EC_FRAME_SAMPLE;

    s_sample_cnt++;
    if ((s_sample_cnt % 20U) == 0U) {
        LOG_INF("CV progress: samples=%u E=%d mV I=%d nA raw=0x%08x v=%d uV cycle=%u",
                (unsigned)s_sample_cnt, (int)s_e_now_mV,
                (int)current.current_nA, (unsigned)current.raw_code,
                (int)current.v_diff_uV, (unsigned)s_cycle);
    }

    int16_t target;
    for (;;) {
        target = s_segment == 0 ? s_p.e_vertex1_mV :
                 s_segment == 1 ? s_p.e_vertex2_mV : s_p.e_end_mV;
        if (s_e_now_mV != target) break;
        if (s_segment == 2) { ec_lpamp_shutdown(); return EC_TICK_DONE; }
        if (s_segment == 0) s_segment = 1;
        else { s_cycle++; s_segment = s_cycle >= s_p.cycles ? 2 : 0; }
    }
    int32_t delta = (int32_t)target - s_e_now_mV;
    int32_t distance = delta < 0 ? -delta : delta;
    if (distance > s_p.step_mV) distance = s_p.step_mV;
    s_e_now_mV += delta > 0 ? distance : -distance;
    ec_lpamp_write_dac(s_e_now_mV);
    out->delay_us = (uint32_t)(1000000ULL * distance / s_p.scan_rate_mV_s);
    return EC_TICK_CONTINUE;
}

/* ---- public API ---------------------------------------------------------- */

int ec_proxy_cv_start(const uint8_t *params, uint16_t len, uint32_t *first_delay_us)
{
    if (!params || len != sizeof(ec_cv_params_t)) return -EINVAL;
    memcpy(&s_p, params, sizeof(s_p));
    if (s_p.we_index != 0) return -ENOTSUP;
    if (s_p.rtia_gear >= EC_RTIA_GEAR_COUNT || !s_p.cycles ||
        !s_p.step_mV || s_p.step_mV > 2000 || !s_p.scan_rate_mV_s ||
        s_p.e_vertex1_mV == s_p.e_vertex2_mV) return -EINVAL;
    if (s_p.e_begin_mV < -1000 || s_p.e_begin_mV > 1000 ||
        s_p.e_vertex1_mV < -1000 || s_p.e_vertex1_mV > 1000 ||
        s_p.e_vertex2_mV < -1000 || s_p.e_vertex2_mV > 1000 ||
        s_p.e_end_mV < -1000 || s_p.e_end_mV > 1000) return -ERANGE;
    /* Software-timed mode: >=10 ms even for a shortened endpoint step. */
    int32_t spans[] = { s_p.e_vertex1_mV-s_p.e_begin_mV,
                       s_p.e_vertex2_mV-s_p.e_vertex1_mV,
                       s_p.e_end_mV-s_p.e_vertex2_mV };
    for (unsigned i=0; i<3; i++) {
        uint32_t span = spans[i]<0 ? -spans[i] : spans[i];
        uint32_t shortest = span % s_p.step_mV;
        if (!shortest) shortest = s_p.step_mV;
        if (span && 1000000ULL * shortest / s_p.scan_rate_mV_s < 10000) return -ERANGE;
    }
    int err = ec_proxy_bus_prepare_experiment("CV");
    if (err) return err;
    LOG_INF("CV WE%u: %d→%d→%d→%d mV @%u mV/s, %u cycles, step %u mV, quiet %u ms",
            s_p.we_index, s_p.e_begin_mV, s_p.e_vertex1_mV, s_p.e_vertex2_mV,
            s_p.e_end_mV, s_p.scan_rate_mV_s, s_p.cycles, s_p.step_mV, s_p.quiet_time_ms);

    ec_lpamp_set_rtia_gear(s_p.rtia_gear);
    err = ec_lpamp_calibrate_and_configure("CV");
    if (err) {
        ec_lpamp_shutdown();
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3202u,
                             "CV RTIA calibration failed: %d", err);
        return err;
    }
    err = mux_select_we(s_p.we_index);
    if (err) return err;

    s_e_now_mV   = s_p.e_begin_mV;
    s_cycle = 0;
    s_segment = 0;
    s_sample_cnt = 0;
    s_timeout_cnt = 0;

    /*
     * Apply e_begin to the LPDAC so the electrode is conditioned at the
     * correct potential during the quiet period before the first tick.
     */
    ec_lpamp_write_dac(s_p.e_begin_mV);

    *first_delay_us = (uint32_t)s_p.quiet_time_ms * 1000u;
    return 0;
}

void ec_proxy_cv_stop(void)
{
    AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_SINC2NOTCH | AFECTRL_ADCCNV, bFALSE);
    ec_lpamp_shutdown();
    LOG_INF("[REC][CV] stopped (external)");
}
