#include "inc/ec_method.h"
#include "../inc/ble_aabb.h"
#include <zephyr/kernel.h>
#include <errno.h>
#include "../ad5941_port/ad5941_port.h"
#include <string.h>
K_MUTEX_DEFINE(s_lock);
K_MUTEX_DEFINE(s_control);
static struct k_work_q measurement_queue;
K_THREAD_STACK_DEFINE(measurement_stack, 4096);
static const ec_method_t *s_methods[EC_METHOD_COUNT], *s_current;
static ec_state_t s_state;
static int s_last_error;
static uint16_t s_seq, s_session_id;
static void runner(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(s_runner, runner);
static void finish(int err) {
    if (err && s_current && s_current->stop) s_current->stop();
    if (s_current) {
        if (err) { int8_t wire = err < -128 ? -128 : err; ec_data_push(s_current->id, EC_FRAME_ERROR, &wire, 1); }
        ec_data_push(s_current->id, EC_FRAME_END, NULL, 0);
    }
    s_last_error = err;
    s_state = err ? EC_STATE_ERROR : EC_STATE_IDLE;
    s_current = NULL;
    s_session_id = 0;
}
static void runner(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&s_lock, K_FOREVER);
    if (s_state != EC_STATE_RUNNING || !s_current) { k_mutex_unlock(&s_lock); return; }
    /* Reserve two queue entries for ERROR and END. Never overwrite a sample. */
    if (!ble_aabf_can_accept_frame()) { finish(-ENOBUFS); k_mutex_unlock(&s_lock); return; }
    ec_tick_out_t out = { .frame_type = EC_FRAME_SAMPLE };
    uint32_t began = k_cycle_get_32();
    ec_tick_status_t status = s_current->tick(&out);
    int rc = ad5941_port_error();
    if (!rc && out.len) rc = ec_data_push(s_current->id, out.frame_type, out.payload, out.len);
    if (rc) finish(rc);
    else if (status == EC_TICK_DONE) {
        int err = out.frame_type == EC_FRAME_ERROR && out.len ? (int8_t)out.payload[0] : 0;
        ec_data_push(s_current->id, EC_FRAME_END, NULL, 0);
        s_last_error = err; s_state = err ? EC_STATE_ERROR : EC_STATE_IDLE;
        s_current = NULL; s_session_id = 0;
    } else {
        uint32_t spent = k_cyc_to_us_floor32(k_cycle_get_32() - began);
        uint32_t delay = out.delay_us > spent ? out.delay_us - spent : 100;
        k_work_reschedule_for_queue(&measurement_queue, &s_runner, K_USEC(delay));
    }
    k_mutex_unlock(&s_lock);
}
int ec_method_init(void) {
    memset(s_methods,0,sizeof(s_methods)); s_state = EC_STATE_IDLE;
    k_work_queue_start(&measurement_queue, measurement_stack, K_THREAD_STACK_SIZEOF(measurement_stack), 5, NULL);
    k_thread_name_set(&measurement_queue.thread, "ec_measurement");
    return 0;
}
int ec_method_register(const ec_method_t *m) {
    if (!m || m->id <= 0 || m->id >= EC_METHOD_COUNT) return -EINVAL;
    s_methods[m->id] = m; return 0;
}
int ec_method_start(ec_method_id_t id, uint16_t sid, const uint8_t *params, uint16_t len) {
    if (id <= 0 || id >= EC_METHOD_COUNT || !s_methods[id]) return -ENOTSUP;
    k_mutex_lock(&s_control,K_FOREVER);
    k_mutex_lock(&s_lock,K_FOREVER);
    int rc;
    if (s_state == EC_STATE_RUNNING || s_state == EC_STATE_STOPPING) { rc=-EBUSY; goto exit; }
    if (!ble_aabf_can_accept_frame()) { rc=-EAGAIN; goto exit; }
    uint32_t delay=0;
    rc = s_methods[id]->start(params,len,&delay);
    if (!rc && ad5941_port_error()) {
        rc = ad5941_port_error();
        if (s_methods[id]->stop) s_methods[id]->stop();
    }
    s_last_error=rc;
    if (!rc) {
        s_current=s_methods[id]; s_session_id=sid; s_seq=0; s_state=EC_STATE_RUNNING;
        k_work_reschedule_for_queue(&measurement_queue,&s_runner,K_USEC(delay));
    } else { s_state=EC_STATE_ERROR; }
exit:
    k_mutex_unlock(&s_lock); k_mutex_unlock(&s_control); return rc;
}
int ec_method_stop(ec_method_id_t id) {
    k_mutex_lock(&s_control,K_FOREVER); k_mutex_lock(&s_lock,K_FOREVER);
    if (!s_current) { k_mutex_unlock(&s_lock); k_mutex_unlock(&s_control); return 0; }
    if (id != EC_METHOD_NONE && id != s_current->id) { k_mutex_unlock(&s_lock); k_mutex_unlock(&s_control); return -EINVAL; }
    s_state=EC_STATE_STOPPING;
    k_mutex_unlock(&s_lock);
    /* Separate queue: safe even when disconnect is on the system workqueue. */
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&s_runner,&sync);
    k_mutex_lock(&s_lock,K_FOREVER);
    if (s_current->stop) s_current->stop();
    finish(0);
    k_mutex_unlock(&s_lock); k_mutex_unlock(&s_control); return 0;
}
int ec_method_force_stop(void) { return ec_method_stop(EC_METHOD_NONE); }
ec_state_t ec_method_state(void) { return s_state == EC_STATE_STOPPING ? EC_STATE_RUNNING : s_state; }
ec_method_id_t ec_method_current(void) { return s_current ? s_current->id : EC_METHOD_NONE; }
int ec_method_last_error(void) { return s_last_error; }
uint16_t ec_method_current_session_id(void) { return s_session_id; }
/* ── Data push → AABF framing ────────────────────────────────────────── */

/*
 * Frame layout (8-byte header):
 *   [0] method_id
 *   [1] type
 *   [2-3] session_id (LE u16)
 *   [4-5] seq (LE u16)
 *   [6-7] payload len (LE u16)
 *   [8…] payload
 */
#define FRAME_HEADER_LEN  8u
#define FRAME_PAYLOAD_MAX 200u  /* matches conservative MTU after L2CAP overhead */

int ec_data_push(ec_method_id_t id, ec_frame_type_t type,
                 const void *payload, uint16_t len)
{
    if (len > FRAME_PAYLOAD_MAX) {
        return -EMSGSIZE;
    }

    uint8_t buf[FRAME_HEADER_LEN + FRAME_PAYLOAD_MAX];
    uint16_t seq;

    /* Atomic seq increment (small critical section, ISR-safe). */
    unsigned int key = irq_lock();
    seq = s_seq++;
    irq_unlock(key);

    buf[0] = (uint8_t)id;
    buf[1] = (uint8_t)type;
    buf[2] = (uint8_t)(s_session_id & 0xFF);
    buf[3] = (uint8_t)((s_session_id >> 8) & 0xFF);
    buf[4] = (uint8_t)(seq & 0xFF);
    buf[5] = (uint8_t)((seq >> 8) & 0xFF);
    buf[6] = (uint8_t)(len & 0xFF);
    buf[7] = (uint8_t)((len >> 8) & 0xFF);
    if (payload && len) {
        memcpy(&buf[FRAME_HEADER_LEN], payload, len);
    }

    return ble_aabf_notify_commit(buf, (uint16_t)(FRAME_HEADER_LEN + len));
}
