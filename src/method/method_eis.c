/* EIS shell — delegates to ec_proxy_eis. */
#include "inc/ec_method.h"
#include "proxy/ec_proxy.h"

static int eis_start(const uint8_t *p, uint16_t l, uint32_t *d) { return ec_proxy_eis_start(p, l, d); }
static ec_tick_status_t eis_tick(ec_tick_out_t *o)               { return ec_proxy_eis_tick(o); }
static void eis_stop(void)                                        { ec_proxy_eis_stop(); }

const ec_method_t method_eis = {
    .id = EC_METHOD_EIS, .name = "EIS",
    .start = eis_start, .tick = eis_tick, .stop = eis_stop,
};
