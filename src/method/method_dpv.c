/* DPV shell — delegates to ec_proxy_dpv. */
#include "inc/ec_method.h"
#include "proxy/ec_proxy.h"

static int dpv_start(const uint8_t *p, uint16_t l, uint32_t *d) { return ec_proxy_dpv_start(p, l, d); }
static ec_tick_status_t dpv_tick(ec_tick_out_t *o)               { return ec_proxy_dpv_tick(o); }
static void dpv_stop(void)                                        { ec_proxy_dpv_stop(); }

const ec_method_t method_dpv = {
    .id = EC_METHOD_DPV, .name = "DPV",
    .start = dpv_start, .tick = dpv_tick, .stop = dpv_stop,
};
