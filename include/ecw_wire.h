#ifndef ECW_WIRE_H
#define ECW_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum ecw_method { ECW_IDLE=0, ECW_CV=1, ECW_CA=2, ECW_EIS=3, ECW_DPV=4, ECW_OCP=5, ECW_STIM=6 };
#define ECW_OP_START 3
#define ECW_OP_STOP 4
#define ECW_OP_RESET 5
#define ECW_OP_PING 254
#define ECW_UNSUPPORTED (-95)
struct ecw_request {
    enum ecw_method method;
    uint16_t session_id;
    uint8_t gear;
    int32_t start_mv, vertex1_mv, vertex2_mv, end_mv, step_mv, rate_mv_s;
    uint32_t cycles, period_ms, pulse_ms, duration_ms, interval_ms, quiet_ms;
    int32_t pulse_mv, rtia_ohm;
    uint32_t freq_start_mhz, freq_end_mhz, points_per_dec, averages;
};
struct ecw_sample {
    uint8_t method, flags;
    uint32_t index, elapsed_ms;
    int32_t potential_uv, current_na, base_na, pulse_na;
    uint32_t freq_mhz;
    int32_t z_real_mohm, z_imag_mohm;
};
#define ECW_FLAG_ADC_RAIL 1U
uint16_t wire_u16(const uint8_t *p);
uint32_t wire_u32(const uint8_t *p);
void wire_put16(uint8_t *p, uint16_t v);
void wire_put32(uint8_t *p, uint32_t v);
int8_t wire_error(int rc);
int wire_decode_start(const uint8_t *p, size_t len, struct ecw_request *r);
int wire_validate(const struct ecw_request *r);
size_t wire_frame(uint8_t *out, uint8_t method, uint8_t type, uint16_t session, uint16_t seq, const struct ecw_sample *s);
#endif
