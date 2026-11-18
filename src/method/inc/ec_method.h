#pragma once
#ifndef EC_METHOD_H__
#define EC_METHOD_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Electrochemical method framework.
 *
 * Every supported method (CV, CA, EIS, DPV, OCP, STIM) registers a
 * `ec_method_t` instance. Only one method can run at a time; mutual
 * exclusion is enforced by the registry.
 *
 * Wire protocol IDs match docs/plan.md §4.
 */

typedef enum {
    EC_METHOD_NONE = 0,
    EC_METHOD_CV   = 1,
    EC_METHOD_CA   = 2,
    EC_METHOD_EIS  = 3,
    EC_METHOD_DPV  = 4,
    EC_METHOD_OCP  = 5,
    EC_METHOD_STIM = 6,

    EC_METHOD_COUNT
} ec_method_id_t;

typedef enum {
    EC_STATE_IDLE     = 0,
    EC_STATE_RUNNING  = 2,
    EC_STATE_STOPPING = 3,  /**< Done; deferred END frame pending (wire: RUNNING). */
    EC_STATE_ERROR    = 0xFF,
} ec_state_t;

/* AABF data-frame types (docs/plan.md §5). */
typedef enum {
    EC_FRAME_SAMPLE = 0,
    EC_FRAME_STATUS = 1,
    EC_FRAME_END    = 2,
    EC_FRAME_ERROR  = 0xFF,
} ec_frame_type_t;

/** Tick return status: continue looping or signal natural completion. */
typedef enum {
    EC_TICK_CONTINUE = 0,  /**< Reschedule after out.delay_us. */
    EC_TICK_DONE     = 1,  /**< Last tick; proxy must have torn down hardware. */
} ec_tick_status_t;

#define EC_TICK_PAYLOAD_MAX 200u

/**
 * Output from one tick() call.
 *
 * If len > 0, the runner pushes a BLE AABF frame of frame_type with
 * payload[0..len-1] before acting on status.
 * delay_us is how long to wait before the next tick() (CONTINUE only).
 */
typedef struct {
    ec_tick_status_t status;
    ec_frame_type_t  frame_type;
    uint32_t         delay_us;
    uint16_t         len;
    uint8_t          payload[EC_TICK_PAYLOAD_MAX];
} ec_tick_out_t;

/**
 * Method backend (unified runner interface).
 *
 * start()  Initialises hardware, sets up measurement state, and writes the
 *          initial tick delay (µs) into *first_delay_us.
 *
 * tick()   Called by the runner at the scheduled time.  Advances the state
 *          machine one step, fills out with any data frame to emit, and
 *          returns EC_TICK_CONTINUE or EC_TICK_DONE.  On DONE the proxy
 *          MUST tear down hardware before returning.
 *
 * stop()   Called only on external abort.  Tears down hardware; the runner
 *          pushes the END frame.
 */
typedef struct ec_method {
    ec_method_id_t id;
    const char    *name;

    int              (*start)(const uint8_t *params, uint16_t len,
                              uint32_t *first_delay_us);
    ec_tick_status_t (*tick)(ec_tick_out_t *out);
    void             (*stop)(void);
} ec_method_t;

/* ── Registry ────────────────────────────────────────────────────────── */

int ec_method_init(void);

/** Register a method. Must be called once at boot. */
int ec_method_register(const ec_method_t *m);

/**
 * Start the requested method, passing its parameter blob through.
 * session_id is a host-assigned 16-bit value that is echoed in every AABF
 * frame for this run, allowing the host to discard stale frames from a
 * previous session after a rapid STOP/START sequence.
 * If another method is running or draining, it is stopped before this method
 * starts. Returns -EBUSY only if that preemption cannot complete.
 */
int ec_method_start(ec_method_id_t id, uint16_t session_id,
                    const uint8_t *params, uint16_t len);

/** Stop the currently-running method. No-op if idle. */
int ec_method_stop(ec_method_id_t id);

/** Stop whatever is running, regardless of id (used at disconnect / fault). */
int ec_method_force_stop(void);

ec_state_t     ec_method_state(void);
ec_method_id_t ec_method_current(void);
int            ec_method_last_error(void);
uint16_t       ec_method_current_session_id(void);

/* ── Data path (called by methods, drained by AABF) ───────────────────── */

/**
 * Push a data frame to the AABF stream. `payload` may be NULL when len=0.
 * Safe to call from work-queue context.
 *
 * The framing layer prepends `[method_id][type][seq:u16][len:u16]`.
 */
int ec_data_push(ec_method_id_t id, ec_frame_type_t type,
                 const void *payload, uint16_t len);

#endif /* EC_METHOD_H__ */
