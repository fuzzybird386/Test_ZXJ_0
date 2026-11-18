/*
 * DPV proxy — Differential Pulse Voltammetry using the AD5940 LP loop.
 *
 * References:
 *   [1] AD5940 datasheet, Rev. C, Analog Devices
 *       https://www.analog.com/media/en/technical-documentation/data-sheets/AD5940.pdf
 *   [2] AD5940 library (ad5940.h / ad5940.c) — LP loop APIs, struct definitions
 *       https://github.com/analogdevicesinc/ad5940lib
 *   [3] AD5940 example: SquareWaveVoltammetry (stepped DC + superposed pulse, two-sample Δi)
 *       https://github.com/analogdevicesinc/ad5940-examples/tree/master/examples/AD5940_SqrWaveVoltammetry
 *   [4] DPV technique overview — Electroanalytical Chemistry, A. J. Bard & L. R. Faulkner,
 *       "Electrochemical Methods", 2nd ed., Wiley, 2001, §7.3
 *
 * Per-step sequence (period = period_ms):
 *   1. Set Vbias = e_base    → wait (period_ms − pulse_width_ms) → sample i_base
 *   2. Set Vbias = e_base + pulse_amp → wait pulse_width_ms       → sample i_pulse
 *   3. Advance e_base by step_mV; report {e_base, i_base_nA, i_pulse_nA}
 *
 * SAMPLE frame payload (10 bytes):
 *   int16_t potential_mV   — base potential vs RE
 *   int32_t i_base_nA      — current at base potential (end of pre-pulse phase)
 *   int32_t i_pulse_nA     — current at base+pulse (end of pulse phase)
 * (Host computes Δi = i_pulse − i_base if desired.)
 *
 * Signal chain: same LP amperometry path as CV/CA:
 *   LPDAC0 drives the electrode potential vs RE, LPTIA0 measures current via
 *   the calibrated 200 Ω internal RTIA, and ADC samples LPTIA0_P/N.
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

LOG_MODULE_REGISTER(ec_proxy_dpv, LOG_LEVEL_INF);

/* ---- state --------------------------------------------------------------- */
static ec_dpv_params_t s_p;
static int16_t         s_e_now_mV;
static uint32_t        s_timeout_cnt;
static uint32_t        s_sample_cnt;
/* Signed step (+= forward scan, -= reverse scan). */
static int16_t         s_step_signed;

typedef enum { DPV_PHASE_BASE = 0, DPV_PHASE_PULSE } dpv_phase_t;
static dpv_phase_t s_phase;
/* Intermediate base current for current step. */
static int32_t s_i_base_nA;

/* ---- helpers ------------------------------------------------------------- */

static void dpv_set_vbias(int16_t e_mV)
{
    ec_lpamp_write_dac(e_mV);
}

static int dpv_sample_current_nA(int32_t *current_nA)
{
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
    AD5940_ADCConvtCtrlS(bTRUE);
    uint32_t t0 = k_uptime_get_32();
    while (!AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_SINC2RDY) &&
           !AD5940_INTCTestFlag(AFEINTC_1, AFEINTSRC_SINC2RDY)) {
        if (k_uptime_get_32() - t0 > 20) {
            s_timeout_cnt++;
            if ((s_timeout_cnt & 0x0F) == 1U) {
                LOG_WRN("DPV: SINC2RDY timeout (cnt=%u f0=0x%08x f1=0x%08x)",
                        (unsigned)s_timeout_cnt,
                        (unsigned)AD5940_INTCGetFlag(AFEINTC_0),
                        (unsigned)AD5940_INTCGetFlag(AFEINTC_1));
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
    *current_nA = -(int32_t)lroundf(v_diff / ec_lpamp_rtia_ohm() * 1e9f);
    return 0;
}

static ec_tick_status_t dpv_abort_adc_error(ec_tick_out_t *out, const char *phase)
{
    out->frame_type = EC_FRAME_ERROR;
    out->payload[0] = (uint8_t)(-ETIMEDOUT);
    out->len = 1u;
    AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_SINC2NOTCH | AFECTRL_ADCCNV, bFALSE);
    ec_lpamp_shutdown();
    (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3401u,
                         "DPV ADC timeout in %s phase at %d mV",
                         phase ? phase : "unknown", (int)s_e_now_mV);
    return EC_TICK_DONE;
}

/* ---- tick ---------------------------------------------------------------- */

/**
 * Two-phase tick:
 *   BASE phase:  sample i_base, apply pulse, return CONTINUE with no data
 *                and delay = pulse_width_ms (runner will call tick again).
 *   PULSE phase: sample i_pulse, emit SAMPLE frame, advance step.
 *                Return DONE when past e_end, CONTINUE otherwise.
 */
ec_tick_status_t ec_proxy_dpv_tick(ec_tick_out_t *out)
{
    uint32_t pw     = s_p.pulse_width_ms     ? s_p.pulse_width_ms     : 50u;
    uint32_t period = s_p.period_ms          ? s_p.period_ms          : 100u;
    uint32_t pre    = (period > pw)          ? (period - pw)          : 1u;

    if (s_phase == DPV_PHASE_BASE) {
        /* Sample base current. */
        int err = dpv_sample_current_nA(&s_i_base_nA);
        if (err) {
            return dpv_abort_adc_error(out, "base");
        }

        /* Apply pulse: base + pulse_amp. */
        dpv_set_vbias((int16_t)(s_e_now_mV + (int16_t)s_p.pulse_amp_mV));

        /* No data frame this tick; come back after pulse width. */
        out->len      = 0;
        out->delay_us = pw * 1000u;
        s_phase = DPV_PHASE_PULSE;
        return EC_TICK_CONTINUE;
    }

    /* PULSE phase. */
    int32_t i_pulse_nA = 0;
    int err = dpv_sample_current_nA(&i_pulse_nA);
    if (err) {
        return dpv_abort_adc_error(out, "pulse");
    }

    struct __packed {
        int16_t potential_mV;
        int32_t i_base_nA;
        int32_t i_pulse_nA;
    } *sample = (void *)out->payload;
    sample->potential_mV = s_e_now_mV;
    sample->i_base_nA    = s_i_base_nA;
    sample->i_pulse_nA   = i_pulse_nA;
    out->len        = (uint16_t)sizeof(*sample);
    out->frame_type = EC_FRAME_SAMPLE;

    s_sample_cnt++;
    LOG_DBG("[REC][DPV] n=%u E=%dmV Ibase=%dnA Ipulse=%dnA dI=%dnA",
            (unsigned)s_sample_cnt,
            sample->potential_mV,
            sample->i_base_nA,
            sample->i_pulse_nA,
            sample->i_pulse_nA - sample->i_base_nA);

    if (s_e_now_mV == s_p.e_end_mV) {
        ec_lpamp_shutdown();
        return EC_TICK_DONE;
    }
    int32_t next = (int32_t)s_e_now_mV + s_step_signed;
    if ((s_step_signed > 0 && next > s_p.e_end_mV) ||
        (s_step_signed < 0 && next < s_p.e_end_mV)) next = s_p.e_end_mV;
    s_e_now_mV = next;
    /* Set new base potential, schedule pre-pulse phase. */
    dpv_set_vbias(s_e_now_mV);
    out->delay_us = pre * 1000u;
    s_phase = DPV_PHASE_BASE;
    return EC_TICK_CONTINUE;
}

/* ---- public API ---------------------------------------------------------- */

int ec_proxy_dpv_start(const uint8_t *params, uint16_t len, uint32_t *first_delay_us)
{
    if (!params || len != sizeof(ec_dpv_params_t)) return -EINVAL;
    memcpy(&s_p, params, sizeof(s_p));
    if (s_p.we_index != 0) return -ENOTSUP;
    if (s_p.rtia_gear >= EC_RTIA_GEAR_COUNT || !s_p.step_mV ||
        s_p.step_mV > 2000 || !s_p.pulse_amp_mV || s_p.pulse_amp_mV > 2000 ||
        s_p.pulse_width_ms < 10 || s_p.period_ms < (uint32_t)s_p.pulse_width_ms + 10) return -EINVAL;
    if (s_p.e_begin_mV < -1000 || s_p.e_end_mV < -1000 ||
        (int32_t)s_p.e_begin_mV + s_p.pulse_amp_mV > 1000 ||
        (int32_t)s_p.e_end_mV + s_p.pulse_amp_mV > 1000) return -ERANGE;
    int err = ec_proxy_bus_prepare_experiment("DPV");
    if (err) return err;
    LOG_INF("DPV WE%u: %d→%d step %u, pulse %u mV/%u ms, period %u, quiet %u",
            s_p.we_index, s_p.e_begin_mV, s_p.e_end_mV, s_p.step_mV, s_p.pulse_amp_mV,
            s_p.pulse_width_ms, s_p.period_ms, s_p.quiet_time_ms);

    ec_lpamp_set_rtia_gear(s_p.rtia_gear);
    err = ec_lpamp_calibrate_and_configure("DPV");
    if (err) {
        ec_lpamp_shutdown();
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3402u,
                             "DPV RTIA calibration failed: %d", err);
        return err;
    }
    err = mux_select_we(s_p.we_index);
    if (err) return err;

    s_e_now_mV   = s_p.e_begin_mV;
    s_sample_cnt = 0;
    s_timeout_cnt = 0;
    s_phase      = DPV_PHASE_BASE;
    s_step_signed = (s_p.e_end_mV >= s_p.e_begin_mV)
                    ? (int16_t)s_p.step_mV
                    : -(int16_t)s_p.step_mV;

    /* Set initial base potential. */
    dpv_set_vbias(s_e_now_mV);

    uint32_t period = s_p.period_ms      ? s_p.period_ms      : 100u;
    uint32_t pw     = s_p.pulse_width_ms ? s_p.pulse_width_ms : 50u;
    uint32_t pre    = (period > pw)      ? (period - pw)      : 1u;
    *first_delay_us = ((uint32_t)s_p.quiet_time_ms + pre) * 1000u;
    return 0;
}

void ec_proxy_dpv_stop(void)
{
    AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_SINC2NOTCH | AFECTRL_ADCCNV, bFALSE);
    ec_lpamp_shutdown();
    LOG_INF("[REC][DPV] stopped (external), samples=%u", (unsigned)s_sample_cnt);
}
