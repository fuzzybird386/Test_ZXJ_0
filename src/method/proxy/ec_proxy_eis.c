/*
 * EIS proxy — Electrochemical Impedance Spectroscopy on AD5941.
 *
 * Hardware path: HIGH-BANDWIDTH (HS) loop only — never the LP loop.
 *   - Excitation source: HSDAC + Excitation Buffer → SE0 (counter electrode driver)
 *   - Sensing:           CE0 ↔ HSTIA inverting input (sense return)
 *                        RE0 → P switch → buffer → ADC mux (reference electrode)
 *   - WE selection:      handled OUTSIDE AD5941 by the on-board ADG704 mux
 *                        which routes the chosen WEx to the SE node.
 *
 * The 3-wire switch matrix used here matches the AD5940 datasheet
 * "3-electrode impedance" example (datasheet rev A, Fig. 73):
 *   Dswitch = SWD_CE0
 *   Pswitch = SWP_RE0  | SWP_PL
 *   Nswitch = SWN_SE0  | SWN_NL
 *   Tswitch = SWT_SE0LOAD | SWT_TRTIA
 *
 * AFE power mode, SINC filters and DFT length are selected per frequency
 * using ADI's helper so the capture window spans enough cycles for phase.
 *
 * Each frequency point takes two DFT measurements: DUT voltage and HSTIA RTIA
 * voltage. The BLE sample keeps the real/imaginary impedance contract:
 *   freq_mHz + z_real_mOhm + z_imag_mOhm.
 */

#include "ec_proxy.h"
#include "../../inc/ble_aabb.h"
#include "../inc/ec_method.h"
#include "../inc/ec_params.h"
#include "../../mux/mux.h"
#include "../../../lib/ad5940/ad5940.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ec_proxy_eis, LOG_LEVEL_INF);

static ec_eis_params_t s_p;
static uint32_t        s_freq_now_mHz;
static uint32_t        s_sample_cnt;
static float           s_freq_step_mult;  /* 10^(1/ppd), computed at start */
static bool            s_freq_descending;

#define EIS_CAL_EAC_MV          10u
#define EIS_FREQ_FALLBACK_MILLIHZ 100000u  /* 100 Hz. */

/*
 * HSTIA impedance-range gear table. Index = enum ec_hstia_gear_t (host-selected
 * in the EIS START params). Each entry pairs the AD5940 HSTIARTIA_* code with
 * its nominal resistance, used to program the feedback path and to seed/bound
 * the per-frequency RTIA calibration.
 */
static const struct {
    uint32_t rtia_sel;
    float    nom_ohm;
} EC_EIS_HSTIA_GEARS[EC_HSTIA_GEAR_COUNT] = {
    [EC_HSTIA_GEAR_200R] = { HSTIARTIA_200,  200.0f    },
    [EC_HSTIA_GEAR_1K]   = { HSTIARTIA_1K,   1000.0f   },
    [EC_HSTIA_GEAR_5K]   = { HSTIARTIA_5K,   5000.0f   },
    [EC_HSTIA_GEAR_10K]  = { HSTIARTIA_10K,  10000.0f  },
    [EC_HSTIA_GEAR_20K]  = { HSTIARTIA_20K,  20000.0f  },
    [EC_HSTIA_GEAR_40K]  = { HSTIARTIA_40K,  40000.0f  },
    [EC_HSTIA_GEAR_80K]  = { HSTIARTIA_80K,  80000.0f  },
    [EC_HSTIA_GEAR_160K] = { HSTIARTIA_160K, 160000.0f },
};

static uint32_t s_hstia_sel     = HSTIARTIA_5K;   /* programmed feedback path  */
static float    s_hstia_nom_ohm = 5000.0f;        /* nominal, for cal bounding */

static fImpCar_Type s_hstia_rtia_ohm = {
    .Real = 5000.0f,
    .Image = 0.0f,
};

/* Select the HSTIA gear (clamps out-of-range to the 5 kΩ default). */
static void eis_set_hstia_gear(uint8_t gear)
{
    if (gear >= EC_HSTIA_GEAR_COUNT) {
        gear = EC_HSTIA_GEAR_5K;
    }
    s_hstia_sel        = EC_EIS_HSTIA_GEARS[gear].rtia_sel;
    s_hstia_nom_ohm    = EC_EIS_HSTIA_GEARS[gear].nom_ohm;
    s_hstia_rtia_ohm.Real  = s_hstia_nom_ohm;
    s_hstia_rtia_ohm.Image = 0.0f;
}

static int32_t eis_sign_extend_dft(uint32_t raw)
{
    int32_t v = (int32_t)(raw & 0x3FFFFu);
    if (v & 0x20000) {
        v |= (int32_t)0xFFFC0000;
    }
    return v;
}

static int32_t eis_ohm_to_mohm(float value_ohm)
{
    if (!isfinite(value_ohm)) {
        return 0;
    }

    float value_mohm = value_ohm * 1000.0f;
    if (value_mohm > (float)INT32_MAX) return INT32_MAX;
    if (value_mohm < (float)INT32_MIN) return INT32_MIN;
    return (int32_t)lroundf(value_mohm);
}

static FreqParams_Type eis_freq_params(uint32_t freq_mHz)
{
    float freq_hz = (freq_mHz ? (float)freq_mHz : (float)EIS_FREQ_FALLBACK_MILLIHZ) / 1000.0f;
    return AD5940_GetFreqParameters(freq_hz);
}

static uint32_t eis_timeout_ms(uint32_t freq_mHz)
{
    if (freq_mHz == 0u) freq_mHz = EIS_FREQ_FALLBACK_MILLIHZ;
    uint32_t timeout_ms = (uint32_t)((uint64_t)5 * 1000000ULL / freq_mHz) + 200u;
    if (timeout_ms > 10000u) timeout_ms = 10000u;
    if (timeout_ms < 500u) timeout_ms = 500u;
    return timeout_ms;
}

static uint32_t eis_hsdac_amp_code(uint16_t e_ac_mV)
{
    uint32_t amp_code = ((uint32_t)e_ac_mV * 2047u + 400u) / 800u;
    return (amp_code > 2047u) ? 2047u : amp_code;
}

static int32_t eis_hsdac_offset_code(int16_t e_dc_mV)
{
    int32_t dc_code = ((int32_t)e_dc_mV * 2047) / 800;
    if (dc_code >  2047) dc_code =  2047;
    if (dc_code < -2048) dc_code = -2048;
    return dc_code;
}

static void eis_fill_hstia_cfg(HSTIACfg_Type *tia)
{
    memset(tia, 0, sizeof(*tia));
    tia->HstiaBias    = HSTIABIAS_1P1;
    tia->HstiaCtia    = 31;
    tia->HstiaRtiaSel = s_hstia_sel;
    tia->DiodeClose   = bFALSE;
    tia->HstiaDeRtia  = HSTIADERTIA_OPEN;
    tia->HstiaDeRload = HSTIADERLOAD_OPEN;
    tia->HstiaDe1Rtia = HSTIADERTIA_OPEN;
    tia->HstiaDe1Rload = HSTIADERLOAD_OPEN;
}

static int eis_wait_dft_ready(uint32_t timeout_ms)
{
    uint32_t t0 = k_uptime_get_32();
    while (!AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_DFTRDY) &&
           !AD5940_INTCTestFlag(AFEINTC_1, AFEINTSRC_DFTRDY)) {
        if (k_uptime_get_32() - t0 > timeout_ms) {
            uint32_t f0 = AD5940_INTCGetFlag(AFEINTC_0);
            uint32_t f1 = AD5940_INTCGetFlag(AFEINTC_1);
            uint32_t c0 = AD5940_INTCGetCfg(AFEINTC_0);
            uint32_t c1 = AD5940_INTCGetCfg(AFEINTC_1);
            uint32_t afe = AD5940_ReadReg(REG_AFE_AFECON);
            LOG_ERR("EIS: DFTRDY timeout flags f0=0x%08x f1=0x%08x cfg0=0x%08x cfg1=0x%08x AFECON=0x%08x",
                    (unsigned)f0, (unsigned)f1, (unsigned)c0, (unsigned)c1, (unsigned)afe);
            return -ETIMEDOUT;
        }
    }
    return 0;
}

static int eis_take_dft(uint32_t mux_p, uint32_t mux_n, uint32_t timeout_ms,
                        iImpCar_Type *dft)
{
    AD5940_ADCMuxCfgS(mux_p, mux_n);
    AD5940_INTCClrFlag(AFEINTSRC_DFTRDY);
    AD5940_Delay10us(25);
    AD5940_AFECtrlS(AFECTRL_ADCCNV | AFECTRL_DFT, bTRUE);

    int err = eis_wait_dft_ready(timeout_ms);

    AD5940_AFECtrlS(AFECTRL_ADCCNV | AFECTRL_DFT, bFALSE);
    AD5940_INTCClrFlag(AFEINTSRC_DFTRDY);

    if (err) {
        return err;
    }

    dft->Real  = eis_sign_extend_dft(AD5940_ReadAfeResult(AFERESULT_DFTREAL));
    dft->Image = eis_sign_extend_dft(AD5940_ReadAfeResult(AFERESULT_DFTIMAGE));
    return 0;
}

/* ── HS loop programming for 3-wire impedance ────────────────────────── */

static void eis_program_hsloop(uint32_t freq_mHz, uint16_t e_ac_mV, int16_t eis_e_dc_mV)
{
    FreqParams_Type freq_params = eis_freq_params(freq_mHz);
    HSLoopCfg_Type hs;
    memset(&hs, 0, sizeof(hs));

    /* 3-wire impedance switch matrix (see datasheet Fig. 73). */
    hs.SWMatCfg.Dswitch = SWD_CE0;
    hs.SWMatCfg.Pswitch = SWP_RE0 | SWP_PL;
    hs.SWMatCfg.Nswitch = SWN_SE0 | SWN_NL;
    hs.SWMatCfg.Tswitch = SWT_SE0LOAD | SWT_TRTIA;

    /* HSDAC: full-rate update, gain 1, excitation buffer ÷4 then ×1 path */
    hs.HsDacCfg.ExcitBufGain    = EXCITBUFGAIN_0P25;   /* peak ≈ 200 mV */
    hs.HsDacCfg.HsDacGain       = HSDACGAIN_1;
    hs.HsDacCfg.HsDacUpdateRate = 7;                   /* fastest update */

    /* HSTIA: bias to internal 1.1 V, calibrated 5 kΩ feedback path. */
    eis_fill_hstia_cfg(&hs.HsTiaCfg);

    /*
     * Sine amplitude: HSDAC 12-bit (0..2047 peak) × ExcitBufGain=0.25
     * gives V_peak_at_SE = code/2047 × 800 mV (full-scale ±800 mV) × 0.25.
     * Reverse: code = e_ac_mV * 2047 / 800.  Clamp to 2047.
     *
     * DC offset (e_dc_mV) is applied via SinOffsetWord (signed 12-bit,
     * range ±800 mV full-scale).  code_dc = e_dc_mV * 2047 / 800.
     */
    /* Sine wave generator. */
    hs.WgCfg.WgType                  = WGTYPE_SIN;
    hs.WgCfg.GainCalEn               = bTRUE;
    hs.WgCfg.OffsetCalEn             = bTRUE;
    hs.WgCfg.SinCfg.SinFreqWord      =
        AD5940_WGFreqWordCal((float)freq_mHz / 1000.0f, EC_PROXY_SYSCLK_HZ);
    hs.WgCfg.SinCfg.SinAmplitudeWord = eis_hsdac_amp_code(e_ac_mV);
    hs.WgCfg.SinCfg.SinOffsetWord    = eis_hsdac_offset_code(eis_e_dc_mV);
    hs.WgCfg.SinCfg.SinPhaseWord     = 0;

    AD5940_HSLoopCfgS(&hs);

    ADCBaseCfg_Type adc_base = {
        .ADCMuxP = ADCMUXP_P_NODE,
        .ADCMuxN = ADCMUXN_N_NODE,
        .ADCPga  = ADCPGA_1P5,
    };
    AD5940_ADCBaseCfgS(&adc_base);

    ADCFilterCfg_Type flt = {
        .ADCSinc3Osr         = freq_params.ADCSinc3Osr,
        .ADCSinc2Osr         = freq_params.ADCSinc2Osr,
        .ADCAvgNum           = ADCAVGNUM_2,
        .ADCRate             = freq_params.HighPwrMode ? ADCRATE_1P6MHZ : ADCRATE_800KHZ,
        .BpNotch             = bTRUE,
        .BpSinc3             = bFALSE,
        .Sinc3ClkEnable      = bTRUE,
        .Sinc2NotchClkEnable = bTRUE,
        .Sinc2NotchEnable    = bTRUE,
    };
    AD5940_ADCFilterCfgS(&flt);

    /* Configure DFT so the capture window spans enough cycles at this frequency. */
    DFTCfg_Type dft = {
        .DftNum  = freq_params.DftNum,
        .DftSrc  = freq_params.DftSrc,
        .HanWinEn = bTRUE,
    };
    AD5940_DFTCfgS(&dft);

    /* AUTOSET BW so the SINC / DFT chain tracks excitation freq. */
    AD5940_AFEPwrBW(freq_params.HighPwrMode ? AFEPWR_HP : AFEPWR_LP, AFEBW_AUTOSET);
    AD5940_HPModeEn(freq_params.HighPwrMode ? bTRUE : bFALSE);
}

static int eis_calibrate_hsdac(uint32_t afe_pwr_mode)
{
    HSDACCal_Type cal;
    AD5940_StructInit(&cal, sizeof(cal));
    cal.fRcal = EC_PROXY_RCAL_OHM;
    cal.SysClkFreq = EC_PROXY_SYSCLK_HZ;
    cal.AdcClkFreq = EC_PROXY_ADCCLK_HZ;
    cal.AfePwrMode = afe_pwr_mode;
    cal.ExcitBufGain = EXCITBUFGAIN_0P25;
    cal.HsDacGain = HSDACGAIN_1;
    cal.ADCSinc3Osr = ADCSINC3OSR_4;
    cal.ADCSinc2Osr = ADCSINC2OSR_22;

    AD5940Err err = AD5940_HSDACCal(&cal);
    if (err != AD5940ERR_OK) {
        LOG_ERR("EIS HSDAC calibration failed (%s): %d",
                afe_pwr_mode == AFEPWR_HP ? "HP" : "LP", (int)err);
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3601u,
                             "EIS HSDAC calibration failed: %d", (int)err);
        return -EIO;
    }

    LOG_INF("EIS HSDAC calibration complete (%s)",
            afe_pwr_mode == AFEPWR_HP ? "HP" : "LP");
    return 0;
}

static void eis_program_rtia_cal_loop(uint32_t freq_mHz)
{
    FreqParams_Type freq_params = eis_freq_params(freq_mHz);
    HSLoopCfg_Type hs;
    memset(&hs, 0, sizeof(hs));

    hs.SWMatCfg.Dswitch = SWD_RCAL0;
    hs.SWMatCfg.Pswitch = SWP_RCAL0;
    hs.SWMatCfg.Nswitch = SWN_RCAL1;
    hs.SWMatCfg.Tswitch = SWT_RCAL1 | SWT_TRTIA | SWT_AIN1;

    hs.HsDacCfg.ExcitBufGain    = EXCITBUFGAIN_0P25;
    hs.HsDacCfg.HsDacGain       = HSDACGAIN_1;
    hs.HsDacCfg.HsDacUpdateRate = 7;
    eis_fill_hstia_cfg(&hs.HsTiaCfg);

    hs.WgCfg.WgType                  = WGTYPE_SIN;
    hs.WgCfg.GainCalEn               = bTRUE;
    hs.WgCfg.OffsetCalEn             = bTRUE;
    hs.WgCfg.SinCfg.SinFreqWord      =
        AD5940_WGFreqWordCal((float)freq_mHz / 1000.0f, EC_PROXY_SYSCLK_HZ);
    hs.WgCfg.SinCfg.SinAmplitudeWord = eis_hsdac_amp_code(EIS_CAL_EAC_MV);
    hs.WgCfg.SinCfg.SinOffsetWord    = 0;
    hs.WgCfg.SinCfg.SinPhaseWord     = 0;

    AD5940_HSLoopCfgS(&hs);

    ADCBaseCfg_Type adc_base = {
        .ADCMuxP = ADCMUXP_P_NODE,
        .ADCMuxN = ADCMUXN_N_NODE,
        .ADCPga  = ADCPGA_1P5,
    };
    AD5940_ADCBaseCfgS(&adc_base);

    ADCFilterCfg_Type flt = {
        .ADCSinc3Osr         = freq_params.ADCSinc3Osr,
        .ADCSinc2Osr         = freq_params.ADCSinc2Osr,
        .ADCAvgNum           = ADCAVGNUM_2,
        .ADCRate             = freq_params.HighPwrMode ? ADCRATE_1P6MHZ : ADCRATE_800KHZ,
        .BpNotch             = bTRUE,
        .BpSinc3             = bFALSE,
        .Sinc3ClkEnable      = bTRUE,
        .Sinc2NotchClkEnable = bTRUE,
        .Sinc2NotchEnable    = bTRUE,
    };
    AD5940_ADCFilterCfgS(&flt);

    DFTCfg_Type dft = {
        .DftNum  = freq_params.DftNum,
        .DftSrc  = freq_params.DftSrc,
        .HanWinEn = bTRUE,
    };
    AD5940_DFTCfgS(&dft);

    AD5940_AFEPwrBW(freq_params.HighPwrMode ? AFEPWR_HP : AFEPWR_LP, AFEBW_AUTOSET);
    AD5940_HPModeEn(freq_params.HighPwrMode ? bTRUE : bFALSE);
}

static int eis_calibrate_hstia_rtia(uint32_t cal_freq_mHz)
{
    if (cal_freq_mHz == 0u) cal_freq_mHz = EIS_FREQ_FALLBACK_MILLIHZ;
    uint32_t timeout_ms = eis_timeout_ms(cal_freq_mHz);

    AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_DFTRDY | AFEINTSRC_SINC2RDY, bTRUE);
    AD5940_INTCCfg(AFEINTC_1, AFEINTSRC_DFTRDY | AFEINTSRC_SINC2RDY, bTRUE);
    AD5940_INTCClrFlag(AFEINTSRC_DFTRDY | AFEINTSRC_SINC2RDY);

    eis_program_rtia_cal_loop(cal_freq_mHz);
    AD5940_AFECtrlS(AFECTRL_HSTIAPWR | AFECTRL_INAMPPWR |
                    AFECTRL_EXTBUFPWR | AFECTRL_DACREFPWR |
                    AFECTRL_HSDACPWR | AFECTRL_SINC2NOTCH |
                    AFECTRL_ADCPWR | AFECTRL_WG, bTRUE);
    AD5940_Delay10us(25);

    iImpCar_Type dft_rcal = {0};
    iImpCar_Type dft_rtia = {0};
    int err = eis_take_dft(ADCMUXP_P_NODE, ADCMUXN_N_NODE, timeout_ms, &dft_rcal);
    if (!err) {
        err = eis_take_dft(ADCMUXP_HSTIA_P, ADCMUXN_HSTIA_N, timeout_ms, &dft_rtia);
    }
    AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);

    if (err) {
        LOG_ERR("EIS HSTIA RTIA calibration DFT timeout at %u mHz",
                (unsigned)cal_freq_mHz);
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3602u,
                             "EIS RTIA calibration timeout at %u Hz",
                             (unsigned)(cal_freq_mHz / 1000u));
        return err;
    }

    if (dft_rcal.Real == 0 && dft_rcal.Image == 0) {
        LOG_ERR("EIS HSTIA RTIA calibration got zero RCAL DFT");
        (void)ble_log_event(BLE_LOG_LEVEL_ERROR, 0x3603u,
                            "EIS RTIA calibration zero RCAL signal");
        return -EIO;
    }

    dft_rcal.Image = -dft_rcal.Image;
    dft_rtia.Real  = -dft_rtia.Real;

    fImpCar_Type ratio = AD5940_ComplexDivInt(&dft_rtia, &dft_rcal);
    fImpCar_Type rtia = {
        .Real = ratio.Real * EC_PROXY_RCAL_OHM,
        .Image = ratio.Image * EC_PROXY_RCAL_OHM,
    };
    float mag = AD5940_ComplexMag(&rtia);
    /* Accept a wide band around the selected gear's nominal value. */
    if (!(mag > s_hstia_nom_ohm * 0.25f && mag < s_hstia_nom_ohm * 4.0f)) {
        LOG_ERR("EIS HSTIA RTIA calibration out of range: %d ohm + j%d ohm",
                (int)rtia.Real, (int)rtia.Image);
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3604u,
                             "EIS RTIA calibration out of range: %d ohm",
                             (int)mag);
        return -ERANGE;
    }

    s_hstia_rtia_ohm = rtia;
    LOG_INF("EIS HSTIA RTIA calibrated: %d ohm + j%d ohm at %u Hz",
            (int)s_hstia_rtia_ohm.Real, (int)s_hstia_rtia_ohm.Image,
            (unsigned)(cal_freq_mHz / 1000u));
    return 0;
}

static int eis_calibrate_frontend(void)
{
    int err = eis_calibrate_hsdac(AFEPWR_LP);
    if (err) return err;

    err = eis_calibrate_hsdac(AFEPWR_HP);
    if (err) return err;

    return 0;
}

static int eis_prepare_frequency(uint32_t freq_mHz)
{
    int err = eis_calibrate_hstia_rtia(freq_mHz);
    if (err) return err;

    AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_DFTRDY | AFEINTSRC_SINC2RDY, bTRUE);
    AD5940_INTCCfg(AFEINTC_1, AFEINTSRC_DFTRDY | AFEINTSRC_SINC2RDY, bTRUE);
    AD5940_INTCClrFlag(AFEINTSRC_DFTRDY | AFEINTSRC_SINC2RDY);

    eis_program_hsloop(freq_mHz, s_p.e_ac_mV, s_p.e_dc_mV);
    AD5940_AFECtrlS(AFECTRL_HPREFPWR | AFECTRL_INAMPPWR | AFECTRL_EXTBUFPWR
                    | AFECTRL_DACREFPWR | AFECTRL_HSDACPWR |
                    AFECTRL_HSTIAPWR | AFECTRL_SINC2NOTCH |
                    AFECTRL_WG, bTRUE);
    AD5940_Delay10us(25);
    return 0;
}

/* ---- tick ---------------------------------------------------------------- */

ec_tick_status_t ec_proxy_eis_tick(ec_tick_out_t *out)
{
    /*
     * Measure two complex voltages at the same frequency:
     *   Vdut  = ADC(P_NODE, N_NODE)
     *   Vrtia = ADC(HSTIA_P, HSTIA_N)
     * Then Zdut = Vdut / (Vrtia / RTIA).
     */
    uint32_t timeout_ms = eis_timeout_ms(s_freq_now_mHz);

    AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_SINC2NOTCH, bTRUE);
    iImpCar_Type dft_dut = {0};
    iImpCar_Type dft_rtia = {0};
    int err = eis_take_dft(ADCMUXP_P_NODE, ADCMUXN_N_NODE, timeout_ms, &dft_dut);
    if (!err) {
        err = eis_take_dft(ADCMUXP_HSTIA_P, ADCMUXN_HSTIA_N, timeout_ms, &dft_rtia);
    }
    AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_DFT | AFECTRL_ADCCNV, bFALSE);

    if (err) {
        out->frame_type = EC_FRAME_ERROR;
        out->payload[0] = (uint8_t)(-ETIMEDOUT);
        out->len = 1u;
        LOG_ERR("EIS: DFTRDY timeout at %u mHz", (unsigned)s_freq_now_mHz);
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3301u,
                             "EIS DFT timeout at %u Hz",
                             (unsigned)(s_freq_now_mHz / 1000u));
        AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);
        mux_disable();
        return EC_TICK_DONE;
    }

    if (dft_rtia.Real == 0 && dft_rtia.Image == 0) {
        out->frame_type = EC_FRAME_ERROR;
        out->payload[0] = (uint8_t)(-EIO);
        out->len = 1u;
        LOG_ERR("EIS: zero RTIA DFT at %u mHz", (unsigned)s_freq_now_mHz);
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3302u,
                             "EIS RTIA signal is zero at %u Hz",
                             (unsigned)(s_freq_now_mHz / 1000u));
        AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);
        mux_disable();
        return EC_TICK_DONE;
    }

    /*
     * Match ADI's HSRtiaCal sign handling: DFT imaginary values are inverted
     * by the engine, and the HSTIA current-sense polarity is opposite to the
     * DUT voltage measurement.
     */
    dft_dut.Image = -dft_dut.Image;
    dft_rtia.Real = -dft_rtia.Real;

    fImpCar_Type z_ratio = AD5940_ComplexDivInt(&dft_dut, &dft_rtia);
    fImpCar_Type z_ohm = AD5940_ComplexMulFloat(&z_ratio, &s_hstia_rtia_ohm);

    struct __packed {
        uint32_t freq_mHz;
        int32_t  z_real_mOhm;
        int32_t  z_imag_mOhm;
    } *sample = (void *)out->payload;
    sample->freq_mHz    = s_freq_now_mHz;
    sample->z_real_mOhm = eis_ohm_to_mohm(z_ohm.Real);
    sample->z_imag_mOhm = eis_ohm_to_mohm(z_ohm.Image);
    out->len        = (uint16_t)sizeof(*sample);
    out->frame_type = EC_FRAME_SAMPLE;

    s_sample_cnt++;
    LOG_INF("[REC][EIS] n=%u f=%umHz Z=(%d,%d)mOhm raw V=(%d,%d) I=(%d,%d)",
            (unsigned)s_sample_cnt,
            (unsigned)sample->freq_mHz,
            sample->z_real_mOhm,
            sample->z_imag_mOhm,
            (int)dft_dut.Real,
            (int)dft_dut.Image,
            (int)dft_rtia.Real,
            (int)dft_rtia.Image);

    /* Geometric step in the requested direction. */
    s_freq_now_mHz = (uint32_t)((float)s_freq_now_mHz * s_freq_step_mult + 0.5f);

    bool done = s_freq_descending ? (s_freq_now_mHz < s_p.freq_end_mHz)
                                  : (s_freq_now_mHz > s_p.freq_end_mHz);
    if (done) {
        AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);
        mux_disable();
        LOG_INF("[REC][EIS] done, points=%u", (unsigned)s_sample_cnt);
        return EC_TICK_DONE;
    }

    err = eis_prepare_frequency(s_freq_now_mHz);
    if (err) {
        out->frame_type = EC_FRAME_ERROR;
        out->payload[0] = (uint8_t)err;
        out->len = 1u;
        AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);
        mux_disable();
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3605u,
                             "EIS prepare failed at %u Hz",
                             (unsigned)(s_freq_now_mHz / 1000u));
        return EC_TICK_DONE;
    }

    /*
     * Wait at least cycles_avg full cycles of the NEXT frequency before
     * sampling. This gives the DFT integrator time to settle.
     *
     * period_us = cycles_avg * 1e9 / freq_mHz
     *           (freq_mHz = freq_Hz * 1000,  1 Hz period = 1e6 us)
     *
     * Cap at 300 s to prevent absurd delays on sub-mHz start frequencies.
     */
    uint8_t  avg = s_p.cycles_avg ? s_p.cycles_avg : 3u;
    uint64_t period_us = (uint64_t)avg * 1000000000ULL / s_freq_now_mHz;
    if (period_us > 300000000ULL) period_us = 300000000ULL;  /* 300 s cap */
    out->delay_us = (uint32_t)period_us;
    return EC_TICK_CONTINUE;
}

int ec_proxy_eis_start(const uint8_t *params, uint16_t len, uint32_t *first_delay_us)
{
    if (len < sizeof(ec_eis_params_t)) return -EINVAL;
    int err = ec_proxy_bus_prepare_experiment("EIS");
    if (err) return err;

    memcpy(&s_p, params, sizeof(s_p));
    eis_set_hstia_gear(s_p.hstia_gear);
    err = mux_select_we(0xFF);
    if (err) return err;

    LOG_INF("EIS proxy WE%u: Edc=%d Eac=%u %u..%u mHz ppd=%u avg=%u (HS loop only, RTIA=%dohm)",
            s_p.we_index, s_p.e_dc_mV, s_p.e_ac_mV,
            s_p.freq_start_mHz, s_p.freq_end_mHz,
            s_p.points_per_dec, s_p.cycles_avg, (int)s_hstia_nom_ohm);

    err = eis_calibrate_frontend();
    if (err) {
        mux_disable();
        return err;
    }

    s_freq_now_mHz = s_p.freq_start_mHz ? s_p.freq_start_mHz : EIS_FREQ_FALLBACK_MILLIHZ;
    s_sample_cnt   = 0;

    uint8_t ppd = s_p.points_per_dec ? s_p.points_per_dec : 5u;
    float step_mult = powf(10.0f, 1.0f / (float)ppd);
    s_freq_descending = (s_p.freq_start_mHz > s_p.freq_end_mHz);
    s_freq_step_mult = s_freq_descending ? (1.0f / step_mult) : step_mult;

    err = mux_select_we(s_p.we_index);
    if (err) return err;

    err = eis_prepare_frequency(s_freq_now_mHz);
    if (err) {
        mux_disable();
        return err;
    }

    /* Initial settle: one full cycle of the start frequency (min 10 ms). */
    uint8_t  avg = s_p.cycles_avg ? s_p.cycles_avg : 3u;
    uint64_t settle_us = (uint64_t)avg * 1000000000ULL / s_freq_now_mHz;
    if (settle_us < 10000ULL)  settle_us = 10000ULL;
    if (settle_us > 300000000ULL) settle_us = 300000000ULL;
    *first_delay_us = (uint32_t)settle_us;
    return 0;
}

void ec_proxy_eis_stop(void)
{
    AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);
    mux_disable();
    LOG_INF("[REC][EIS] stopped (external), points=%u", (unsigned)s_sample_cnt);
}
