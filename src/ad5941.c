#include "ecw.h"
#include "ad5940.h"
#include <zephyr/kernel.h>
#include <math.h>
#include <stdlib.h>
static atomic_t ready;
static uint16_t chip_id;
static float rtia_actual = 10000.0f;
static int32_t rtia_nominal = 10000;
static float avdd_actual = 3.3f;
static int adc_volt(uint32_t p, uint32_t n, float *volt, uint8_t *flags);
/* The library uses the same protocol/register map for AD5940 and AD5941. */
static const struct { int32_t ohm; uint32_t code; } rtias[] = {
    {200,LPTIARTIA_200R},{1000,LPTIARTIA_1K},{2000,LPTIARTIA_2K},
    {3000,LPTIARTIA_3K},{4000,LPTIARTIA_4K},{6000,LPTIARTIA_6K},
    {8000,LPTIARTIA_8K},{10000,LPTIARTIA_10K},{12000,LPTIARTIA_12K},
    {16000,LPTIARTIA_16K},{20000,LPTIARTIA_20K},{24000,LPTIARTIA_24K},
    {30000,LPTIARTIA_30K},{32000,LPTIARTIA_32K},{40000,LPTIARTIA_40K},
    {48000,LPTIARTIA_48K},{64000,LPTIARTIA_64K},{85000,LPTIARTIA_85K},
    {96000,LPTIARTIA_96K},{100000,LPTIARTIA_100K},{120000,LPTIARTIA_120K},
    {128000,LPTIARTIA_128K},{160000,LPTIARTIA_160K},{196000,LPTIARTIA_196K},
    {256000,LPTIARTIA_256K},{512000,LPTIARTIA_512K}
};
int afe_select_rtia(int32_t ohm)
{
    for (size_t i = 0; i < ARRAY_SIZE(rtias); ++i) {
        if (rtias[i].ohm == ohm) { return rtias[i].code; }
    }
    return -EINVAL;
}
bool afe_ready(void) { return atomic_get(&ready); }
uint16_t afe_chip_id(void) { return chip_id; }
void afe_idle(void)
{
    if (!afe_ready()) { return; }
    if (afe_port_error()) {
        /* A failed SPI bus cannot reliably open electrode switches. Hold the
         * independent hardware RESET# low instead; require a power/reboot cycle. */
        AD5940_RstClr();
        atomic_clear(&ready);
        return;
    }
    AD5940_ADCConvtCtrlS(bFALSE);
    AD5940_AFECtrlS(AFECTRL_ALL, bFALSE);
    SWMatrixCfg_Type open = {0};
    AD5940_SWMatrixCfgS(&open);
    /* All electrode paths open, including SE0 -> TIA and RE0 -> PA. */
    AD5940_WriteReg(REG_AFE_LPTIASW0, 0);
    LPAmpCfg_Type amp = { .LpAmpSel = LPAMP0 };
    AD5940_LPAMPCfgS(&amp);
    LPDACCfg_Type dac = { .LpdacSel = LPDAC0 };
    AD5940_LPDACCfgS(&dac);
    if (afe_port_error()) { AD5940_RstClr(); atomic_clear(&ready); }
}
int afe_init(void)
{
    int rc = afe_port_init();
    if (rc) { return rc; }
    afe_port_clear_error();
    AD5940_RstClr(); k_msleep(5);
    AD5940_RstSet(); k_msleep(20); /* R7/C59 reset release RC = 4.7ms. */
    AD5940_Initialize();
    chip_id = AD5940_ReadReg(REG_AFECON_CHIPID);
    uint32_t adi_id = AD5940_ReadReg(REG_AFECON_ADIID);
    if (afe_port_error()) { return afe_port_error(); }
    if (adi_id != 0x4144 || (chip_id != 0x5501 && chip_id != 0x5502)) {
        return -ENODEV; /* Require Si2/Si3 switch configuration. */
    }
    /* Use internal 16MHz HFOSC. The fitted 16MHz crystal is not required. */
    AD5940_AFEPwrBW(AFEPWR_LP, AFEBW_250KHZ);
    AD5940_INTCCfg(AFEINTC_1, AFEINTSRC_SINC2RDY, bTRUE);
    AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
    atomic_set(&ready, 1);
    afe_idle();
    return afe_port_error();
}
int afe_prepare(bool ocp, int32_t rtia_ohm)
{
    if (!afe_ready()) { return -ENODEV; }
    if (afe_port_error()) { return afe_port_error(); }
    int gain = afe_select_rtia(rtia_ohm);
    if (gain < 0) { return gain; }
    afe_idle();
    AD5940_AFEPwrBW(AFEPWR_LP, AFEBW_250KHZ);
    AFERefCfg_Type ref = {
        .HpBandgapEn=bTRUE, .Hp1V8BuffEn=bTRUE, .Hp1V1BuffEn=bTRUE,
        .LpBandgapEn=bTRUE, .LpRefBufEn=bTRUE,
    };
    AD5940_REFCfgS(&ref);
    SWMatrixCfg_Type sw = {0};
    AD5940_SWMatrixCfgS(&sw);
    if (!ocp) {
        LPLoopCfg_Type lp = {0};
        lp.LpDacCfg = (LPDACCfg_Type) {
            .LpdacSel=LPDAC0, .LpDacSrc=LPDACSRC_MMR,
            .LpDacVzeroMux=LPDACVZERO_6BIT, .LpDacVbiasMux=LPDACVBIAS_12BIT,
            .LpDacSW=LPDACSW_VBIAS2LPPA | LPDACSW_VZERO2LPTIA,
            .LpDacRef=LPDACREF_2P5, .PowerEn=bTRUE,
            .DacData6Bit=32, .DacData12Bit=2048,
        };
        lp.LpAmpCfg = (LPAmpCfg_Type) {
            /* Pin 40 (AIN4/LPF0) is unconnected on this PCB. Keep its analog
             * filter resistor disconnected; read the internal LPTIA output. */
            .LpAmpSel=LPAMP0, .LpTiaRf=LPTIARF_OPEN,
            .LpTiaRload=LPTIARLOAD_10R, .LpTiaRtia=(uint32_t)gain,
            .LpAmpPwrMod=LPAMPPWR_NORM, .LpPaPwrEn=bTRUE, .LpTiaPwrEn=bTRUE,
            .LpTiaSW=LPTIASW(2)|LPTIASW(4)|LPTIASW(5)|LPTIASW(12)|LPTIASW(13),
        };
        AD5940_LPLoopCfgS(&lp);
        rtia_actual = rtia_ohm * (CONFIG_ECW_RTIA_SCALE_PPM / 1000000.0f);
        rtia_nominal = rtia_ohm;
    }
    /* In OCP leave PA, TIA, DAC and all electrode switches off. */
    ADCBaseCfg_Type adc = { .ADCMuxP=ADCMUXP_LPTIA0_P,
        .ADCMuxN=ADCMUXN_LPTIA0_N, .ADCPga=ADCPGA_1 };
    AD5940_ADCBaseCfgS(&adc);
    ADCFilterCfg_Type filter = {
        .ADCSinc3Osr=ADCSINC3OSR_4, .ADCSinc2Osr=ADCSINC2OSR_22,
        .ADCAvgNum=ADCAVGNUM_2, .ADCRate=ADCRATE_800KHZ,
        .BpNotch=bTRUE, .BpSinc3=bFALSE,
        .Sinc3ClkEnable=bTRUE, .Sinc2NotchClkEnable=bTRUE, .Sinc2NotchEnable=bTRUE,
    };
    AD5940_ADCFilterCfgS(&filter);
    AD5940_ADCPowerCtrlS(bTRUE);
    k_msleep(5);
    /* Validate both AFE supplies before starting the experiment. The 1.11V
     * reference is nominal; this is a plausibility check, not a voltmeter calibration. */
    float avdd_half, dvdd_half;
    uint8_t flags = 0;
    int rc = adc_volt(ADCMUXP_AVDD_2, ADCMUXN_VSET1P1, &avdd_half, &flags);
    if (rc) { return rc; }
    rc = adc_volt(ADCMUXP_DVDD_2, ADCMUXN_VSET1P1, &dvdd_half, &flags);
    if (rc) { return rc; }
    avdd_actual = 2.0f * (avdd_half + 1.11f);
    float dvdd = 2.0f * (dvdd_half + 1.11f);
    if (flags || avdd_actual < 2.8f || avdd_actual > 3.6f ||
        dvdd < 2.8f || dvdd > 3.6f || fabsf(avdd_actual-dvdd)>0.3f) { return -ERANGE; }
    return afe_port_error();
}
int afe_set_potential(int32_t mv, int32_t *actual_uv)
{
    if (mv < -1000 || mv > 1000) { return -ERANGE; }
    /* E = V_WE - V_RE = Vzero - Vbias. ADI SDK compensation: code--
     * above the 6bit*64 crossover (Amperometric example). */
    int code = 2048 - (int)lroundf(mv * 4095.0f / 2200.0f);
    int applied = code > 2048 ? code - 1 : code;
    if (applied < 0 || applied > 4095) { return -ERANGE; }
    AD5940_LPDAC0WriteS((uint16_t)applied, 32);
    *actual_uv = (int32_t)lroundf((2048 - code) * 2200000.0f / 4095.0f);
    return afe_port_error();
}
static int adc_volt(uint32_t p, uint32_t n, float *volt, uint8_t *flags)
{
    AD5940_ADCConvtCtrlS(bFALSE);
    AD5940_ADCMuxCfgS(p, n);
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
    AD5940_ADCConvtCtrlS(bTRUE);
    int64_t deadline = k_uptime_get() + 20;
    uint32_t sum = 0, count = 0;
    /* Discard two filter outputs after every MUX/conversion restart. */
    while (count < 10) {
        if (atomic_get(&ecw_stop)) { AD5940_ADCConvtCtrlS(bFALSE); return -ECANCELED; }
        if (afe_port_error() || k_uptime_get() >= deadline) {
            AD5940_ADCConvtCtrlS(bFALSE);
            return afe_port_error() ? afe_port_error() : -ETIMEDOUT;
        }
        if (!AD5940_INTCTestFlag(AFEINTC_1, AFEINTSRC_SINC2RDY)) {
            k_usleep(100); continue;
        }
        AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
        uint32_t code = AD5940_ReadAfeResult(AFERESULT_SINC2) & 0xffff;
        if (count >= 2) {
            sum += code;
            if (code < 256 || code > 65279) { *flags |= ECW_FLAG_ADC_RAIL; }
        }
        ++count;
    }
    AD5940_ADCConvtCtrlS(bFALSE);
    *volt = AD5940_ADCCode2Volt((sum + 4) / 8, ADCPGA_1,
                               CONFIG_ECW_ADC_REFERENCE_MV / 1000.0f);
    return afe_port_error();
}
int afe_reduce_current_range(void)
{
    /* Only change TIA gain. Keep PA, DAC, RLOAD and electrode switches intact. */
    static const int32_t gears[]={200,1000,4000,10000,20000,100000,256000,512000};
    if (!afe_ready()) { return -ENODEV; }
    if (atomic_get(&ecw_stop)) { return -ECANCELED; }
    int32_t next=0;
    for (size_t i=0;i<ARRAY_SIZE(gears);++i) {
        if (gears[i]<rtia_nominal) { next=gears[i]; }
    }
    if (!next) { return -ERANGE; }
    uint32_t reg=AD5940_ReadReg(REG_AFE_LPTIACON0);
    if (afe_port_error()) { return afe_port_error(); }
    reg=(reg & ~BITM_AFE_LPTIACON0_TIAGAIN) |
        ((uint32_t)afe_select_rtia(next)<<BITP_AFE_LPTIACON0_TIAGAIN);
    AD5940_WriteReg(REG_AFE_LPTIACON0,reg);
    if (afe_port_error()) { return afe_port_error(); }
    rtia_nominal=next;
    rtia_actual=next*(CONFIG_ECW_RTIA_SCALE_PPM/1000000.0f);
    k_msleep(2);
    return 0;
}
int afe_current(int32_t *na, uint8_t *flags)
{
    float v;
    int rc = adc_volt(ADCMUXP_LPTIA0_P, ADCMUXN_LPTIA0_N, &v, flags);
    if (!rc) {
        /* The LPTIA linear output range is narrower than the ADC code range. */
        if (fabsf(v)>0.85f) { *flags |= ECW_FLAG_ADC_RAIL; }
        /* Anodic-positive convention: conventional current leaves WE towards
         * the cell. Feedback supplies this current, so Vout - V_WE = I*RTIA.
         * ADC LPTIA0_P is output and LPTIA0_N is the summing/WE node.
         * The old minus sign reported cell-to-WE current (cathodic positive). */
        *na = (int32_t)lroundf(v * 1e9f / rtia_actual);
    }
    return rc;
}
int afe_ocp(int32_t *uv, uint8_t *flags)
{
    float we, re;
    /* ADCMUXN has no RE0 option: measure both relative to the SAME 1.11V. */
    int rc = adc_volt(ADCMUXP_VSE0, ADCMUXN_VSET1P1, &we, flags);
    if (rc) { return rc; }
    rc = adc_volt(ADCMUXP_VRE0, ADCMUXN_VSET1P1, &re, flags);
    if (!rc) {
        /* Input common mode must stay inside the documented ADC supply rails. */
        if (we + 1.11f < 0.05f || re + 1.11f < 0.05f ||
            we + 1.11f > avdd_actual - 0.05f ||
            re + 1.11f > avdd_actual - 0.05f) { return -ERANGE; }
        *uv = (int32_t)lroundf((we - re) * 1e6f);
    }
    return rc;
}

/* Bounded, cancellable EIS acquisition, based on the ADI HSRtiaCal signal paths.
 * Do not call the vendor calibration routine: its unbounded DFT polls cannot
 * respond to an APP STOP or a failed SPI bus. */
struct complex_value { double re, im; };
static struct complex_value cdiv(struct complex_value a,struct complex_value b)
{
    double d=b.re*b.re+b.im*b.im;
    return (struct complex_value){(a.re*b.re+a.im*b.im)/d,(a.im*b.re-a.re*b.im)/d};
}
static struct complex_value cmul(struct complex_value a,struct complex_value b)
{
    return (struct complex_value){a.re*b.re-a.im*b.im,a.re*b.im+a.im*b.re};
}
static int pause_ms(uint32_t ms)
{
    int64_t until=k_uptime_get()+ms;
    while (k_uptime_get()<until) {
        if (atomic_get(&ecw_stop)) { return -ECANCELED; }
        if (afe_port_error()) { return afe_port_error(); }
        k_msleep(1);
    }
    return 0;
}
static int32_t signed18(uint32_t v)
{
    v&=0x3ffff;
    return v&0x20000 ? (int32_t)v-0x40000 : (int32_t)v;
}
struct dft_plan { ADCFilterCfg_Type filter; DFTCfg_Type dft; uint32_t timeout_ms, settle_ms; };
static struct dft_plan dft_plan(double hz)
{
    struct dft_plan plan={0};
    static const uint16_t osrs[]={22,44,89,178,267,533,640,667,800,889,1067,1333};
    double fs=200000.0;
    plan.filter=(ADCFilterCfg_Type){.ADCSinc3Osr=ADCSINC3OSR_4,.ADCSinc2Osr=ADCSINC2OSR_22,
        .ADCAvgNum=ADCAVGNUM_2,.ADCRate=ADCRATE_800KHZ,.BpNotch=bTRUE,
        .Sinc3ClkEnable=bTRUE,.Sinc2NotchClkEnable=bTRUE,.Sinc2NotchEnable=bTRUE,
        .DFTClkEnable=bTRUE,.WGClkEnable=bTRUE};
    plan.dft.DftSrc=DFTSRC_SINC3;
    /* Keep 100kHz below Nyquist: 800kSPS / OSR2 = 400kSPS. */
    if (hz>50000) { plan.filter.ADCSinc3Osr=ADCSINC3OSR_2; fs=400000.0; }
    if (hz<1000) {
        unsigned choice=0;
        for (unsigned i=0;i<ARRAY_SIZE(osrs);++i) {
            if (fs/osrs[i]>=hz*16 || i==0) { choice=i; }
        }
        plan.filter.ADCSinc2Osr=choice; fs/=osrs[choice];
        plan.dft.DftSrc=DFTSRC_SINC2NOTCH;
    }
    uint32_t n=4, code=DFTNUM_4;
    while (n<16384 && n<fs/hz*4) { n*=2; ++code; }
    plan.dft.DftNum=code; plan.dft.HanWinEn=bTRUE;
    plan.timeout_ms=(uint32_t)ceil(1000*n/fs)+500;
    plan.settle_ms=(uint32_t)ceil(1000*3/hz)+5;
    return plan;
}
static int take_dft(uint32_t p,uint32_t n,const struct dft_plan *plan,struct complex_value *out)
{
    AD5940_AFECtrlS(AFECTRL_ADCCNV|AFECTRL_DFT,bFALSE);
    AD5940_ADCMuxCfgS(p,n);
    AD5940_INTCClrFlag(AFEINTSRC_DFTRDY);
    int rc=pause_ms(2); if (rc) { return rc; }
    AD5940_AFECtrlS(AFECTRL_ADCCNV|AFECTRL_DFT,bTRUE);
    int64_t until=k_uptime_get()+plan->timeout_ms;
    while (!AD5940_INTCTestFlag(AFEINTC_1,AFEINTSRC_DFTRDY)) {
        if (atomic_get(&ecw_stop)) { rc=-ECANCELED; break; }
        if (afe_port_error()) { rc=afe_port_error(); break; }
        if (k_uptime_get()>=until) { rc=-ETIMEDOUT; break; }
        k_msleep(1);
    }
    AD5940_AFECtrlS(AFECTRL_ADCCNV|AFECTRL_DFT,bFALSE);
    if (rc) { return rc; }
    int32_t real=signed18(AD5940_ReadAfeResult(AFERESULT_DFTREAL));
    int32_t imag=signed18(AD5940_ReadAfeResult(AFERESULT_DFTIMAGE));
    if (abs(real)>=125000 || abs(imag)>=125000) { return -ERANGE; }
    *out=(struct complex_value){real,-imag}; /* ADI DFT imaginary sign. */
    AD5940_INTCClrFlag(AFEINTSRC_DFTRDY);
    return afe_port_error();
}
static int setup_hs(const struct ecw_request *r,double hz,double peak_mv,bool calibrate,const struct dft_plan *plan)
{
    static const uint32_t gears[]={HSTIARTIA_200,HSTIARTIA_1K,HSTIARTIA_5K,HSTIARTIA_10K,
        HSTIARTIA_20K,HSTIARTIA_40K,HSTIARTIA_80K,HSTIARTIA_160K};
    afe_idle();
    if (!afe_ready()) { return -ENODEV; }
    AD5940_AFEPwrBW(hz>80000?AFEPWR_HP:AFEPWR_LP,AFEBW_250KHZ);
    AFERefCfg_Type ref={.HpBandgapEn=bTRUE,.Hp1V8BuffEn=bTRUE,.Hp1V1BuffEn=bTRUE,
        .LpBandgapEn=bTRUE,.LpRefBufEn=bTRUE}; AD5940_REFCfgS(&ref);
    HSLoopCfg_Type hs={0};
    hs.HsTiaCfg=(HSTIACfg_Type){.HstiaBias=HSTIABIAS_1P1,.HstiaRtiaSel=gears[r->gear],
        .HstiaCtia=16,.HstiaDeRtia=HSTIADERTIA_OPEN,.HstiaDeRload=HSTIADERLOAD_OPEN};
    double full_peak;
    if (peak_mv<=20) {
        hs.HsDacCfg.ExcitBufGain=EXCITBUFGAIN_0P25; hs.HsDacCfg.HsDacGain=HSDACGAIN_0P2; full_peak=20;
    } else {
        hs.HsDacCfg.ExcitBufGain=EXCITBUFGAIN_0P25; hs.HsDacCfg.HsDacGain=HSDACGAIN_1; full_peak=100;
    }
    hs.HsDacCfg.HsDacUpdateRate=7;
    hs.WgCfg.WgType=WGTYPE_SIN;
    hs.WgCfg.GainCalEn=bTRUE; hs.WgCfg.OffsetCalEn=bTRUE;
    hs.WgCfg.SinCfg.SinFreqWord=AD5940_WGFreqWordCal(hz,16000000.0f);
    hs.WgCfg.SinCfg.SinAmplitudeWord=(uint32_t)lround(peak_mv/full_peak*2047);
    if (calibrate) {
        hs.SWMatCfg=(SWMatrixCfg_Type){.Dswitch=SWD_RCAL0,.Pswitch=SWP_RCAL0,
            .Nswitch=SWN_RCAL1,.Tswitch=SWT_RCAL1|SWT_TRTIA};
    } else {
        /* SE0 pad -> 100-ohm SE0 load switch -> HSTIA. DE0 is not used. */
        hs.SWMatCfg=(SWMatrixCfg_Type){.Dswitch=SWD_CE0,.Pswitch=SWP_RE0,
            .Nswitch=SWN_SE0,.Tswitch=SWT_SE0LOAD|SWT_TRTIA};
        hs.HsTiaCfg.HstiaBias=HSTIABIAS_VZERO0;
        LPDACCfg_Type dac={.LpdacSel=LPDAC0,.LpDacSrc=LPDACSRC_MMR,
            .LpDacVzeroMux=LPDACVZERO_6BIT,.LpDacVbiasMux=LPDACVBIAS_12BIT,
            .LpDacSW=LPDACSW_VBIAS2PIN|LPDACSW_VZERO2HSTIA,.LpDacRef=LPDACREF_2P5,
            .PowerEn=bTRUE,.DacData6Bit=32,.DacData12Bit=2048};
        AD5940_LPDACCfgS(&dac);
        int32_t actual; int rc=afe_set_potential(r->start_mv,&actual); if (rc) { return rc; }
    }
    AD5940_HSLoopCfgS(&hs);
    ADCBaseCfg_Type adc={.ADCMuxP=ADCMUXP_P_NODE,.ADCMuxN=ADCMUXN_N_NODE,.ADCPga=ADCPGA_1P5};
    AD5940_ADCBaseCfgS(&adc);
    ADCFilterCfg_Type filter=plan->filter; AD5940_ADCFilterCfgS(&filter);
    DFTCfg_Type dft=plan->dft; AD5940_DFTCfgS(&dft);
    AD5940_INTCCfg(AFEINTC_1,AFEINTSRC_DFTRDY,bTRUE);
    AD5940_AFECtrlS(AFECTRL_HSTIAPWR|AFECTRL_INAMPPWR|AFECTRL_EXTBUFPWR|AFECTRL_DACREFPWR|
        AFECTRL_HSDACPWR|AFECTRL_SINC2NOTCH|AFECTRL_WG|AFECTRL_ADCPWR|
        (calibrate?0:AFECTRL_DCBUFPWR),bTRUE);
    return pause_ms(plan->settle_ms);
}
int afe_eis_point(const struct ecw_request *r,uint32_t mhz,struct ecw_sample *s)
{
    static const double nominal[]={200,1000,5000,10000,20000,40000,80000,160000};
    double requested_hz=mhz/1000.0;
    /* Report the frequency the 30-bit WG can actually generate. */
    uint32_t word=AD5940_WGFreqWordCal(requested_hz,16000000.0f);
    double hz=word*16000000.0/1073741824.0;
    struct dft_plan plan=dft_plan(hz);
    double cal_peak=fmin(50.0,300.0*CONFIG_ECW_RCAL_OHM/nominal[r->gear]);
    int rc=setup_hs(r,hz,cal_peak,true,&plan); if (rc) { return rc; }
    struct complex_value vcal, vtia, rtia;
    rc=take_dft(ADCMUXP_P_NODE,ADCMUXN_N_NODE,&plan,&vcal); if (rc) { return rc; }
    rc=take_dft(ADCMUXP_HSTIA_P,ADCMUXN_HSTIA_N,&plan,&vtia); if (rc) { return rc; }
    if (hypot(vcal.re,vcal.im)<2 || hypot(vtia.re,vtia.im)<2) { return -ERANGE; }
    rtia=cdiv((struct complex_value){-vtia.re,-vtia.im},vcal);
    rtia.re*=CONFIG_ECW_RCAL_OHM; rtia.im*=CONFIG_ECW_RCAL_OHM;
    double magnitude=hypot(rtia.re,rtia.im);
    if (!isfinite(magnitude)||magnitude<nominal[r->gear]*0.25||magnitude>nominal[r->gear]*4) { return -ERANGE; }
    rc=setup_hs(r,hz,r->pulse_mv,false,&plan); if (rc) { return rc; }
    struct complex_value total={0};
    for (uint32_t i=0;i<r->averages;++i) {
        struct complex_value voltage,current;
        rc=take_dft(ADCMUXP_P_NODE,ADCMUXN_N_NODE,&plan,&voltage); if (rc) { return rc; }
        rc=take_dft(ADCMUXP_HSTIA_P,ADCMUXN_HSTIA_N,&plan,&current); if (rc) { return rc; }
        if (hypot(current.re,current.im)<2) { return -ERANGE; }
        struct complex_value z=cmul(cdiv((struct complex_value){-voltage.re,-voltage.im},current),rtia);
        total.re+=z.re; total.im+=z.im;
    }
    double re=total.re/r->averages*1000, im=total.im/r->averages*1000;
    if (!isfinite(re)||!isfinite(im)||fabs(re)>2147483647.0||fabs(im)>2147483647.0) { return -ERANGE; }
    s->freq_mhz=(uint32_t)llround(hz*1000); s->z_real_mohm=(int32_t)llround(re); s->z_imag_mohm=(int32_t)llround(im);
    return afe_port_error();
}
