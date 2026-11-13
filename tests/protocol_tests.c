#include "ecw_wire.h"
#include <errno.h>
#include <string.h>
volatile unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) return __LINE__; } while (0)
void *memset(void *p,int c,size_t n) { unsigned char *d=p; while(n--) *d++=c; return p; }
static uint8_t packet[64];
static size_t start(uint8_t method,const uint8_t *params,size_t size)
{
    memset(packet,0,sizeof(packet)); wire_put16(packet,0xA581); packet[2]=3; packet[3]=method;
    wire_put16(packet+4,size); for(size_t i=0;i<size;++i) packet[i+6]=params[i]; return size+6;
}
int run_tests(void)
{
    struct ecw_request r;
    /* Literal APP v1.3 golden packets, not encoded by the firmware under test. */
    const uint8_t cv[]={0,3,0x0c,0xfe,0xf4,1,0x0c,0xfe,0xc8,0,100,0,3,0,5,0,0xd0,7};
    size_t n=start(1,cv,sizeof(cv));
    CHECK(wire_decode_start(packet,n,&r)==0);
    CHECK(r.session_id==0xA581 && r.method==1 && r.gear==3 && r.rtia_ohm==10000);
    CHECK(r.start_mv==-500 && r.vertex1_mv==500 && r.vertex2_mv==-500 && r.end_mv==200);
    CHECK(r.cycles==3 && r.step_mv==5 && r.rate_mv_s==100 && r.quiet_ms==2000);
    for (unsigned len=0;len<64;++len) {
        CHECK((wire_decode_start(packet,len,&r)==0)==(len==24));
    }
    start(1,cv,sizeof(cv)); packet[6]=1; CHECK(wire_decode_start(packet,24,&r)==-95);
    packet[6]=7; CHECK(wire_decode_start(packet,24,&r)==-95);
    packet[6]=0; packet[7]=8; CHECK(wire_decode_start(packet,24,&r)==-EINVAL);
    packet[7]=0; CHECK(wire_decode_start(packet,24,&r)==0 && r.rtia_ohm==200);
    static const int32_t gears[]={200,1000,4000,10000,20000,100000,256000,512000};
    for (unsigned g=0;g<8;++g) { packet[7]=g; CHECK(wire_decode_start(packet,24,&r)==0 && r.rtia_ohm==gears[g]); }
    /* APP default CA: 200mV, 60s, 100ms, one pulse, 5s rest. */
    const uint8_t ca[]={0,0,200,0,0x60,0xea,0,0,100,0,1,0,0x88,0x13};
    n=start(2,ca,sizeof(ca)); CHECK(wire_decode_start(packet,n,&r)==0);
    CHECK(r.duration_ms==60000 && r.interval_ms==100 && r.cycles==1 && r.quiet_ms==5000);
    wire_put16(packet+16,3); CHECK(wire_decode_start(packet,n,&r)==0 && r.cycles==3);
    wire_put16(packet+14,1); CHECK(wire_decode_start(packet,n,&r)==-EINVAL);
    const uint8_t dpv[]={0,0,0x38,0xff,0x20,3,4,0,50,0,50,0,200,0,0xd0,7};
    n=start(4,dpv,sizeof(dpv)); CHECK(wire_decode_start(packet,n,&r)==0);
    CHECK(r.start_mv==-200 && r.end_mv==800 && r.pulse_ms==50 && r.period_ms==200);
    wire_put16(packet+18,60); CHECK(wire_decode_start(packet,n,&r)==-EINVAL);
    const uint8_t ocp[]={0,0,0x40,0x42,0x0f,0,0xe8,3};
    n=start(5,ocp,sizeof(ocp)); CHECK(wire_decode_start(packet,n,&r)==0);
    CHECK(r.duration_ms==1000000 && r.interval_ms==1000 && r.method==5);
    packet[7]=1; CHECK(wire_decode_start(packet,n,&r)==-EINVAL);
    const uint8_t eis[]={0,2,200,0,10,0,0xa0,0x86,1,0,0,0xe1,0xf5,5,50,1};
    n=start(3,eis,sizeof(eis)); CHECK(wire_decode_start(packet,n,&r)==0);
    CHECK(r.freq_start_mhz==100000 && r.freq_end_mhz==100000000 && r.points_per_dec==50 && r.gear==2);
    wire_put32(packet+12,100000000); wire_put32(packet+16,1000);
    CHECK(wire_decode_start(packet,n,&r)==0 && r.freq_start_mhz>r.freq_end_mhz);
    wire_put32(packet+16,999); CHECK(wire_decode_start(packet,n,&r)==-EINVAL);
    n=start(6,eis,16); CHECK(wire_decode_start(packet,n,&r)==-95);
    packet[3]=0; CHECK(wire_decode_start(packet,n,&r)==-95);
    /* Reject impossible CV timing, allow final fractional step and flat legs. */
    start(1,cv,18); wire_put16(packet+20,4); CHECK(wire_decode_start(packet,24,&r)==-EINVAL);
    wire_put16(packet+16,50); CHECK(wire_decode_start(packet,24,&r)==0);
    uint8_t out[32]; memset(out,0xcc,sizeof(out));
    struct ecw_sample s={.potential_uv=-500000,.current_na=-123456,.elapsed_ms=1000,
        .base_na=-50,.pulse_na=150,.freq_mhz=100000,.z_real_mohm=123000,.z_imag_mohm=-45000};
    CHECK(wire_frame(out,1,0,0xa581,0xffff,&s)==14);
    const uint8_t cv_frame[]={1,0,0x81,0xa5,0xff,0xff,6,0,0x0c,0xfe,0xc0,0x1d,0xfe,0xff};
    for(unsigned i=0;i<14;++i) CHECK(out[i]==cv_frame[i]);
    CHECK(out[14]==0xcc);
    CHECK(wire_frame(out,2,0,0x1234,0,&s)==18);
    CHECK(wire_u32(out+8)==1000 && (int16_t)wire_u16(out+12)==-500 && (int32_t)wire_u32(out+14)==-123456);
    CHECK(wire_frame(out,4,0,0x1234,1,&s)==18);
    CHECK((int32_t)wire_u32(out+10)==-50 && (int32_t)wire_u32(out+14)==150);
    s.potential_uv=-12345; CHECK(wire_frame(out,5,0,7,2,&s)==16);
    CHECK(wire_u32(out+8)==1000 && (int32_t)wire_u32(out+12)==-12345);
    CHECK(wire_frame(out,3,0,7,2,&s)==20);
    CHECK(wire_u32(out+8)==100000 && (int32_t)wire_u32(out+12)==123000 && (int32_t)wire_u32(out+16)==-45000);
    CHECK(wire_frame(out,4,2,0x1234,7,0)==8 && out[1]==2 && wire_u16(out+6)==0);
    CHECK(wire_frame(out,3,255,0xabcd,8,0)==8 && out[1]==255 && wire_u16(out+2)==0xabcd);
    CHECK(wire_error(-134)==-95 && wire_error(-95)==-95 && wire_error(-1000)==-128 && wire_error(-22)==-22);
    s.potential_uv=-499700; wire_frame(out,1,0,1,0,&s); CHECK((int16_t)wire_u16(out+8)==-500);
    s.potential_uv=499700; wire_frame(out,1,0,1,0,&s); CHECK((int16_t)wire_u16(out+8)==500);
    /* Deterministic malformed/valid packet fuzz; the decoder must never read
     * outside the supplied 64-byte packet or produce unchecked valid fields. */
    uint32_t seed=0x124578ab;
    for (unsigned i=0;i<10000;++i) {
        for(unsigned k=0;k<64;++k) { seed=seed*1664525+1013904223; packet[k]=seed>>24; }
        unsigned len=seed%65; int rc=wire_decode_start(packet,len,&r);
        CHECK(rc!=0 || wire_validate(&r)==0);
    }
    return 0;
}
int abs(int x) { return x < 0 ? -x : x; }
