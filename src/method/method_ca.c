/* Chronoamperometry shell — delegates to ec_proxy_ca. */
#include "inc/ec_method.h"
#include "proxy/ec_proxy.h"

static int ca_start(const uint8_t *p, uint16_t l, uint32_t *d) { return ec_proxy_ca_start(p, l, d); }
static ec_tick_status_t ca_tick(ec_tick_out_t *o)               { return ec_proxy_ca_tick(o); }
static void ca_stop(void)                                        { ec_proxy_ca_stop(); }

const ec_method_t method_ca = {
    .id = EC_METHOD_CA, .name = "CA",
    .start = ca_start, .tick = ca_tick, .stop = ca_stop,
};
