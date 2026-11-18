/*
 * Shared LP amperometry setup for CV/CA.
 *
 * Board topology:
 *   CE  -> AD5941 CE0
 *   RE  -> AD5941 RE0
 *   WE0 -> AD5941 SE0 directly
 *
 * Current is measured at the LPTIA output, not directly at the SE0 pin.
 */

#include "ec_proxy.h"
#include "ec_proxy_lpamp.h"
#include "../inc/ec_params.h"

#include <errno.h>
#include <math.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ec_proxy_lpamp, LOG_LEVEL_INF);

#ifndef ECW_AUTOTEST_ENABLE
#define ECW_AUTOTEST_ENABLE 0
#endif

/*
 * LPTIA current-range gear table. Index = enum ec_rtia_gear_t (host-selected
 * via the rtia_gear byte in the CV/CA/DPV START params). Each entry pairs the
 * AD5940 LPTIARTIA_* code with its nominal resistance, used both to program
 * the TIA and to seed/bound RTIA self-calibration.
 */
static const struct {
    uint32_t rtia_sel;
    float    nom_ohm;
} EC_LPAMP_RTIA_GEARS[EC_RTIA_GEAR_COUNT] = {
    [EC_RTIA_GEAR_200R] = { LPTIARTIA_200R, 200.0f    },
    [EC_RTIA_GEAR_1K]   = { LPTIARTIA_1K,   1000.0f   },
    [EC_RTIA_GEAR_4K]   = { LPTIARTIA_4K,   4000.0f   },
    [EC_RTIA_GEAR_10K]  = { LPTIARTIA_10K,  10000.0f  },
    [EC_RTIA_GEAR_20K]  = { LPTIARTIA_20K,  20000.0f  },
    [EC_RTIA_GEAR_100K] = { LPTIARTIA_100K, 100000.0f },
    [EC_RTIA_GEAR_256K] = { LPTIARTIA_256K, 256000.0f },
    [EC_RTIA_GEAR_512K] = { LPTIARTIA_512K, 512000.0f },
};

/* Active gear (defaults to gear 0). Set via ec_lpamp_set_rtia_gear(). */
static uint32_t s_rtia_sel     = LPTIARTIA_200R;
static float    s_rtia_nom_ohm = 200.0f;

/* Calibrated RTIA magnitude (filled by ec_lpamp_calibrate_rtia()). */
static float s_rtia_ohm = 200.0f;

void ec_lpamp_set_rtia_gear(uint8_t gear)
{
    if (gear >= EC_RTIA_GEAR_COUNT) {
        LOG_WRN("RTIA gear %u out of range, using gear 0 (200R)", gear);
        gear = EC_RTIA_GEAR_200R;
    }
    s_rtia_sel     = EC_LPAMP_RTIA_GEARS[gear].rtia_sel;
    s_rtia_nom_ohm = EC_LPAMP_RTIA_GEARS[gear].nom_ohm;
    s_rtia_ohm     = s_rtia_nom_ohm;
    LOG_INF("RTIA gear %u selected: nominal %d ohm", gear, (int)s_rtia_nom_ohm);
}

uint16_t ec_lpamp_mV_to_dac12(int16_t e_mV)
{
    /*
     * Topology: WE is held at Vzero by the LPTIA virtual ground, while the
     * LPPA drives CE so that RE tracks Vbias (12-bit LPDAC). The electrode
     * potential is E = V(WE) - V(RE) = Vzero - Vbias. To apply E = e_mV we
     * must therefore set Vbias = Vzero - e_mV (note the minus sign).
     */
    int32_t code = EC_LPAMP_VZERO_CODE6 * 64 - (int32_t)lroundf(e_mV * 4095.0f / 2200.0f);
    if (code < EC_LPAMP_VZERO_CODE6 * 64) code--;
    if (code < 0) {
        code = 0;
    }
    if (code > 4095) {
        code = 4095;
    }
    return (uint16_t)code;
}

void ec_lpamp_write_dac(int16_t e_mV)
{
    LPDACCfg_Type dac;
    uint16_t code12 = ec_lpamp_mV_to_dac12(e_mV);

    AD5940_StructInit(&dac, sizeof(dac));
    dac.LpdacSel      = LPDAC0;
    dac.LpDacSrc      = LPDACSRC_MMR;
    dac.LpDacVzeroMux = LPDACVZERO_6BIT;
    dac.LpDacVbiasMux = LPDACVBIAS_12BIT;
    dac.LpDacRef      = LPDACREF_2P5;
    dac.DataRst       = bFALSE;
    dac.LpDacSW       = LPDACSW_VBIAS2LPPA |
                        LPDACSW_VZERO2LPTIA |
                        LPDACSW_VZERO2PIN;
    dac.DacData12Bit  = code12;
    dac.DacData6Bit   = EC_LPAMP_VZERO_CODE6;
    dac.PowerEn       = bTRUE;

    AD5940_LPDACCfgS(&dac);

#if ECW_AUTOTEST_ENABLE
    LOG_INF("LPDAC write E=%d mV code12=%u DAT0=0x%05x CON0=0x%02x SW0=0x%02x",
            (int)e_mV, code12,
            (unsigned)AD5940_ReadReg(REG_AFE_LPDACDAT0),
            (unsigned)AD5940_ReadReg(REG_AFE_LPDACCON0),
            (unsigned)AD5940_ReadReg(REG_AFE_LPDACSW0));
#endif
}

static int ec_lpamp_calibrate_rtia(void)
{
    fImpPol_Type rtia = {
        .Magnitude = s_rtia_nom_ohm,
        .Phase = 0.0f,
    };
    LPRTIACal_Type cal;

    AD5940_StructInit(&cal, sizeof(cal));
    cal.LpAmpSel = LPAMP0;
    cal.bPolarResult = bTRUE;
    cal.AdcClkFreq = EC_PROXY_ADCCLK_HZ;
    cal.SysClkFreq = EC_PROXY_SYSCLK_HZ;
    cal.ADCSinc3Osr = ADCSINC3OSR_4;
    cal.ADCSinc2Osr = ADCSINC2OSR_22;
    cal.DftCfg.DftNum = DFTNUM_2048;
    cal.DftCfg.DftSrc = DFTSRC_SINC2NOTCH;
    cal.DftCfg.HanWinEn = bTRUE;
    cal.fFreq = EC_PROXY_ADCCLK_HZ / 4.0f / 22.0f / 2048.0f * 3.0f;
    cal.fRcal = EC_PROXY_RCAL_OHM;
    cal.LpTiaRtia = s_rtia_sel;
    cal.LpAmpPwrMod = LPAMPPWR_NORM;
    cal.bWithCtia = bFALSE;

    AD5940Err err = AD5940_LPRtiaCal(&cal, &rtia);
    if (err != AD5940ERR_OK) {
        LOG_ERR("LPTIA RTIA calibration failed: %d", (int)err);
        return -EIO;
    }
    /* Accept calibration within ~[0.4x, 2.5x] of the selected nominal value. */
    float lo = s_rtia_nom_ohm * 0.4f;
    float hi = s_rtia_nom_ohm * 2.5f;
    if (!(rtia.Magnitude > lo && rtia.Magnitude < hi)) {
        LOG_ERR("LPTIA RTIA calibration out of range: %d ohm (nominal %d)",
                (int)rtia.Magnitude, (int)s_rtia_nom_ohm);
        return -ERANGE;
    }

    s_rtia_ohm = rtia.Magnitude;
    LOG_INF("LPTIA RTIA calibrated: %d ohm, phase=%d mdeg",
            (int)s_rtia_ohm, (int)(rtia.Phase * 1000.0f));
    return 0;
}

static void ec_lpamp_configure_measurement_path(void)
{
    AFERefCfg_Type ref;
    LPLoopCfg_Type lp_loop;
    DSPCfg_Type dsp;
    HSLoopCfg_Type hs_loop;

    AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);

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

    AD5940_StructInit(&lp_loop, sizeof(lp_loop));
    lp_loop.LpAmpCfg.LpAmpSel = LPAMP0;
    lp_loop.LpAmpCfg.LpAmpPwrMod = LPAMPPWR_BOOST3;
    lp_loop.LpAmpCfg.LpPaPwrEn = bTRUE;
    lp_loop.LpAmpCfg.LpTiaPwrEn = bTRUE;
    lp_loop.LpAmpCfg.LpTiaRf = LPTIARF_20K;
    /* Match AD5940_LPRtiaCal: RTIA depends on RLOAD (datasheet table 21). */
    lp_loop.LpAmpCfg.LpTiaRload = LPTIARLOAD_100R;
    lp_loop.LpAmpCfg.LpTiaRtia = s_rtia_sel;
    lp_loop.LpAmpCfg.LpTiaSW = LPTIASW(2) | LPTIASW(4) | LPTIASW(5);
    lp_loop.LpDacCfg.LpdacSel = LPDAC0;
    lp_loop.LpDacCfg.LpDacSrc = LPDACSRC_MMR;
    lp_loop.LpDacCfg.LpDacVzeroMux = LPDACVZERO_6BIT;
    lp_loop.LpDacCfg.LpDacVbiasMux = LPDACVBIAS_12BIT;
    lp_loop.LpDacCfg.LpDacRef = LPDACREF_2P5;
    lp_loop.LpDacCfg.DataRst = bFALSE;
    lp_loop.LpDacCfg.LpDacSW = LPDACSW_VBIAS2LPPA |
                                LPDACSW_VZERO2LPTIA |
                                LPDACSW_VZERO2PIN;
    lp_loop.LpDacCfg.DacData12Bit = ec_lpamp_mV_to_dac12(0);
    lp_loop.LpDacCfg.DacData6Bit = EC_LPAMP_VZERO_CODE6;
    lp_loop.LpDacCfg.PowerEn = bTRUE;
    AD5940_LPLoopCfgS(&lp_loop);

    AD5940_StructInit(&dsp, sizeof(dsp));
    dsp.ADCBaseCfg.ADCMuxP = ADCMUXP_LPTIA0_P;
    dsp.ADCBaseCfg.ADCMuxN = ADCMUXN_LPTIA0_N;
    dsp.ADCBaseCfg.ADCPga = EC_LPAMP_ADC_PGA;
    dsp.ADCFilterCfg.ADCSinc3Osr = ADCSINC3OSR_4;
    dsp.ADCFilterCfg.ADCSinc2Osr = ADCSINC2OSR_22;
    dsp.ADCFilterCfg.ADCAvgNum = ADCAVGNUM_2;
    dsp.ADCFilterCfg.ADCRate = ADCRATE_800KHZ;
    dsp.ADCFilterCfg.BpNotch = bTRUE;
    dsp.ADCFilterCfg.BpSinc3 = bFALSE;
    dsp.ADCFilterCfg.Sinc3ClkEnable = bTRUE;
    dsp.ADCFilterCfg.Sinc2NotchClkEnable = bTRUE;
    dsp.ADCFilterCfg.Sinc2NotchEnable = bTRUE;
    AD5940_DSPCfgS(&dsp);

    AD5940_StructInit(&hs_loop, sizeof(hs_loop));
    hs_loop.SWMatCfg.Dswitch = SWD_OPEN;
    hs_loop.SWMatCfg.Pswitch = SWP_OPEN;
    hs_loop.SWMatCfg.Nswitch = SWN_OPEN;
    hs_loop.SWMatCfg.Tswitch = SWT_OPEN;
    AD5940_HSLoopCfgS(&hs_loop);

    AD5940_ADCPowerCtrlS(bTRUE);
    AD5940_AFECtrlS(AFECTRL_HPREFPWR | AFECTRL_DACREFPWR |
                    AFECTRL_SINC2NOTCH, bTRUE);
    AD5940_Delay10us(25);
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY | AFEINTSRC_DFTRDY);
}

int ec_lpamp_calibrate_and_configure(const char *tag)
{
    int err = ec_lpamp_calibrate_rtia();
    if (err) {
        return err;
    }

    ec_lpamp_configure_measurement_path();
    LOG_INF("%s LP amperometry path: CE0/RE0/SE0, ADC=LPTIA0_P/N, RTIA=%d ohm",
            tag ? tag : "EC", (int)s_rtia_ohm);
    return 0;
}

float ec_lpamp_rtia_ohm(void)
{
    return s_rtia_ohm;
}

void ec_lpamp_shutdown(void)
{
    /* Internal switches open and both LP amplifiers off. No external relay. */
    LPAmpCfg_Type amp;
    LPDACCfg_Type dac;
    SWMatrixCfg_Type sw;
    AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);
    AD5940_StructInit(&amp, sizeof(amp));
    amp.LpAmpSel = LPAMP0;
    AD5940_LPAMPCfgS(&amp);
    AD5940_StructInit(&dac, sizeof(dac));
    dac.LpdacSel = LPDAC0;
    AD5940_LPDACCfgS(&dac);
    AD5940_StructInit(&sw, sizeof(sw));
    AD5940_SWMatrixCfgS(&sw);
}
