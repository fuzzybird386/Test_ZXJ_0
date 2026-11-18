#include "inc/wave_gen.h"
#include "inc/dac80501.h"

#include <errno.h>
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wave_gen, LOG_LEVEL_INF);

/*
 * Timer-driven waveform player.
 *
 * The Zephyr k_timer fires at update_rate_hz; each tick computes the next
 * sample (in nA) for the configured pattern and calls dac80501_set_current_na().
 *
 * Design choice — using k_timer (which dispatches in ISR context for the
 * expiry callback, then schedules the user work-handler) keeps timing jitter
 * low while giving methods a safe place to react to completion via a worker.
 */

static struct k_timer  s_timer;
static struct k_work   s_done_work;
static volatile bool   s_running;
static wave_pattern_t  s_pattern;
static volatile uint32_t s_sample_idx;     /* sample within current cycle  */
static volatile uint32_t s_cycle;          /* completed cycles             */
static volatile uint32_t s_samples_per_cycle;
static wave_done_cb_t  s_done_cb;

static int32_t compute_sample_uA(uint32_t idx, uint32_t spc)
{
    int32_t amp = s_pattern.amplitude_uA;
    if (spc == 0u) {
        return 0;
    }
    switch (s_pattern.kind) {
    case WAVE_DC:
        return amp;
    case WAVE_PULSE: {
        /* Quarter cycles: +A, 0, -A, 0 */
        uint32_t q = spc / 4u;
        if (q == 0u) q = 1u;
        uint32_t phase = idx / q;
        switch (phase) {
        case 0: return  amp;
        case 1: return  0;
        case 2: return -amp;
        default: return 0;
        }
    }
    case WAVE_SINE: {
        double angle = (2.0 * 3.14159265358979 * (double)idx) / (double)spc;
        return (int32_t)((double)amp * sin(angle));
    }
    case WAVE_TRIANGLE: {
        uint32_t half = spc / 2u;
        if (half == 0u) half = 1u;
        if (idx < half) {
            /* rising  −A → +A */
            int64_t v = (int64_t)amp * (2 * (int64_t)idx - (int64_t)half) / (int64_t)half;
            return (int32_t)v;
        } else {
            /* falling +A → −A */
            int64_t k = (int64_t)idx - (int64_t)half;
            int64_t v = (int64_t)amp * ((int64_t)half - 2 * k) / (int64_t)half;
            return (int32_t)v;
        }
    }
    case WAVE_SQUARE:
        return (idx < spc / 2u) ? amp : -amp;
    case WAVE_CUSTOM: {
        if (!s_pattern.samples_uA || s_pattern.n_samples == 0u) {
            return 0;
        }
        return s_pattern.samples_uA[idx % s_pattern.n_samples];
    }
    }
    return 0;
}

static void done_work_handler(struct k_work *w)
{
    ARG_UNUSED(w);
    if (s_done_cb) {
        s_done_cb();
    }
}

static void timer_expiry(struct k_timer *t)
{
    ARG_UNUSED(t);
    if (!s_running) {
        return;
    }

    int32_t uA = compute_sample_uA(s_sample_idx, s_samples_per_cycle);
    (void)dac80501_set_current_uA(uA);

    s_sample_idx++;
    if (s_sample_idx >= s_samples_per_cycle) {
        s_sample_idx = 0;
        s_cycle++;
        if (s_pattern.loops != 0u && s_cycle >= s_pattern.loops) {
            s_running = false;
            k_timer_stop(&s_timer);
            (void)dac80501_set_zero();
            k_work_submit(&s_done_work);
        }
    }
}

int wave_gen_init(void)
{
    k_timer_init(&s_timer, timer_expiry, NULL);
    k_work_init(&s_done_work, done_work_handler);
    return 0;
}

int wave_gen_start(const wave_pattern_t *p)
{
    if (!p || p->update_rate_hz == 0u) {
        return -EINVAL;
    }
    if (s_running) {
        return -EBUSY;
    }

    s_pattern = *p;

    /* samples_per_cycle = update_rate_hz * 1000 / freq_mHz */
    if (p->kind == WAVE_DC) {
        s_samples_per_cycle = 1u;            /* doesn't matter */
    } else if (p->kind == WAVE_CUSTOM) {
        s_samples_per_cycle = p->n_samples ? p->n_samples : 1u;
    } else {
        if (p->freq_mHz == 0u) {
            return -EINVAL;
        }
        uint64_t spc = (uint64_t)p->update_rate_hz * 1000ull / p->freq_mHz;
        if (spc < 2u) spc = 2u;
        s_samples_per_cycle = (uint32_t)spc;
    }

    s_sample_idx = 0;
    s_cycle = 0;
    s_running = true;

    uint32_t period_us = 1000000u / p->update_rate_hz;
    if (period_us == 0u) period_us = 1u;
    k_timer_start(&s_timer, K_USEC(period_us), K_USEC(period_us));

    LOG_INF("wave_gen start kind=%u amp=%d uA freq=%u mHz rate=%u Hz spc=%u",
            p->kind, p->amplitude_uA, p->freq_mHz, p->update_rate_hz, s_samples_per_cycle);
    return 0;
}

int wave_gen_stop(void)
{
    if (!s_running) {
        return 0;
    }
    s_running = false;
    k_timer_stop(&s_timer);
    (void)dac80501_set_zero();
    LOG_INF("wave_gen stopped");
    return 0;
}

bool wave_gen_is_running(void) { return s_running; }
void wave_gen_set_done_cb(wave_done_cb_t cb) { s_done_cb = cb; }
