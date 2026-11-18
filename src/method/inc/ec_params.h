#pragma once
#ifndef EC_PARAMS_H__
#define EC_PARAMS_H__

#include <stdint.h>

/**
 * Public BLE 0xAAB0 START parameter blobs.
 *
 * Every method (CV / CA / EIS / DPV / OCP / STIM) is described by exactly
 * one struct in this file. Both the BLE host and the AD5941 / DAC80501
 * proxies depend ONLY on this header — no proxy is allowed to redefine
 * the wire layout privately. This is the open-closed seam: add a new
 * method by adding a new struct here and a new proxy backend; the BLE
 * shell, the registry, and the existing proxies stay untouched.
 *
 * All structs are packed and little-endian on the wire. Dimensional units
 * are spelled into the field names (mV / uV / uA / nA / mHz / ms / Hz)
 * so there is no ambiguity between firmware and host.
 *
 * ── Working-electrode selection (electrochemical methods only) ────────
 * The board has two ADG704 4:1 muxes wired to a single SE node:
 *   we_index 0..3 → U7 (MSW_EN0=1, A1A0 selects S4..S1 → WE0/WE2/WE1/WE3)
 *   we_index 4..7 → U9 (MSW_EN1=1, A1A0 selects S4..S1 → WE5/WE7/WE4/WE6)
 * `we_index` is the logical electrode number 0..7. The mux driver
 * (src/mux/mux.c) translates it to the physical EN/A1/A0 lines.
 *
 * STIM uses two independent electrodes (no mux involvement) and therefore
 * has no `we_index` field.
 */

/* ── LPTIA current-range gear (CV / CA / DPV) ─────────────────────────────
 * Selects the AD5940 low-power TIA feedback resistor (RTIA), which sets the
 * current measurement range/gain. Higher RTIA → smaller full-scale current
 * but finer resolution (better for nA-level signals); lower RTIA → larger
 * full-scale current (µA–mA). The firmware maps each gear to a concrete
 * LPTIARTIA_* code in src/method/proxy/ec_proxy_lpamp.c. Both host and
 * firmware MUST agree on this enumeration.
 */
enum ec_rtia_gear_t {
    EC_RTIA_GEAR_200R = 0,   /* 200 Ω   — ~mA  full scale (default)         */
    EC_RTIA_GEAR_1K   = 1,   /* 1 kΩ    — ~900 µA                           */
    EC_RTIA_GEAR_4K   = 2,   /* 4 kΩ    — ~225 µA                           */
    EC_RTIA_GEAR_10K  = 3,   /* 10 kΩ   — ~90 µA                            */
    EC_RTIA_GEAR_20K  = 4,   /* 20 kΩ   — ~45 µA                            */
    EC_RTIA_GEAR_100K = 5,   /* 100 kΩ  — ~9 µA                             */
    EC_RTIA_GEAR_256K = 6,   /* 256 kΩ  — ~3.5 µA                           */
    EC_RTIA_GEAR_512K = 7,   /* 512 kΩ  — ~1.8 µA, nA-level resolution      */
    EC_RTIA_GEAR_COUNT
};

/*
 * EIS HSTIA current-range gears. EIS runs on the high-speed loop, whose TIA
 * (HSTIA) has its OWN feedback-resistor set, DISTINCT from the LPTIA gears
 * above. The selected RTIA sets the measurable impedance range: a larger RTIA
 * favours high-impedance samples (better resolution at high |Z|), a smaller
 * RTIA favours low-impedance samples. Host (app) and firmware MUST agree on
 * this enumeration. Codes map 1:1 onto AD5940 HSTIARTIA_* (except the device's
 * 200Ω entry, which we expose as gear 0).
 */
enum ec_hstia_gear_t {
    EC_HSTIA_GEAR_200R = 0,  /* 200 Ω   — low |Z| samples                       */
    EC_HSTIA_GEAR_1K   = 1,  /* 1 kΩ                                            */
    EC_HSTIA_GEAR_5K   = 2,  /* 5 kΩ    — general-purpose default               */
    EC_HSTIA_GEAR_10K  = 3,  /* 10 kΩ                                           */
    EC_HSTIA_GEAR_20K  = 4,  /* 20 kΩ                                           */
    EC_HSTIA_GEAR_40K  = 5,  /* 40 kΩ                                           */
    EC_HSTIA_GEAR_80K  = 6,  /* 80 kΩ                                           */
    EC_HSTIA_GEAR_160K = 7,  /* 160 kΩ  — high |Z| samples                      */
    EC_HSTIA_GEAR_COUNT
};

/* ── Cyclic Voltammetry (id=1) ────────────────────────────────────────── */
typedef struct __packed {
    uint8_t  we_index;          /* 0..7 — picks WEx via mux                */
    uint8_t  rtia_gear;         /* LPTIA current-range gear (enum ec_rtia_gear_t) */
    int16_t  e_begin_mV;
    int16_t  e_vertex1_mV;
    int16_t  e_vertex2_mV;
    int16_t  e_end_mV;
    uint16_t scan_rate_mV_s;
    uint16_t cycles;
    uint16_t step_mV;
    uint16_t quiet_time_ms;
} ec_cv_params_t;

/* ── Chronoamperometry (id=2) ────────────────────────────────────────── */
typedef struct __packed {
    uint8_t  we_index;
    uint8_t  rtia_gear;         /* LPTIA current-range gear (enum ec_rtia_gear_t) */
    int16_t  e_step_mV;
    uint32_t pulse_width_ms;
    uint16_t sample_period_ms;
    uint16_t n_pulses;
    uint16_t quiet_time_ms;
} ec_ca_params_t;

/* ── EIS (id=3) — runs through the AD5941 high-bandwidth loop ─────────── */
typedef struct __packed {
    uint8_t  we_index;
    uint8_t  hstia_gear;        /* HSTIA impedance-range gear (enum ec_hstia_gear_t) */
    int16_t  e_dc_mV;           /* DC bias                                 */
    uint16_t e_ac_mV;           /* peak excitation amplitude               */
    uint32_t freq_start_mHz;
    uint32_t freq_end_mHz;
    uint8_t  points_per_dec;
    uint8_t  cycles_avg;
} ec_eis_params_t;

/* ── Differential Pulse Voltammetry (id=4) ───────────────────────────── */
typedef struct __packed {
    uint8_t  we_index;
    uint8_t  rtia_gear;         /* LPTIA current-range gear (enum ec_rtia_gear_t) */
    int16_t  e_begin_mV;
    int16_t  e_end_mV;
    uint16_t step_mV;
    uint16_t pulse_amp_mV;
    uint16_t pulse_width_ms;
    uint16_t period_ms;
    uint16_t quiet_time_ms;
} ec_dpv_params_t;

/* ── Open-Circuit Potential (id=5) ───────────────────────────────────── */
typedef struct __packed {
    uint8_t  we_index;
    uint8_t  _rsvd;
    uint32_t duration_ms;
    uint16_t sample_period_ms;
} ec_ocp_params_t;

/* ── Electrical Stimulation (id=6) ───────────────────────────────────── */
/* Independent electrode pair — no mux, no AD5941. */
typedef struct __packed {
    uint8_t  wave_kind;         /* enum wave_kind_t                        */
    uint8_t  polarity_bias;
    int32_t  amplitude_uA;
    uint32_t freq_mHz;
    uint32_t loops;
    uint16_t update_rate_hz;
} ec_stim_params_t;

#endif /* EC_PARAMS_H__ */
