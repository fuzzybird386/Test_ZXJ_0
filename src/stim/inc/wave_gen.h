#pragma once
#ifndef WAVE_GEN_H__
#define WAVE_GEN_H__

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    WAVE_DC       = 0,
    WAVE_PULSE    = 1,   /* biphasic +A, 0, -A, 0 */
    WAVE_SINE     = 2,
    WAVE_TRIANGLE = 3,
    WAVE_SQUARE   = 4,
    WAVE_CUSTOM   = 5,
} wave_kind_t;

typedef struct {
    wave_kind_t kind;
    int32_t  amplitude_uA;       /* peak amplitude */
    uint32_t freq_mHz;           /* fundamental frequency in mHz (1 Hz = 1000) */
    uint32_t loops;              /* 0 = infinite */
    uint32_t update_rate_hz;     /* DAC sample rate */

    /* WAVE_CUSTOM only. samples in nA. */
    const int32_t *samples_uA;
    uint32_t       n_samples;
} wave_pattern_t;

/* Optional callback fired when a finite-loop pattern completes. */
typedef void (*wave_done_cb_t)(void);

int  wave_gen_init(void);

int  wave_gen_start(const wave_pattern_t *p);
int  wave_gen_stop(void);
bool wave_gen_is_running(void);
void wave_gen_set_done_cb(wave_done_cb_t cb);

#endif /* WAVE_GEN_H__ */
