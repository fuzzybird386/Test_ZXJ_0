/*
 * test_stim_wave.c — Boot-time staircase waveform test for wave_gen + DAC80501.
 *
 * Enabled by defining ECW_STIM_WAVE_TEST=1 (e.g. in CMakeLists or prj.conf).
 * Has no effect in normal builds.
 *
 * Waveform: staircase from −1000 µA to +1000 µA in 250 µA steps, then back
 * down — 16 steps total, each step held for 2 s.  Loops indefinitely.
 *
 *  Step index:  0     1     2     3     4    5    6    7
 *  Current µA: -1000 -750 -500  -250    0  250  500  750
 *  Step index:  8     9    10    11    12   13   14   15
 *  Current µA: 1000  750  500   250    0  -250 -500 -750
 *  → wraps back to step 0 (−1000 µA)
 */

#include "inc/test.h"
#include "inc/power.h"
#include "stim/inc/dac80501.h"
#include "stim/inc/wave_gen.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(test_stim_wave, LOG_LEVEL_INF);

#if defined(ECW_STIM_WAVE_TEST) && ECW_STIM_WAVE_TEST

/* 16-step staircase (µA): −1000 → +1000 → back (exclusive of repeated peaks) */
static const int32_t s_staircase_uA[] = {
    -1000, -750, -500, -250,
        0,  250,  500,  750,
     1000,  750,  500,  250,
        0, -250, -500, -750,
};

#define STAIRCASE_STEPS  ARRAY_SIZE(s_staircase_uA)

/* update_rate_hz=1 → timer fires every 1 s; n_samples=STEPS → one full
 * staircase cycle = 16 s.  We want 2 s per step, so set update_rate_hz=1
 * and repeat each sample twice by duplicating the table.
 *
 * Simpler: set update_rate_hz = 1 and n_samples = STEPS, but that gives
 * 1 s per step.  To get 2 s per step we just set update_rate_hz = 1 and
 * duplicate every entry in the array (32 entries at 1 Hz = 2 s per step).
 */
static const int32_t s_staircase_2s_uA[] = {
    -1000, -1000,
     -750,  -750,
     -500,  -500,
     -250,  -250,
        0,     0,
      250,   250,
      500,   500,
      750,   750,
     1000,  1000,
      750,   750,
      500,   500,
      250,   250,
        0,     0,
     -250,  -250,
     -500,  -500,
     -750,  -750,
};

static const wave_pattern_t s_stair_pattern = {
    .kind          = WAVE_CUSTOM,
    .amplitude_uA  = 1000,       /* informational only for CUSTOM */
    .freq_mHz      = 0,           /* not used for CUSTOM */
    .loops         = 0,           /* infinite */
    .update_rate_hz = 1,          /* 1 Hz → each sample = 1 s; table duplicated → 2 s/step */
    .samples_uA    = s_staircase_2s_uA,
    .n_samples     = ARRAY_SIZE(s_staircase_2s_uA),
};

int stim_wave_test_init(void)
{
    LOG_INF("=== STIM WAVE TEST MODE ===");
    LOG_INF("Staircase: -1000..+1000 µA, 250 µA/step, 2 s/step, infinite loop");

    /* 1. Enable external stim power rail (±3.3 V). */
    int err = power_stim_rail_set(true);
    if (err) {
        LOG_ERR("Failed to enable stim rail (%d)", err);
        return err;
    }
    LOG_INF("Stim rail ON");

    /* 2. Settle delay, then re-init the DAC now that the rail is stable.
     *    dac80501_init() in main() runs before the stim rail is on; calling
     *    it again here ensures the soft-reset and GAIN/CONFIG registers are
     *    correctly written to the chip while it is powered.
     */
    k_msleep(10);
    err = dac80501_init();
    if (err) {
        LOG_ERR("dac80501_init (post-rail) failed (%d)", err);
        (void)power_stim_rail_set(false);
        return err;
    }

    /* 3. Start the waveform — runs forever in timer ISR context. */
    err = wave_gen_start(&s_stair_pattern);
    if (err) {
        LOG_ERR("wave_gen_start failed (%d)", err);
        (void)power_stim_rail_set(false);
        return err;
    }

    LOG_INF("Staircase waveform started — will not stop until reboot");
    return 0;
}

#else /* ECW_STIM_WAVE_TEST not enabled */

int stim_wave_test_init(void)
{
    return 0; /* no-op in normal builds */
}

#endif /* ECW_STIM_WAVE_TEST */
