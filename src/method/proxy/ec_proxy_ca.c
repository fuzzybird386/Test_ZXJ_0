/*
 * CA proxy — Chronoamperometry using the AD5940 LP amperometric loop.
 *
 * References:
 *   [1] AD5940 datasheet, Rev. C, Analog Devices
 *       https://www.analog.com/media/en/technical-documentation/data-sheets/AD5940.pdf
 *   [2] AD5940 library (ad5940.h / ad5940.c) — LP loop APIs, struct definitions
 *       https://github.com/analogdevicesinc/ad5940lib
 *   [3] AD5940 example: ChronoAmperometric (LPDAC step + LPTIA timed sampling)
 *       https://github.com/analogdevicesinc/ad5940-examples/tree/master/examples/AD5940_ChronoAmperometric
 *
 * Signal chain: same as CV (see ec_proxy_cv.c for hardware details).
 * n_pulses:        number of identical step-pulses to apply in sequence.  Each
 *                   pulse holds e_step_mV for pulse_width_ms, sampling every
 *                   sample_period_ms.  After the last pulse the method ends.
 *                   Set n_pulses=1 for classic single-step CA.
 * quiet_time_ms:    pre-pulse equilibration at 0 mV (WE floating to RE) before
 *                   the step is applied.
 *
 * SAMPLE frame payload (10 bytes):
 *   uint32_t t_ms        — elapsed time since measurement start
 *   int16_t  e_step_mV   — applied potential vs RE
 *   int32_t  current_nA  — measured current
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

LOG_MODULE_REGISTER(ec_proxy_ca, LOG_LEVEL_INF);

/* ---- state --------------------------------------------------------------- */
static ec_ca_params_t s_p;
static uint16_t       s_pulse_idx;         /* current pulse index (0 .. n_pulses-1) */
static uint32_t       s_sample_in_pulse;   /* samples taken in this pulse */
static uint32_t       s_samples_per_pulse; /* pulse_width_ms / sample_period_ms */
static uint32_t       s_started_ms;
static uint32_t       s_timeout_cnt;
static uint32_t       s_sample_cnt;

/* ---- helpers ------------------------------------------------------------- */

static void ca_write_step_potential(int16_t e_mV)
{
    ec_lpamp_write_dac(e_mV);
}

static int ca_read_current_nA(int32_t *current_nA)
{
    /* Trigger one ADC conversion. */
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
    AD5940_ADCConvtCtrlS(bTRUE);

    /* Poll for SINC2 ready (max 20 ms). */
    uint32_t t0 = k_uptime_get_32();
    while (!AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_SINC2RDY) &&
           !AD5940_INTCTestFlag(AFEINTC_1, AFEINTSRC_SINC2RDY)) {
        if (k_uptime_get_32() - t0 > 20) {
            s_timeout_cnt++;
            if ((s_timeout_cnt & 0x0F) == 1U) {
                LOG_WRN("CA: SINC2RDY timeout (cnt=%u f0=0x%08x f1=0x%08x)",
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

/* ---- tick ---------------------------------------------------------------- */

ec_tick_status_t ec_proxy_ca_tick(ec_tick_out_t *out)
{
    /* On the very first tick (start of pulse 0), apply the step potential. */
    if (s_pulse_idx == 0 && s_sample_in_pulse == 0) {
        ca_write_step_potential(s_p.e_step_mV);
        s_started_ms = k_uptime_get_32();  /* reset elapsed timer after step */
    }

    int32_t current_nA = 0;
    int err = ca_read_current_nA(&current_nA);
    if (err) {
        out->frame_type = EC_FRAME_ERROR;
        out->payload[0] = (uint8_t)err;
        out->len = 1u;
        AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_SINC2NOTCH |
                        AFECTRL_ADCCNV, bFALSE);
        mux_disable();
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3301u,
                             "CA ADC timeout at %d mV", (int)s_p.e_step_mV);
        return EC_TICK_DONE;
    }

    struct __packed {
        uint32_t t_ms;
        int16_t  e_step_mV;
        int32_t  current_nA;
    } *sample = (void *)out->payload;
    sample->t_ms       = k_uptime_get_32() - s_started_ms;
    sample->e_step_mV  = s_p.e_step_mV;
    sample->current_nA = current_nA;
    out->len        = (uint16_t)sizeof(*sample);
    out->frame_type = EC_FRAME_SAMPLE;

    s_sample_cnt++;
    s_sample_in_pulse++;

    if (s_sample_cnt <= 3U || (s_sample_cnt % 10U) == 0U) {
        LOG_INF("[REC][CA] n=%u pulse=%u/%u t=%ums E=%dmV I=%dnA",
                (unsigned)s_sample_cnt,
                (unsigned)(s_pulse_idx + 1u), (unsigned)s_p.n_pulses,
                (unsigned)sample->t_ms,
                (int)sample->e_step_mV,
                (int)sample->current_nA);
    }

    /* Advance pulse when pulse_width_ms worth of samples have been taken. */
    if (s_sample_in_pulse >= s_samples_per_pulse) {
        s_pulse_idx++;
        s_sample_in_pulse = 0;
        if (s_p.n_pulses != 0 && s_pulse_idx >= s_p.n_pulses) {
            AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_SINC2NOTCH | AFECTRL_ADCCNV, bFALSE);
            mux_disable();
            LOG_INF("[REC][CA] done, pulses=%u samples=%u",
                    (unsigned)s_p.n_pulses, (unsigned)s_sample_cnt);
            return EC_TICK_DONE;
        }
    }

    out->delay_us = (uint32_t)(s_p.sample_period_ms ? s_p.sample_period_ms : 100u) * 1000u;
    return EC_TICK_CONTINUE;
}

/* ---- public API ---------------------------------------------------------- */

int ec_proxy_ca_start(const uint8_t *params, uint16_t len, uint32_t *first_delay_us)
{
    if (len < sizeof(ec_ca_params_t)) return -EINVAL;
    int err = ec_proxy_bus_prepare_experiment("CA");
    if (err) return err;

    memcpy(&s_p, params, sizeof(s_p));
    err = mux_select_we(0xFF);
    if (err) return err;

    LOG_INF("CA WE%u: step=%d mV, pw=%u ms, T=%u ms, N=%u, quiet=%u",
            s_p.we_index, s_p.e_step_mV, s_p.pulse_width_ms, s_p.sample_period_ms,
            s_p.n_pulses, s_p.quiet_time_ms);

    ec_lpamp_set_rtia_gear(s_p.rtia_gear);
    err = ec_lpamp_calibrate_and_configure("CA");
    if (err) {
        mux_disable();
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3302u,
                             "CA RTIA calibration failed: %d", err);
        return err;
    }
    err = mux_select_we(s_p.we_index);
    if (err) return err;

    /*
     * During quiet time the electrode rests at 0 mV (WE ≈ RE potential).
     * The step potential is applied by the first tick() call.
     */
    ca_write_step_potential(0);

    s_pulse_idx       = 0;
    s_sample_in_pulse = 0;
    s_started_ms      = k_uptime_get_32();
    s_sample_cnt      = 0;
    s_timeout_cnt     = 0;

    /* Ceiling division: ensure the last partial period is still counted. */
    uint32_t period_ms = s_p.sample_period_ms ? s_p.sample_period_ms : 100u;
    s_samples_per_pulse = (s_p.pulse_width_ms + period_ms - 1u) / period_ms;
    if (s_samples_per_pulse == 0) s_samples_per_pulse = 1;

    *first_delay_us = (uint32_t)(s_p.quiet_time_ms ? s_p.quiet_time_ms : 0u) * 1000u;
    return 0;
}

void ec_proxy_ca_stop(void)
{
    AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_SINC2NOTCH | AFECTRL_ADCCNV, bFALSE);
    mux_disable();
    LOG_INF("[REC][CA] stopped (external), samples=%u", (unsigned)s_sample_cnt);
}
