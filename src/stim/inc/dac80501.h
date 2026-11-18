#pragma once
#ifndef DAC80501_H__
#define DAC80501_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * DAC80501 — 16-bit single-channel SPI DAC (TI).
 *
 * Bus: bit-banged SPI on P0.04 (SCK) / P0.01 (MOSI) / P0.03 (CS).
 * Mode 1 (CPOL=0, CPHA=1), MSB first, 24-bit frames: [cmd:8][data:16].
 *
 * Reference: internal 2.5 V, gain ÷2×2 → 0..2.5 V output.
 *
 * Output stage maps:
 *   V_dac (0..2.5 V) ─── −1.25 V offset ───►  V_signal (−1.25..+1.25 V)
 *                                              ─── V→I (1 V = 1 mA) ──►
 *                                              I_out (−1.25..+1.25 mA)
 */

int  dac80501_init(void);
int  dac80501_soft_reset(void);

/* Direct DAC output. mv ∈ [0, 2500]. */
int  dac80501_set_voltage_mv(uint16_t mv);

/* Convenience: write a signed differential voltage in µV (−1_250_000..+1_250_000). */
int  dac80501_set_signal_uv(int32_t uv);

/* Convenience: write an output current in nA (−1_250_000..+1_250_000). */
int  dac80501_set_current_na(int32_t na);

/* Convenience: write an output current in µA (−1250..+1250). */
int  dac80501_set_current_uA(int32_t uA);

/* Set output to the safe midpoint (0 µA / 0 V signal) without disabling. */
int  dac80501_set_zero(void);

#endif /* DAC80501_H__ */
