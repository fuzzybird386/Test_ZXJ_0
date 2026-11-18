/*
 * OCP proxy — Open-Circuit Potential using the AD5940 ADC.
 *
 * References:
 *   [1] AD5940 datasheet, Rev. C, Analog Devices
 *   [2] AD5940 library — ADC mux / SINC2 APIs
 *   [3] ADI application note AN-2336: Open-Circuit Potential Measurement
 *
 * Signal chain (three-electrode, OCP mode — no current flows):
 *   WE  → board mux (ADG704) → SE0 pin
 *   RE  → RE0 pin
 *
 * The selected WE is tied to SE0 through the board mux. In OCP mode the
 * potentiostat PA/CE path stays off, but LPTIA0 is enabled with a high RTIA
 * and VZERO0 on its positive input so SE0 has a valid instrument common-mode.
 * The ADC mux exposes SE0 and RE0 on its positive input side, while the
 * recommended negative input for voltage measurement is VBIAS_CAP/VSET1P1.
 * OCP is then measured as two oversampled voltage readings against that same
 * internal reference:
 *   V_WE = ADC(SE0 − VBIAS_CAP)
 *   V_RE = ADC(RE0 − VBIAS_CAP)
 *   OCP = V_RE − V_WE
 *
 * CE is not driven. LPDAC only provides VZERO0 to LPTIA0; LPPA and high speed
 * excitation switches stay off.
 *
 * Reported value: e_uV = (V_RE0 − V_SE0) × 1e6  [µV]
 *   Positive ⇒ RE is above WE (cathodic open-circuit).
 *   Convention matches most potentiostat software (OCP = V_RE − V_WE).
 *
 * SAMPLE frame payload (8 bytes):
 *   uint32_t t_ms   — elapsed time since measurement start
 *   int32_t  e_uV   — open-circuit potential (V_RE − V_WE) in µV
 */

#include "ec_proxy.h"
#include "../../inc/ble_aabb.h"
#include "../inc/ec_method.h"
#include "../inc/ec_params.h"
#include "../../mux/mux.h"
#include "../../../lib/ad5940/ad5940.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ec_proxy_ocp, LOG_LEVEL_INF);

#define OCP_ADC_REF_V              1.82f
#define OCP_ADC_PGA                ADCPGA_1
#define OCP_ADC_SINC3_OSR          ADCSINC3OSR_5
#define OCP_ADC_SINC2_OSR          ADCSINC2OSR_178
#define OCP_ADC_TIMEOUT_MS         50u
#define OCP_MUX_SETTLE_MS          50u
#define OCP_DISCARD_SAMPLES        8u
#define OCP_AVG_SAMPLES            256u
#define OCP_WE_VZERO_MV            1500u
#define OCP_WE_VZERO_CODE6         38u
#define OCP_WE_ANCHOR_RTIA         LPTIARTIA_512K
#define OCP_WE_ANCHOR_SW           (LPTIASW(2) | LPTIASW(4) | LPTIASW(5))

/* ---- state --------------------------------------------------------------- */
static ec_ocp_params_t s_p;
static uint32_t        s_started_ms;
static uint32_t        s_timeout_cnt;
static uint32_t        s_sample_cnt;

typedef struct {
    uint32_t raw_re;
    uint32_t raw_we;
    int32_t  v_re_uV;
    int32_t  v_we_uV;
    int32_t  e_uV;
} ocp_adc_sample_t;

/* ---- helpers ------------------------------------------------------------- */

static int ocp_calibrate_adc(void)
{
    ADCPGACal_Type cal;
    AD5940_StructInit(&cal, sizeof(cal));
    cal.SysClkFreq = EC_PROXY_SYSCLK_HZ;
    cal.AdcClkFreq = EC_PROXY_ADCCLK_HZ;
    cal.VRef1p82 = OCP_ADC_REF_V;
    cal.VRef1p11 = 1.11f;
    cal.ADCSinc3Osr = OCP_ADC_SINC3_OSR;
    cal.ADCSinc2Osr = OCP_ADC_SINC2_OSR;
    cal.ADCPga = OCP_ADC_PGA;
    cal.PGACalType = PGACALTYPE_OFFSETGAIN;
    cal.TimeOut10us = 10000;

    AD5940Err err = AD5940_ADCPGACal(&cal);
    if (err != AD5940ERR_OK) {
        LOG_ERR("OCP ADC PGA calibration failed: %d", (int)err);
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3502u,
                             "OCP ADC calibration failed: %d", (int)err);
        return -EIO;
    }

    LOG_INF("OCP ADC PGA calibration complete");
    return 0;
}

static void ocp_afe_power_down(void)
{
    AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);
    AD5940_WriteReg(REG_AFE_LPTIASW0, 0u);
    AD5940_WriteReg(REG_AFE_LPDACSW0, 0u);
    AD5940_WriteReg(REG_AFE_DSWFULLCON, 0u);
    AD5940_WriteReg(REG_AFE_NSWFULLCON, 0u);
    AD5940_WriteReg(REG_AFE_PSWFULLCON, 0u);
    AD5940_WriteReg(REG_AFE_TSWFULLCON, 0u);
    AD5940_WriteReg(REG_AFE_SWCON, REG_AFE_SWCON_RESET);

    LPAmpCfg_Type amp;
    AD5940_StructInit(&amp, sizeof(amp));
    amp.LpAmpSel = LPAMP0;
    amp.LpPaPwrEn = bFALSE;
    amp.LpTiaPwrEn = bFALSE;
    AD5940_LPAMPCfgS(&amp);
}

static void ocp_open_all_switches(void)
{
    AD5940_WriteReg(REG_AFE_DSWFULLCON, 0u);
    AD5940_WriteReg(REG_AFE_NSWFULLCON, 0u);
    AD5940_WriteReg(REG_AFE_PSWFULLCON, 0u);
    AD5940_WriteReg(REG_AFE_TSWFULLCON, 0u);
    AD5940_WriteReg(REG_AFE_SWCON, REG_AFE_SWCON_RESET);
    AD5940_WriteReg(REG_AFE_LPTIASW0, 0u);
    AD5940_WriteReg(REG_AFE_LPDACSW0, 0u);
}

static void ocp_configure_we_anchor(void)
{
    LPDACCfg_Type dac;
    AD5940_StructInit(&dac, sizeof(dac));
    dac.LpdacSel      = LPDAC0;
    dac.LpDacSrc      = LPDACSRC_MMR;
    dac.LpDacVzeroMux = LPDACVZERO_6BIT;
    dac.LpDacVbiasMux = LPDACVBIAS_12BIT;
    dac.LpDacSW       = LPDACSW_VZERO2LPTIA | LPDACSW_VZERO2PIN;
    dac.LpDacRef      = LPDACREF_2P5;
    dac.DataRst       = bFALSE;
    dac.PowerEn       = bTRUE;
    dac.DacData12Bit  = 0u;
    dac.DacData6Bit   = OCP_WE_VZERO_CODE6;
    AD5940_LPDACCfgS(&dac);

    LPAmpCfg_Type amp;
    AD5940_StructInit(&amp, sizeof(amp));
    amp.LpAmpSel    = LPAMP0;
    amp.LpTiaRf     = LPTIARF_OPEN;
    amp.LpTiaRload  = LPTIARLOAD_SHORT;
    amp.LpTiaRtia   = OCP_WE_ANCHOR_RTIA;
    amp.LpAmpPwrMod = LPAMPPWR_NORM;
    amp.LpTiaSW     = OCP_WE_ANCHOR_SW;
    amp.LpPaPwrEn   = bFALSE;
    amp.LpTiaPwrEn  = bTRUE;
    AD5940_LPAMPCfgS(&amp);

    LOG_INF("OCP WE anchor: VZERO=%umV(code6=%u), LPTIA RTIA=512K, LPTIASW=0x%04x",
            (unsigned)OCP_WE_VZERO_MV,
            (unsigned)OCP_WE_VZERO_CODE6,
            (unsigned)OCP_WE_ANCHOR_SW);
    LOG_INF("OCP anchor regs: LPDACCON=0x%08x LPDACDAT=0x%08x LPDACSW=0x%08x LPTIACON=0x%08x LPTIASW=0x%08x",
            (unsigned)AD5940_ReadReg(REG_AFE_LPDACCON0),
            (unsigned)AD5940_ReadReg(REG_AFE_LPDACDAT0),
            (unsigned)AD5940_ReadReg(REG_AFE_LPDACSW0),
            (unsigned)AD5940_ReadReg(REG_AFE_LPTIACON0),
            (unsigned)AD5940_ReadReg(REG_AFE_LPTIASW0));
}

static int ocp_wait_sinc2_code(uint32_t *code_out)
{
    uint32_t t0 = k_uptime_get_32();
    while (!AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_SINC2RDY) &&
           !AD5940_INTCTestFlag(AFEINTC_1, AFEINTSRC_SINC2RDY)) {
        if (k_uptime_get_32() - t0 > OCP_ADC_TIMEOUT_MS) {
            s_timeout_cnt++;
            if ((s_timeout_cnt & 0x0F) == 1U) {
                LOG_WRN("OCP: SINC2RDY timeout (cnt=%u f0=0x%08x f1=0x%08x)",
                        (unsigned)s_timeout_cnt,
                        (unsigned)AD5940_INTCGetFlag(AFEINTC_0),
                        (unsigned)AD5940_INTCGetFlag(AFEINTC_1));
            }
            AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
            return -ETIMEDOUT;
        }
    }
    s_timeout_cnt = 0;
    if (code_out) {
        *code_out = AD5940_ReadAfeResult(AFERESULT_SINC2) & 0xFFFFu;
    }
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
    return 0;
}

static int ocp_read_pin_vs_vbias_uV(uint32_t mux_p, int32_t *out_uV, uint32_t *out_raw)
{
    ADCBaseCfg_Type mux = {
        .ADCMuxP = mux_p,
        .ADCMuxN = ADCMUXN_VSET1P1,
        .ADCPga  = OCP_ADC_PGA,
    };
    AD5940_ADCBaseCfgS(&mux);

    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
    AD5940_AFECtrlS(AFECTRL_ADCCNV, bTRUE);
    k_sleep(K_MSEC(OCP_MUX_SETTLE_MS));
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);

    uint32_t code = 0;
    for (uint32_t i = 0; i < OCP_DISCARD_SAMPLES; i++) {
        int err = ocp_wait_sinc2_code(&code);
        if (err) {
            AD5940_AFECtrlS(AFECTRL_ADCCNV, bFALSE);
            return err;
        }
    }

    uint64_t acc = 0;
    for (uint32_t i = 0; i < OCP_AVG_SAMPLES; i++) {
        int err = ocp_wait_sinc2_code(&code);
        if (err) {
            AD5940_AFECtrlS(AFECTRL_ADCCNV, bFALSE);
            return err;
        }
        acc += code;
    }

    AD5940_AFECtrlS(AFECTRL_ADCCNV, bFALSE);
    uint32_t avg_code = (uint32_t)((acc + (OCP_AVG_SAMPLES / 2u)) / OCP_AVG_SAMPLES);

    if (out_raw) {
        *out_raw = avg_code;
    }
    if (out_uV) {
        float v = AD5940_ADCCode2Volt(avg_code, OCP_ADC_PGA, OCP_ADC_REF_V);
        *out_uV = (int32_t)(v * 1e6f);
    }
    return 0;
}

static int ocp_read_v_uV(int32_t *e_uV, ocp_adc_sample_t *sample)
{
    ocp_adc_sample_t tmp = {0};
    int err = ocp_read_pin_vs_vbias_uV(ADCMUXP_VSE0, &tmp.v_we_uV, &tmp.raw_we);
    if (err) {
        return err;
    }

    err = ocp_read_pin_vs_vbias_uV(ADCMUXP_VRE0, &tmp.v_re_uV, &tmp.raw_re);
    if (err) {
        return err;
    }

    tmp.e_uV = tmp.v_re_uV - tmp.v_we_uV;
    *e_uV = tmp.e_uV;
    if (sample) {
        *sample = tmp;
    }
    return 0;
}

/* ---- tick ---------------------------------------------------------------- */

ec_tick_status_t ec_proxy_ocp_tick(ec_tick_out_t *out)
{
    uint32_t tick_t0 = k_uptime_get_32();
    struct __packed {
        uint32_t t_ms;
        int32_t  e_uV;
    } *sample = (void *)out->payload;
    int32_t e_uV = 0;
    ocp_adc_sample_t adc = {0};
    int err = ocp_read_v_uV(&e_uV, &adc);
    if (err) {
        out->frame_type = EC_FRAME_ERROR;
        out->payload[0] = (uint8_t)err;
        out->len = 1u;
        ocp_afe_power_down();
        mux_disable();
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3501u,
                             "OCP ADC timeout on WE%u", s_p.we_index);
        return EC_TICK_DONE;
    }
    uint32_t elapsed = k_uptime_get_32() - s_started_ms;
    sample->t_ms = elapsed;
    sample->e_uV = e_uV;
    out->len        = (uint16_t)sizeof(*sample);
    out->frame_type = EC_FRAME_SAMPLE;

    s_sample_cnt++;
    if (s_sample_cnt <= 3U || (s_sample_cnt % 5U) == 0U) {
        LOG_INF("[REC][OCP] n=%u t=%ums OCP=%duV RE=%duV(raw=0x%04x) WE=%duV(raw=0x%04x)",
                (unsigned)s_sample_cnt,
                (unsigned)sample->t_ms,
                (int)sample->e_uV,
                (int)adc.v_re_uV,
                (unsigned)adc.raw_re,
                (int)adc.v_we_uV,
                (unsigned)adc.raw_we);
    }

    if (s_p.duration_ms && elapsed >= s_p.duration_ms) {
        ocp_afe_power_down();
        mux_disable();
        LOG_INF("[REC][OCP] done, samples=%u", (unsigned)s_sample_cnt);
        return EC_TICK_DONE;
    }

    uint32_t period_ms = s_p.sample_period_ms ? s_p.sample_period_ms : 1000u;
    uint32_t work_ms = k_uptime_get_32() - tick_t0;
    uint32_t next_delay_ms = (work_ms < period_ms) ? (period_ms - work_ms) : 1u;
    out->delay_us = next_delay_ms * 1000u;
    return EC_TICK_CONTINUE;
}

/* ---- public API ---------------------------------------------------------- */

int ec_proxy_ocp_start(const uint8_t *params, uint16_t len, uint32_t *first_delay_us)
{
    if (len < sizeof(ec_ocp_params_t)) return -EINVAL;
    int err = ec_proxy_bus_prepare_experiment("OCP");
    if (err) return err;

    memcpy(&s_p, params, sizeof(s_p));
    err = mux_select_we(0xFF);
    if (err) return err;

    LOG_INF("OCP WE%u: dur=%u ms, T=%u ms", s_p.we_index, s_p.duration_ms, s_p.sample_period_ms);

    err = ocp_calibrate_adc();
    if (err) {
        mux_disable();
        return err;
    }

    AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);

    AFERefCfg_Type ref;
    AD5940_StructInit(&ref, sizeof(ref));
    ref.HpBandgapEn = bTRUE;
    ref.Hp1V1BuffEn = bTRUE;
    ref.Hp1V8BuffEn = bTRUE;
    ref.Hp1V8ThemBuff = bFALSE;
    ref.Hp1V8Ilimit = bFALSE;
    ref.Disc1V1Cap = bFALSE;
    ref.Disc1V8Cap = bFALSE;
    ref.LpBandgapEn = bTRUE;
    ref.LpRefBufEn = bTRUE;
    ref.LpRefBoostEn = bFALSE;
    AD5940_REFCfgS(&ref);

    ocp_open_all_switches();
    AD5940_AFEPwrBW(AFEPWR_LP, AFEBW_50KHZ);
    AD5940_WriteReg(REG_AFE_ADCBUFCON, 0x005F3D04u);

    err = mux_select_we(s_p.we_index);
    if (err) {
        mux_disable();
        return err;
    }
    ocp_configure_we_anchor();

    /* ADC: initial MUX state; ocp_read_v_uV() switches SE0/RE0 per sample. */
    ADCBaseCfg_Type adc_base = {
        .ADCMuxP = ADCMUXP_VSE0,
        .ADCMuxN = ADCMUXN_VSET1P1,
        .ADCPga  = OCP_ADC_PGA,
    };
    AD5940_ADCBaseCfgS(&adc_base);

    ADCFilterCfg_Type flt = {
        .ADCSinc3Osr         = OCP_ADC_SINC3_OSR,
        .ADCSinc2Osr         = OCP_ADC_SINC2_OSR,
        .ADCAvgNum           = ADCAVGNUM_2,
        .ADCRate             = ADCRATE_800KHZ,
        .BpNotch             = bFALSE,
        .BpSinc3             = bFALSE,
        .Sinc3ClkEnable      = bTRUE,
        .Sinc2NotchClkEnable = bTRUE,
        .Sinc2NotchEnable    = bTRUE,
    };
    AD5940_ADCFilterCfgS(&flt);

    AD5940_ADCPowerCtrlS(bTRUE);
    AD5940_AFECtrlS(AFECTRL_HPREFPWR | AFECTRL_DACREFPWR |
                    AFECTRL_SINC2NOTCH, bTRUE);
    AD5940_Delay10us(100);
    AD5940_AFECtrlS(AFECTRL_ADCCNV, bFALSE);
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);

    s_started_ms  = k_uptime_get_32();
    s_sample_cnt  = 0;
    s_timeout_cnt = 0;

    LOG_INF("OCP ADC path: anchored SE0/VBIAS_CAP and RE0/VBIAS_CAP, settle=%ums discard=%u avg=%u",
            (unsigned)OCP_MUX_SETTLE_MS,
            (unsigned)OCP_DISCARD_SAMPLES,
            (unsigned)OCP_AVG_SAMPLES);

    *first_delay_us = 0u;
    return 0;
}

void ec_proxy_ocp_stop(void)
{
    ocp_afe_power_down();
    mux_disable();
    LOG_INF("[REC][OCP] stopped (external), samples=%u", (unsigned)s_sample_cnt);
}
