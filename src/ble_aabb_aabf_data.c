#include "inc/ble_aabb_priv.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>
struct frame { uint16_t len; uint8_t data[208]; };
K_MSGQ_DEFINE(frames, sizeof(struct frame), 64, 4);
K_MUTEX_DEFINE(stream_lock);
static atomic_t subscribed;
static struct frame pending;
static bool have_pending;
void ble_ccc_cfg_changed_aabf(const struct bt_gatt_attr *attr,uint16_t value) {
 ARG_UNUSED(attr);
 k_mutex_lock(&stream_lock,K_FOREVER);
 atomic_set(&subscribed,value == BT_GATT_CCC_NOTIFY);
 if (!atomic_get(&subscribed)) { k_msgq_purge(&frames); have_pending=false; }
 k_mutex_unlock(&stream_lock);
}
bool ble_aabf_can_accept_frame(void) {
 return atomic_get(&subscribed) && k_msgq_num_free_get(&frames) >= 3;
}
int ble_aabf_notify_commit(const uint8_t *data,uint16_t len) {
 if (!data || !len || len>sizeof(pending.data)) return -EINVAL;
 if (!atomic_get(&subscribed)) return -ENOTCONN;
 struct frame f = {.len=len}; memcpy(f.data,data,len);
 return k_msgq_put(&frames,&f,K_NO_WAIT) ? -ENOBUFS : 0;
}
int ble_aabf_loop(void) {
 int rc=0;
 k_mutex_lock(&stream_lock,K_FOREVER);
 for (int i=0; i<8 && atomic_get(&subscribed); i++) {
  if (!have_pending) { if (k_msgq_get(&frames,&pending,K_NO_WAIT)) break; have_pending=true; }
  rc=bt_gatt_notify(NULL,&aabb_service.attrs[DATA_AABF_INDEX],pending.data,pending.len);
  if (rc) break; /* Retain the frame and retry, preserving order. */
  have_pending=false;
 }
 k_mutex_unlock(&stream_lock); return rc;
}
int ble_aabf_init(void) { return 0; }
