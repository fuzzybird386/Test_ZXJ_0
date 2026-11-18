/* Cyclic Voltammetry shell — delegates to ec_proxy_cv. */
#include "inc/ec_method.h"
#include "proxy/ec_proxy.h"

static int cv_start(const uint8_t *p, uint16_t l, uint32_t *d) { return ec_proxy_cv_start(p, l, d); }
static ec_tick_status_t cv_tick(ec_tick_out_t *o)               { return ec_proxy_cv_tick(o); }
static void cv_stop(void)                                        { ec_proxy_cv_stop(); }

const ec_method_t method_cv = {
    .id = EC_METHOD_CV, .name = "CV",
    .start = cv_start, .tick = cv_tick, .stop = cv_stop,
};
