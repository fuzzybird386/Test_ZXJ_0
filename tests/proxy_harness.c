/* Actual CV/DPV sources are linked, only physical AFE and time are simulated. */
#include <zephyr/kernel.h>
#include "src/method/proxy/ec_proxy.h"
#include "src/method/proxy/ec_proxy_lpamp.h"
#include "src/method/inc/ec_params.h"
#include "src/inc/ble_aabb.h"
#include <stdarg.h>
#include <errno.h>
int applied, stopped, prepare_count, timeout_mode;
uint32_t clock_ms;
uint32_t k_uptime_get_32(void) { return clock_ms++; }
int ec_proxy_bus_prepare_experiment(const char *s) { prepare_count++; return 0; }
void ec_lpamp_set_rtia_gear(uint8_t x) {}
int ec_lpamp_calibrate_and_configure(const char *s) { return 0; }
void ec_lpamp_write_dac(int16_t e) { applied=e; }
float ec_lpamp_rtia_ohm(void) { return 10000.0f; }
void ec_lpamp_shutdown(void) { stopped++; }
int mux_select_we(uint8_t x) { return x == 0 ? 0 : -EINVAL; }
void AD5940_INTCClrFlag(uint32_t f) {}
void AD5940_ADCConvtCtrlS(BoolFlag b) {}
BoolFlag AD5940_INTCTestFlag(uint32_t n, uint32_t f) { return !timeout_mode; }
uint32_t AD5940_INTCGetFlag(uint32_t n) { return 0; }
uint32_t AD5940_ReadReg(uint16_t r) { return 0; }
uint32_t AD5940_ReadAfeResult(uint32_t r) { return 32768; }
float AD5940_ADCCode2Volt(uint32_t a,uint32_t b,float c) { return 0.01f; }
void AD5940_AFECtrlS(uint32_t f,BoolFlag b) {}
int ble_log_eventf(ble_log_level_t l,uint16_t c,const char *f,...) { return 0; }
volatile int test_result;
volatile int test_count;
#define CHECK(c) do { test_count++; if (!(c)) { test_result=__LINE__; return; } } while(0)
static ec_tick_out_t out;
static uint32_t delay;
static int potential(void) { return (int16_t)((uint16_t)out.payload[0] | (uint16_t)out.payload[1]<<8); }
void test_entry(void) {
 ec_cv_params_t cv = {0,3,0,10,-10,0,100,1,5,0};
 int expected[] = {0,5,10,5,0,-5,-10,-5,0};
 CHECK(sizeof(cv)==18);
 CHECK(ec_proxy_cv_start((void*)&cv,sizeof(cv),&delay)==0);
 for(unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);i++) {
  CHECK(applied==expected[i]);
  int rc=ec_proxy_cv_tick(&out);
  CHECK(out.len==6 && potential()==expected[i]);
  CHECK(rc==(i==8 ? EC_TICK_DONE : EC_TICK_CONTINUE));
 }
 CHECK(stopped==1);
 cv.e_vertex1_mV=-10; cv.e_vertex2_mV=10;
 CHECK(ec_proxy_cv_start((void*)&cv,sizeof(cv),&delay)==0);
 for(unsigned i=0;i<9;i++) { int rc=ec_proxy_cv_tick(&out); CHECK(potential()==-expected[i]); CHECK(rc==(i==8)); }
 cv.e_begin_mV=0; cv.e_vertex1_mV=5; cv.e_vertex2_mV=-5; cv.e_end_mV=-5; cv.cycles=300;
 CHECK(ec_proxy_cv_start((void*)&cv,sizeof(cv),&delay)==0);
 int points=0; while(1) { int rc=ec_proxy_cv_tick(&out); points++; CHECK(points<=1200); if(rc) break; }
 CHECK(points==1200 && potential()==-5);
 cv.cycles=1; cv.e_vertex1_mV=7; cv.e_vertex2_mV=-7; cv.e_end_mV=2;
 CHECK(ec_proxy_cv_start((void*)&cv,sizeof(cv),&delay)==0);
 int odd[]={0,5,7,2,-3,-7,-2,2};
 for(unsigned i=0;i<8;i++) { int rc=ec_proxy_cv_tick(&out); CHECK(potential()==odd[i]); CHECK(rc==(i==7)); }
 int before=prepare_count;
 cv.step_mV=0; CHECK(ec_proxy_cv_start((void*)&cv,sizeof(cv),&delay)==-EINVAL);
 cv.step_mV=5; cv.we_index=1; CHECK(ec_proxy_cv_start((void*)&cv,sizeof(cv),&delay)==-ENOTSUP);
 cv.we_index=0; cv.e_end_mV=1001; CHECK(ec_proxy_cv_start((void*)&cv,sizeof(cv),&delay)==-ERANGE);
 CHECK(prepare_count==before);
 ec_dpv_params_t dpv={0,3,-10,10,6,50,50,200,2000};
 CHECK(sizeof(dpv)==16);
 CHECK(ec_proxy_dpv_start((void*)&dpv,sizeof(dpv),&delay)==0 && delay==2150000);
 int bases[]={-10,-4,2,8,10};
 for(unsigned i=0;i<5;i++) {
  CHECK(applied==bases[i]); CHECK(ec_proxy_dpv_tick(&out)==EC_TICK_CONTINUE);
  CHECK(out.len==0 && applied==bases[i]+50 && out.delay_us==50000);
  int rc=ec_proxy_dpv_tick(&out); CHECK(potential()==bases[i] && out.len==10); CHECK(rc==(i==4));
 }
 dpv.e_begin_mV=10; dpv.e_end_mV=-10;
 CHECK(ec_proxy_dpv_start((void*)&dpv,sizeof(dpv),&delay)==0);
 for(unsigned i=0;i<5;i++) { CHECK(ec_proxy_dpv_tick(&out)==0); CHECK(applied==-bases[i]+50); int rc=ec_proxy_dpv_tick(&out); CHECK(potential()==-bases[i]); CHECK(rc==(i==4)); }
 before=prepare_count;
 dpv.period_ms=dpv.pulse_width_ms; CHECK(ec_proxy_dpv_start((void*)&dpv,sizeof(dpv),&delay)==-EINVAL);
 dpv.period_ms=200; dpv.e_end_mV=980; CHECK(ec_proxy_dpv_start((void*)&dpv,sizeof(dpv),&delay)==-ERANGE);
 CHECK(prepare_count==before);
 dpv.e_end_mV=0; CHECK(ec_proxy_dpv_start((void*)&dpv,sizeof(dpv),&delay)==0);
 timeout_mode=1; before=stopped;
 CHECK(ec_proxy_dpv_tick(&out)==EC_TICK_DONE && out.frame_type==EC_FRAME_ERROR && stopped==before+1);
 test_result=-1;
}
