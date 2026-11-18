/*
 * Electrical stimulation — packaged as an electrochemical method.
 *
 * STIM uses two INDEPENDENT electrodes that bypass the WE mux matrix and
 * the AD5941 path entirely. Mutual exclusion with EC methods is still
 * enforced by the ec_method registry (only one method runs at a time);
 * STIM just doesn't touch the mux or the AFE.
 *
 * Pipeline: ec_stim_params_t  →  wave_gen (timer) → DAC80501 (current) →
 *           stim power rail → external electrodes.
 *
 * Tick strategy: runner calls stim_tick() every 100 ms for STATUS frames.
 * wave_done_cb() (called from wave_gen timer context) only sets a flag;
 * the runner detects it on the next tick and returns EC_TICK_DONE.
 */

#include "inc/ec_method.h"
#include "inc/ec_params.h"
#include "../stim/inc/dac80501.h"
#include "../stim/inc/wave_gen.h"
#include "../inc/power.h"

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(method_stim, LOG_LEVEL_INF);

static ec_stim_params_t s_p;
static volatile bool    s_wave_done;
static int32_t          s_effective_amplitude_uA;

static void wave_done_cb(void)
{
    LOG_INF("STIM wave_gen done");
    s_wave_done = true;
    /* Runner will detect flag on next 100 ms tick and return DONE. */
}

static int32_t apply_polarity_bias(int32_t amplitude_uA, uint8_t polarity_bias)
{
    int64_t mag = (amplitude_uA < 0) ? -(int64_t)amplitude_uA : (int64_t)amplitude_uA;
    if (mag > INT32_MAX) {
        mag = INT32_MAX;
    }

    switch (polarity_bias) {
    case 1:
        return (int32_t)mag;
    case 2:
        return -(int32_t)mag;
    default:
        return amplitude_uA;
    }
}

static int stim_start(const uint8_t *params, uint16_t len, uint32_t *first_delay_us)
{
    if (!params || len < sizeof(ec_stim_params_t)) return -EINVAL;

    memcpy(&s_p, params, sizeof(s_p));
    if (s_p.wave_kind > WAVE_SQUARE) return -ENOTSUP;
    if (s_p.polarity_bias > 2) return -EINVAL;
    if (s_p.update_rate_hz == 0) s_p.update_rate_hz = 1000;
    s_effective_amplitude_uA = apply_polarity_bias(s_p.amplitude_uA, s_p.polarity_bias);
    s_wave_done = false;

    LOG_INF("STIM: kind=%u polarity=%u amp=%d uA freq=%u mHz loops=%u rate=%u Hz",
            s_p.wave_kind, s_p.polarity_bias, s_effective_amplitude_uA, s_p.freq_mHz, s_p.loops,
            s_p.update_rate_hz);

    int err = power_stim_rail_set(true);
    if (err) return err;

    wave_pattern_t wp = {
        .kind           = (wave_kind_t)s_p.wave_kind,
        .amplitude_uA   = s_effective_amplitude_uA,
        .freq_mHz       = s_p.freq_mHz,
        .loops          = s_p.loops,
        .update_rate_hz = s_p.update_rate_hz,
        .samples_uA     = NULL,
        .n_samples      = 0,
    };
    wave_gen_set_done_cb(wave_done_cb);
    err = wave_gen_start(&wp);
    if (err) {
        (void)power_stim_rail_set(false);
        return err;
    }

    *first_delay_us = 100000u;  /* first STATUS frame after 100 ms */
    return 0;
}

static ec_tick_status_t stim_tick(ec_tick_out_t *out)
{
    if (s_wave_done) {
        /* Natural completion. Hardware is already quiesced by wave_gen. */
        (void)power_stim_rail_set(false);
        LOG_INF("STIM done (wave complete)");
        out->len = 0;
        return EC_TICK_DONE;
    }

    /* Emit a STATUS frame. */
    struct __packed {
        uint32_t uptime_ms;
        int32_t  amplitude_uA;
        uint32_t freq_mHz;
        uint8_t  wave_kind;
        uint8_t  running;
    } *status = (void *)out->payload;
    status->uptime_ms    = k_uptime_get_32();
    status->amplitude_uA = s_effective_amplitude_uA;
    status->freq_mHz     = s_p.freq_mHz;
    status->wave_kind    = s_p.wave_kind;
    status->running      = 1;
    out->len        = (uint16_t)sizeof(*status);
    out->frame_type = EC_FRAME_STATUS;
    out->delay_us   = 100000u;  /* 100 ms between STATUS frames */
    return EC_TICK_CONTINUE;
}

static void stim_stop(void)
{
    s_wave_done = false;
    (void)wave_gen_stop();
    (void)dac80501_set_zero();
    (void)power_stim_rail_set(false);
    LOG_INF("STIM stopped (external)");
}

const ec_method_t method_stim = {
    .id    = EC_METHOD_STIM,
    .name  = "STIM",
    .start = stim_start,
    .tick  = stim_tick,
    .stop  = stim_stop,
};
