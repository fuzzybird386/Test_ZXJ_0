#define CHIPSEL_594X
#define CONFIG_ECW_RTIA_SCALE_PPM 1000000
#define CONFIG_ECW_ADC_REFERENCE_MV 1820
#define CONFIG_ECW_RCAL_OHM 200
#include <zephyr/sys/util.h>
static inline void k_usleep(int us) { (void)us; }
#include "../src/ad5941.c"
volatile unsigned checks;
int64_t test_now;
atomic_t ecw_stop;
static uint32_t register_value;
static int bus_error;
static unsigned writes;
static float model_tia_voltage;
static uint16_t dac12,dac6;
void AD5940_LPDAC0WriteS(uint16_t value12,uint8_t value6) { dac12=value12;dac6=value6; }
static uint32_t model_code=32768;
void AD5940_ADCConvtCtrlS(BoolFlag state) { (void)state; }
void AD5940_ADCMuxCfgS(uint32_t p,uint32_t n) { (void)p;(void)n; }
void AD5940_INTCClrFlag(uint32_t mask) { (void)mask; }
BoolFlag AD5940_INTCTestFlag(uint32_t group,uint32_t mask) { (void)group;(void)mask;return bTRUE; }
uint32_t AD5940_ReadAfeResult(uint32_t select) { (void)select;return model_code; }
float AD5940_ADCCode2Volt(uint32_t code,uint32_t gain,float ref) { (void)code;(void)gain;(void)ref;return model_tia_voltage; }
uint32_t AD5940_ReadReg(uint16_t addr) { (void)addr;return register_value; }
void AD5940_WriteReg(uint16_t addr,uint32_t value) { (void)addr;register_value=value;++writes; }
int afe_port_error(void) { return bus_error; }
#define CHECK(x) do { ++checks; if (!(x)) return __LINE__; } while (0)
int run_tests(void) {
    uint32_t preserved=0x30000|BITM_AFE_LPTIACON0_HALFPWR|(3U<<BITP_AFE_LPTIACON0_TIARL);
    atomic_set(&ready,1);rtia_nominal=512000;rtia_actual=512000;
    register_value=preserved|((uint32_t)LPTIARTIA_512K<<BITP_AFE_LPTIACON0_TIAGAIN);
    const int32_t expected[]={256000,100000,20000,10000,4000,1000,200};
    for(unsigned i=0;i<ARRAY_SIZE(expected);++i) {
        CHECK(afe_reduce_current_range()==0);
        CHECK(rtia_nominal==expected[i] && rtia_actual==(float)expected[i]);
        CHECK((register_value&~BITM_AFE_LPTIACON0_TIAGAIN)==preserved);
        CHECK((register_value&BITM_AFE_LPTIACON0_TIAGAIN)==((uint32_t)afe_select_rtia(expected[i])<<BITP_AFE_LPTIACON0_TIAGAIN));
    }
    CHECK(test_now==14 && writes==7);
    CHECK(afe_reduce_current_range()==-ERANGE && writes==7);
    atomic_set(&ecw_stop,1);CHECK(afe_reduce_current_range()==-ECANCELED && writes==7);
    atomic_clear(&ecw_stop);atomic_clear(&ready);CHECK(afe_reduce_current_range()==-ENODEV);
    atomic_set(&ready,1);rtia_nominal=10000;bus_error=-EIO;
    CHECK(afe_reduce_current_range()==-EIO && writes==7 && rtia_nominal==10000);
    /* Same physical current must retain nA units after the gain changes.
     * Reproduce ~220uA exceeding 0.85V with 4k, then fitting at 1k. */
    bus_error=0;rtia_nominal=4000;rtia_actual=4000;model_tia_voltage=-0.88f;
    int32_t na;uint8_t flags=0;
    CHECK(afe_current(&na,&flags)==0 && (flags&ECW_FLAG_ADC_RAIL));
    CHECK(afe_reduce_current_range()==0 && rtia_nominal==1000);
    model_tia_voltage=-0.22f;
    flags=0;CHECK(afe_current(&na,&flags)==0 && !flags);
    CHECK(na>=-220001 && na<=-219999);
    /* Positive output relative to the summing node supplies conventional
     * current leaving WE: oxidation must be positive in the APP. */
    model_tia_voltage=0.1f;flags=0;
    CHECK(afe_current(&na,&flags)==0 && !flags && na>=99999 && na<=100001);
    model_code=65535;flags=0;
    CHECK(afe_current(&na,&flags)==0 && (flags&ECW_FLAG_ADC_RAIL));
    /* Test actual DAC writes, not just the sample's reported x coordinate.
     * E_WE-RE increases when Vbias/code decreases; Vzero remains fixed. */
    int32_t uv;uint16_t last_code=4095;
    for(int mv=-600;mv<=1000;mv+=5) {
        CHECK(afe_set_potential(mv,&uv)==0);
        CHECK(dac6==32 && dac12<last_code);
        CHECK(uv>=mv*1000-300 && uv<=mv*1000+300);
        last_code=dac12;
    }
    for(int mv=995;mv>=-600;mv-=5) {
        CHECK(afe_set_potential(mv,&uv)==0 && dac12>last_code && dac6==32);
        last_code=dac12;
    }
    CHECK(afe_set_potential(1001,&uv)==-ERANGE);
    return 0;
}
