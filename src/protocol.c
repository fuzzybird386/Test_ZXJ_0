#include "ecw_wire.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
uint16_t wire_u16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1]<<8; }
uint32_t wire_u32(const uint8_t *p) { return wire_u16(p) | (uint32_t)wire_u16(p+2)<<16; }
void wire_put16(uint8_t *p,uint16_t v) { p[0]=v; p[1]=v>>8; }
void wire_put32(uint8_t *p,uint32_t v) { wire_put16(p,v); wire_put16(p+2,v>>16); }
static int32_t i16(const uint8_t *p) { return (int16_t)wire_u16(p); }
static int16_t sample_mv(int32_t uv) { return (int16_t)((uv+(uv>=0?500:-500))/1000); }
int8_t wire_error(int rc)
{
    if (rc == -ENOTSUP) { return -95; }
    return rc < -127 || rc > 0 ? -128 : (int8_t)rc;
}
int wire_decode_start(const uint8_t *p,size_t len,struct ecw_request *r)
{
    static const uint8_t sizes[] = {0,18,14,16,16,8,16};
    static const int32_t rtias[] = {200,1000,4000,10000,20000,100000,256000,512000};
    if (len < 6 || p[2] != ECW_OP_START) { return -EINVAL; }
    uint8_t m=p[3];
    if (m < 1 || m > 6) { return ECW_UNSUPPORTED; }
    if (wire_u16(p+4)!=sizes[m] || len!=6U+sizes[m]) { return -EMSGSIZE; }
    memset(r,0,sizeof(*r)); r->method=m; r->session_id=wire_u16(p); p+=6;
    if (m==ECW_STIM || p[0]!=0) { return ECW_UNSUPPORTED; }
    r->gear=p[1];
    if (m!=ECW_OCP && r->gear>7) { return -EINVAL; }
    r->rtia_ohm=rtias[r->gear & 7];
    switch (m) {
    case ECW_CV:
        r->start_mv=i16(p+2); r->vertex1_mv=i16(p+4); r->vertex2_mv=i16(p+6);
        r->end_mv=i16(p+8); r->rate_mv_s=wire_u16(p+10);
        r->cycles=wire_u16(p+12); r->step_mv=wire_u16(p+14); r->quiet_ms=wire_u16(p+16); break;
    case ECW_CA:
        r->start_mv=i16(p+2); r->duration_ms=wire_u32(p+4);
        r->interval_ms=wire_u16(p+8); r->cycles=wire_u16(p+10); r->quiet_ms=wire_u16(p+12); break;
    case ECW_EIS:
        r->start_mv=i16(p+2); r->pulse_mv=wire_u16(p+4);
        r->freq_start_mhz=wire_u32(p+6); r->freq_end_mhz=wire_u32(p+10);
        r->points_per_dec=p[14] ? p[14] : 5; r->averages=p[15] ? p[15] : 1; break;
    case ECW_DPV:
        r->start_mv=i16(p+2); r->end_mv=i16(p+4); r->step_mv=wire_u16(p+6);
        r->pulse_mv=wire_u16(p+8); r->pulse_ms=wire_u16(p+10);
        r->period_ms=wire_u16(p+12); r->quiet_ms=wire_u16(p+14); break;
    case ECW_OCP:
        if (p[1]) { return -EINVAL; }
        r->duration_ms=wire_u32(p+2); r->interval_ms=wire_u16(p+6); break;
    default: return ECW_UNSUPPORTED;
    }
    return wire_validate(r);
}
static bool potential(int32_t mv) { return mv>=-1000 && mv<=1000; }
int wire_validate(const struct ecw_request *r)
{
    switch(r->method) {
    case ECW_CV: {
        if (!potential(r->start_mv)||!potential(r->vertex1_mv)||!potential(r->vertex2_mv)||
            !potential(r->end_mv)||r->step_mv<1||r->step_mv>2000||r->rate_mv_s<1||
            !r->cycles||r->cycles>65535||r->step_mv*1000/r->rate_mv_s<50) { return -EINVAL; }
        uint64_t steps=(abs(r->vertex1_mv-r->start_mv)+r->step_mv-1)/r->step_mv;
        steps+=(uint64_t)(2*r->cycles-1)*((abs(r->vertex2_mv-r->vertex1_mv)+r->step_mv-1)/r->step_mv);
        steps+=(abs(r->end_mv-r->vertex2_mv)+r->step_mv-1)/r->step_mv;
        return steps<1000000 ? 0 : -E2BIG;
    }
    case ECW_CA:
        if (!potential(r->start_mv)||!r->cycles||(uint64_t)r->duration_ms*r->cycles>86400000||r->interval_ms<50) { return -EINVAL; }
        break;
    case ECW_DPV:
        if (!potential(r->start_mv)||!potential(r->end_mv)||r->step_mv<1||r->step_mv>2000||
            r->pulse_mv<1||r->pulse_mv>500||!potential(r->start_mv+r->pulse_mv)||
            !potential(r->end_mv+r->pulse_mv)||r->pulse_ms<50||r->period_ms<r->pulse_ms+50) { return -EINVAL; }
        return 0;
    case ECW_OCP:
        if (r->interval_ms<100) { return -EINVAL; } break;
    case ECW_EIS:
        return potential(r->start_mv)&&r->pulse_mv>=1&&r->pulse_mv<=100&&
            abs(r->start_mv)+r->pulse_mv<=1000&&
            r->freq_start_mhz>=1000&&r->freq_start_mhz<=100000000&&
            r->freq_end_mhz>=1000&&r->freq_end_mhz<=100000000&&
            r->points_per_dec>=1&&r->points_per_dec<=255&&r->averages>=1&&r->averages<=255 ? 0:-EINVAL;
    default: return ECW_UNSUPPORTED;
    }
    return r->duration_ms>=r->interval_ms&&r->duration_ms<=86400000&&r->interval_ms<=65535 ? 0 : -EINVAL;
}
size_t wire_frame(uint8_t *out,uint8_t method,uint8_t type,uint16_t session,uint16_t seq,const struct ecw_sample *s)
{
    size_t n=0; uint8_t *p=out+8;
    out[0]=method; out[1]=type; wire_put16(out+2,session); wire_put16(out+4,seq);
    if (type==0 && s) {
        switch(method) {
        case ECW_CV:
            wire_put16(p,sample_mv(s->potential_uv)); wire_put32(p+2,s->current_na); n=6; break;
        case ECW_CA:
            wire_put32(p,s->elapsed_ms); wire_put16(p+4,sample_mv(s->potential_uv)); wire_put32(p+6,s->current_na); n=10; break;
        case ECW_DPV:
            wire_put16(p,sample_mv(s->potential_uv)); wire_put32(p+2,s->base_na); wire_put32(p+6,s->pulse_na); n=10; break;
        case ECW_OCP:
            wire_put32(p,s->elapsed_ms); wire_put32(p+4,s->potential_uv); n=8; break;
        case ECW_EIS:
            wire_put32(p,s->freq_mhz); wire_put32(p+4,s->z_real_mohm); wire_put32(p+8,s->z_imag_mohm); n=12; break;
        default: break;
        }
    }
    wire_put16(out+6,n); return n+8;
}
