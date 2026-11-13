/* Runs the real methods.c against a deterministic AFE and BLE model. This
 * verifies waveform/control semantics, not analog accuracy or radio delivery. */
#include "../src/methods.c"
#include <string.h>
volatile unsigned checks;
int64_t test_now;
static int32_t applied_mv;
static unsigned count, stop_after;
static bool connected_model=true;
static bool boot_model=true, afe_model=true;
static unsigned saturated_reads,range_changes;
static int range_result;
static unsigned read_delay=2;
static bool clip_at_280;
static unsigned total_samples,positive_turns,negative_turns,clipped_at_index;
static bool audit_enabled;
static unsigned audit_errors,cycle_logs;
/* Expose actual framed user-run samples to the Python runner for CSV review. */
uint8_t user_trace[2561][14];
unsigned user_trace_count;
static struct ecw_sample received[128];
static int64_t emitted_at[128];
#define CHECK(x) do { ++checks; if (!(x)) return __LINE__; } while (0)
void *memset(void *p,int c,size_t n) { uint8_t *d=p;while(n--) *d++=c;return p; }
int abs(int x) { return x<0?-x:x; }
int afe_set_potential(int32_t mv,int32_t *uv) { applied_mv=mv;*uv=mv*1000;return 0; }
int afe_current(int32_t *na,uint8_t *flags) {
    test_now+=read_delay;*na=applied_mv*100;*flags=0;
    if(saturated_reads) { --saturated_reads;*flags=ECW_FLAG_ADC_RAIL;*na=999999; }
    if(clip_at_280 && applied_mv>=280 && !range_changes) {
        *flags=ECW_FLAG_ADC_RAIL;*na=999999;clipped_at_index=total_samples;
    }
    return 0;
}
int afe_reduce_current_range(void) { ++range_changes;test_now+=2;return range_result; }
void ble_log(uint8_t level,uint16_t code,const char *message) { (void)level;(void)message;if(code==0x111)++cycle_logs; }
int afe_ocp(int32_t *uv,uint8_t *flags) { test_now+=4;*uv=-12345;*flags=0;return 0; }
int afe_eis_point(const struct ecw_request *r,uint32_t f,struct ecw_sample *s)
{ (void)r;s->freq_mhz=f;s->z_real_mohm=100000;s->z_imag_mohm=-50000;test_now+=10;return 0; }
bool ble_stream_ready(void) { return connected_model; }
bool afe_boot_finished(void) { return boot_model; }
bool afe_ready(void) { return afe_model; }
uint32_t ble_session(void) { return 12; }
int ble_tx_error(void) { return 0; }
int ble_frame(uint32_t gen,uint8_t method,uint8_t type,uint16_t sid,uint16_t seq,const struct ecw_sample *s)
{
    if(audit_enabled && type==0) {
        unsigned i=total_samples,phase=i%640;
        int expected_mv=phase<=320 ? -600+(int)phase*5 : 1000-((int)phase-320)*5;
        uint8_t packet[20];size_t bytes=wire_frame(packet,method,type,sid,seq,s);
        if(bytes!=14 || packet[0]!=ECW_CV || packet[1]!=0 || wire_u16(packet+2)!=0x1234 ||
           wire_u16(packet+4)!=i || wire_u16(packet+6)!=6 ||
           (int16_t)wire_u16(packet+8)!=expected_mv || gen!=12 || s->flags)++audit_errors;
        if(i<2561) { memcpy(user_trace[i],packet,14);user_trace_count=i+1; }
    }
    if(type==0) {
        ++total_samples;
        if(s->potential_uv==1000000)++positive_turns;
        if(s->potential_uv==-600000)++negative_turns;
    }
    if (type==0 && count<128) { received[count]=*s;emitted_at[count++]=test_now; }
    if (stop_after && count>=stop_after) atomic_set(&ecw_stop,1);
    return 0;
}
static void reset_model(void) { test_now=0; count=0;stop_after=0;connected_model=true;atomic_clear(&ecw_stop);saturated_reads=range_changes=0;range_result=0;read_delay=2;clip_at_280=false;total_samples=positive_turns=negative_turns=clipped_at_index=0;audit_enabled=false;audit_errors=cycle_logs=0; }
int run_tests(void)
{
    /* A valid APP OCP request must never reach SPI while boot owns the AFE. */
    struct ecw_request request={.method=ECW_OCP,.duration_ms=1000,.interval_ms=100};
    boot_model=false;
    CHECK(methods_submit(&request,12)==-EAGAIN && !methods_busy());
    boot_model=true; afe_model=false;
    CHECK(methods_submit(&request,12)==-ENODEV && !methods_busy());
    afe_model=true;
    struct job j={.generation=12,.req={.method=ECW_CV,.session_id=0x7711,.start_mv=0,.vertex1_mv=10,
        .vertex2_mv=-10,.end_mv=5,.step_mv=6,.rate_mv_s=100,.cycles=2}};
    struct ecw_sample s={.method=ECW_CV};
    reset_model(); afe_set_potential(0,&s.potential_uv);
    CHECK(run_cv(&j,&s,0)==0);
    const int expected[]={0,6,10,4,-2,-8,-10,-4,2,8,10,4,-2,-8,-10,-4,2,5};
    CHECK(count==ARRAY_SIZE(expected));
    for(unsigned i=0;i<count;++i) { CHECK(received[i].potential_uv==expected[i]*1000);CHECK(received[i].index==i); }
    CHECK(applied_mv==5);
    reset_model(); s.index=0; j.req.start_mv=10;j.req.vertex1_mv=10;j.req.vertex2_mv=10;j.req.end_mv=10;
    afe_set_potential(10,&s.potential_uv); CHECK(run_cv(&j,&s,0)==0 && count==1);
    reset_model(); s.index=0;j.req.start_mv=0;j.req.vertex1_mv=100;j.req.vertex2_mv=-100;stop_after=3;
    afe_set_potential(0,&s.potential_uv); CHECK(run_cv(&j,&s,0)==-ECANCELED && count==3);
    reset_model(); s.index=0;connected_model=false;CHECK(run_cv(&j,&s,0)==-ECANCELED && count==0);
    reset_model(); s.index=0;j.generation=11;CHECK(run_cv(&j,&s,0)==-ECANCELED && count==0);j.generation=12;
    reset_model(); s.index=0;
    j.req=(struct ecw_request){.method=ECW_DPV,.start_mv=0,.end_mv=10,.step_mv=6,.pulse_mv=50,.pulse_ms=50,.period_ms=200};
    CHECK(run_dpv(&j,&s,0)==0 && count==3 && applied_mv==10);
    CHECK(received[0].base_na==0 && received[0].pulse_na==5000);
    CHECK(received[1].potential_uv==6000 && received[1].base_na==600 && received[1].pulse_na==5600);
    CHECK(received[2].potential_uv==10000 && received[2].pulse_na-received[2].base_na==5000);
    reset_model();s.index=0;j.req.start_mv=10;j.req.end_mv=-1;j.req.step_mv=7;
    CHECK(run_dpv(&j,&s,0)==0 && count==3 && received[2].potential_uv==-1000);
    reset_model();s.index=0;
    j.req=(struct ecw_request){.method=ECW_CA,.start_mv=200,.duration_ms=125,.interval_ms=50,.cycles=2,.quiet_ms=30};
    afe_set_potential(200,&s.potential_uv);
    CHECK(run_timed(&j,&s,0)==0 && count==4 && test_now==280);
    CHECK(emitted_at[0]==50 && emitted_at[1]==100 && emitted_at[2]==205 && emitted_at[3]==255);
    for(unsigned i=0;i<count;++i) CHECK(received[i].current_na==20000);
    reset_model();s.index=0;
    j.req=(struct ecw_request){.method=ECW_OCP,.duration_ms=350,.interval_ms=100};
    CHECK(run_timed(&j,&s,0)==0 && count==3 && test_now==350);
    CHECK(received[2].potential_uv==-12345 && emitted_at[2]==300);
    reset_model();s.index=0;j.req.method=ECW_CA;j.req.cycles=1;j.req.start_mv=200;
    afe_set_potential(200,&s.potential_uv);stop_after=1;
    CHECK(run_timed(&j,&s,0)==-ECANCELED && count==1);
    reset_model();s.index=0;
    j.req=(struct ecw_request){.method=ECW_EIS,.freq_start_mhz=1000,.freq_end_mhz=100000,.points_per_dec=1,.averages=1};
    CHECK(run_eis(&j,&s)==0 && count==3);
    CHECK(received[0].freq_mhz==1000 && received[1].freq_mhz==10000 && received[2].freq_mhz==100000);
    reset_model();s.index=0;j.req.freq_start_mhz=100000;j.req.freq_end_mhz=1000;
    CHECK(run_eis(&j,&s)==0 && count==3);
    CHECK(received[0].freq_mhz==100000 && received[1].freq_mhz==10000 && received[2].freq_mhz==1000);
    reset_model();s.index=0;j.req.freq_start_mhz=1000;j.req.freq_end_mhz=1000;
    CHECK(run_eis(&j,&s)==0 && count==1);
    reset_model();s.index=0;j.req.freq_end_mhz=100000;stop_after=1;
    CHECK(run_eis(&j,&s)==-ECANCELED && count==1);
    /* Reproduce clipping during CV: discard the clipped point, widen the
     * range and finish both sweep directions without sending a fake value. */
    reset_model();s.index=0;
    j.req=(struct ecw_request){.method=ECW_CV,.start_mv=0,.vertex1_mv=10,.vertex2_mv=-10,
        .end_mv=0,.step_mv=5,.rate_mv_s=100,.cycles=1};
    saturated_reads=3;read_delay=10;afe_set_potential(0,&s.potential_uv);
    CHECK(run_cv(&j,&s,0)==0 && range_changes==3 && applied_mv==0);
    CHECK(count==9);
    for(unsigned i=0;i<count;++i) CHECK(received[i].current_na!=999999);
    CHECK(emitted_at[1]-emitted_at[0]>=50);
    /* Persistent saturation remains a hard failure, with bounded retries. */
    reset_model();s.index=0;saturated_reads=100;afe_set_potential(0,&s.potential_uv);
    CHECK(run_cv(&j,&s,0)==-ERANGE && range_changes==7 && count==0);
    reset_model();s.index=0;saturated_reads=1;range_result=-ERANGE;
    CHECK(run_cv(&j,&s,0)==-ERANGE && range_changes==1 && count==0);
    reset_model();s.index=0;saturated_reads=1;range_result=-EIO;
    CHECK(run_cv(&j,&s,0)==-EIO && count==0);
    /* DPV must not distort pulse timing by silently switching gains. */
    reset_model();s.index=0;saturated_reads=1;
    j.req=(struct ecw_request){.method=ECW_DPV,.start_mv=0,.end_mv=10,.step_mv=5,
        .pulse_mv=50,.pulse_ms=50,.period_ms=200};
    CHECK(run_dpv(&j,&s,0)==-ERANGE && range_changes==0 && count==0);
    /* User's exact APP request: WE0, 225uA (gear2 / 4k), -600 ->
     * +1000 -> -600mV, four cycles, 5mV/100mV/s and 5s quiet. */
    const uint8_t user_start[]={0x34,0x12,3,1,18,0,0,2,0xa8,0xfd,0xe8,3,
        0xa8,0xfd,0xa8,0xfd,100,0,4,0,5,0,0x88,0x13};
    reset_model();s.index=0;
    CHECK(wire_decode_start(user_start,sizeof(user_start),&j.req)==0);
    CHECK(j.req.rtia_ohm==4000 && j.req.gear==2 && j.req.quiet_ms==5000);
    CHECK(j.req.start_mv==-600 && j.req.vertex1_mv==1000 && j.req.vertex2_mv==-600 && j.req.end_mv==-600);
    CHECK(j.req.rate_mv_s==100 && j.req.step_mv==5 && j.req.cycles==4);
    afe_set_potential(j.req.start_mv,&s.potential_uv);
    CHECK(wait_until(j.req.quiet_ms,&j)==0 && test_now==5000);
    clip_at_280=true;
    audit_enabled=true;
    CHECK(run_cv(&j,&s,test_now)==0);
    CHECK(range_changes==1 && clipped_at_index==176);
    CHECK(total_samples==2561 && s.index==2561);
    CHECK(positive_turns==4 && negative_turns==5);
    CHECK(applied_mv==-600 && s.potential_uv==-600000);
    CHECK(test_now==133050);
    CHECK(!audit_errors && cycle_logs==4 && user_trace_count==2561);
    reset_model();boot_model=false;
    CHECK(methods_prepare_poweroff()==-ETIMEDOUT && test_now==2000);
    boot_model=true;
    return 0;
}
void afe_idle(void) { }
void *memcpy(void *dst,const void *src,size_t n) { uint8_t *d=dst; const uint8_t *s=src; while(n--) *d++=*s++; return dst; }
int *__errno(void) { static int value;return &value; }
