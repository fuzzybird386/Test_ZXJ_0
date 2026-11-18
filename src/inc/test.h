#pragma once
#ifndef EC_TEST_H__
#define EC_TEST_H__

#define ECW_STIM_WAVE_TEST 0

int ec_test_init(void);

/**
 * stim_wave_test_init() — boot-time staircase waveform test.
 *
 * Enabled only when ECW_STIM_WAVE_TEST=1 is defined at compile time.
 * In normal builds this is a no-op that returns 0.
 *
 * What it does:
 *   - Enables the external ±3.3 V stim power rail.
 *   - Starts an infinite staircase waveform: −1000 µA → +1000 µA in
 *     250 µA steps, 2 s per step, then back down, looping forever.
 */
int stim_wave_test_init(void);

#endif /* EC_TEST_H__ */