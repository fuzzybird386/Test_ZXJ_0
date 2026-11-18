/* OCP shell — delegates to ec_proxy_ocp. */
#include "inc/ec_method.h"
#include "proxy/ec_proxy.h"

static int ocp_start(const uint8_t *p, uint16_t l, uint32_t *d) { return ec_proxy_ocp_start(p, l, d); }
static ec_tick_status_t ocp_tick(ec_tick_out_t *o)               { return ec_proxy_ocp_tick(o); }
static void ocp_stop(void)                                        { ec_proxy_ocp_stop(); }

const ec_method_t method_ocp = {
    .id = EC_METHOD_OCP, .name = "OCP",
    .start = ocp_start, .tick = ocp_tick, .stop = ocp_stop,
};
