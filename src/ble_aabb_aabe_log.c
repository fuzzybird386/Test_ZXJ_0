#include "inc/ble_aabb_priv.h"
#include "inc/led.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(aabe, LOG_LEVEL_INF);

/*
 * 0xAABE - Device log characteristic (READ / WRITE / NOTIFY)
 *
 * Notify/read payload:
 *   [0]    version = 1
 *   [1]    level: 1=INFO, 2=WARN, 3=ERROR
 *   [2-3]  log_id (LE u16)
 *   [4-7]  uptime_ms (LE u32)
 *   [8-9]  code (LE u16)
 *   [10]   flags: bit0=requires_ack, bit1=active_error
 *   [11]   message_len
 *   [12..] UTF-8 message bytes
 *
 * Host write payload:
 *   [0]    action: 1=ignore/acknowledge, 2=handled
 *   [1-2]  log_id (LE u16)
 *
 * A write means the user intentionally dismissed the visible error state.  It
 * clears the active error latch globally; the host is not expected to
 * acknowledge every historical error one by one.
 */

#define LOG_WIRE_VERSION        1u
#define LOG_WIRE_HEADER_LEN     12u
#define LOG_MESSAGE_MAX_LEN     180u
#define LOG_WIRE_MAX_LEN        (LOG_WIRE_HEADER_LEN + LOG_MESSAGE_MAX_LEN)

#define LOG_FLAG_REQUIRES_ACK   0x01u
#define LOG_FLAG_ACTIVE_ERROR   0x02u

#define LOG_ACTION_IGNORE       0x01u
#define LOG_ACTION_HANDLED      0x02u

typedef struct {
    uint16_t        id;
    ble_log_level_t level;
    uint32_t        uptime_ms;
    uint16_t        code;
    char            message[LOG_MESSAGE_MAX_LEN + 1u];
} aabe_log_record_t;

typedef struct {
    aabe_log_record_t latest;
    uint8_t           notify_buf[LOG_WIRE_MAX_LEN];
    uint16_t          notify_len;
    uint16_t          next_id;
    uint16_t          pending_error_id;
    volatile bool     notify_enabled;
    volatile bool     notify_ready;
    bool              has_latest;
} aabe_log_state_t;

static aabe_log_state_t s_aabe;
K_MUTEX_DEFINE(s_aabe_lock);

static uint16_t build_wire_locked(const aabe_log_record_t *rec, uint8_t *out)
{
    size_t msg_len = strnlen(rec->message, LOG_MESSAGE_MAX_LEN);
    uint8_t flags = 0;

    if (rec->level == BLE_LOG_LEVEL_ERROR) {
        flags |= LOG_FLAG_REQUIRES_ACK;
        if (s_aabe.pending_error_id == rec->id) {
            flags |= LOG_FLAG_ACTIVE_ERROR;
        }
    }

    out[0] = LOG_WIRE_VERSION;
    out[1] = (uint8_t)rec->level;
    sys_put_le16(rec->id, &out[2]);
    sys_put_le32(rec->uptime_ms, &out[4]);
    sys_put_le16(rec->code, &out[8]);
    out[10] = flags;
    out[11] = (uint8_t)msg_len;
    memcpy(&out[LOG_WIRE_HEADER_LEN], rec->message, msg_len);
    return (uint16_t)(LOG_WIRE_HEADER_LEN + msg_len);
}

void ble_ccc_cfg_changed_aabe(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);

    k_mutex_lock(&s_aabe_lock, K_FOREVER);
    s_aabe.notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    if (s_aabe.notify_enabled && s_aabe.has_latest) {
        s_aabe.notify_len = build_wire_locked(&s_aabe.latest, s_aabe.notify_buf);
        s_aabe.notify_ready = true;
    } else if (!s_aabe.notify_enabled) {
        s_aabe.notify_ready = false;
    }
    k_mutex_unlock(&s_aabe_lock);

    LOG_INF("Notify[AABE] %s", s_aabe.notify_enabled ? "on" : "off");
}

ssize_t ble_on_aabe_read_request(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset)
{
    uint8_t wire[LOG_WIRE_MAX_LEN];
    uint16_t wire_len = 0;

    k_mutex_lock(&s_aabe_lock, K_FOREVER);
    if (s_aabe.has_latest) {
        wire_len = build_wire_locked(&s_aabe.latest, wire);
    }
    k_mutex_unlock(&s_aabe_lock);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wire_len);
}

ssize_t ble_on_aabe_write_request(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset,
                                  uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len < 3u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *d = (const uint8_t *)buf;
    uint8_t action = d[0];
    uint16_t log_id = sys_get_le16(&d[1]);

    if (action != LOG_ACTION_IGNORE && action != LOG_ACTION_HANDLED) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    k_mutex_lock(&s_aabe_lock, K_FOREVER);
    if (s_aabe.pending_error_id != 0u ||
        (s_aabe.has_latest && s_aabe.latest.level == BLE_LOG_LEVEL_ERROR)) {
        s_aabe.pending_error_id = 0u;
        if (s_aabe.has_latest) {
            s_aabe.notify_len = build_wire_locked(&s_aabe.latest, s_aabe.notify_buf);
            s_aabe.notify_ready = s_aabe.notify_enabled;
        }
    }
    led_red_set(false);
    LOG_INF("AABE error latch cleared by host ack log %u", log_id);
    k_mutex_unlock(&s_aabe_lock);

    return (ssize_t)len;
}

int ble_log_event(ble_log_level_t level, uint16_t code, const char *message)
{
    if (level < BLE_LOG_LEVEL_INFO || level > BLE_LOG_LEVEL_ERROR) {
        return -EINVAL;
    }

    k_mutex_lock(&s_aabe_lock, K_FOREVER);

    uint16_t id = s_aabe.next_id++;
    if (id == 0u) {
        id = s_aabe.next_id++;
    }

    s_aabe.latest.id = id;
    s_aabe.latest.level = level;
    s_aabe.latest.uptime_ms = k_uptime_get_32();
    s_aabe.latest.code = code;
    snprintf(s_aabe.latest.message, sizeof(s_aabe.latest.message), "%s",
             message ? message : "");
    s_aabe.has_latest = true;

    if (level == BLE_LOG_LEVEL_ERROR) {
        s_aabe.pending_error_id = id;
    }

    if (s_aabe.notify_enabled) {
        s_aabe.notify_len = build_wire_locked(&s_aabe.latest, s_aabe.notify_buf);
        s_aabe.notify_ready = true;
    }

    k_mutex_unlock(&s_aabe_lock);
    return 0;
}

int ble_log_eventf(ble_log_level_t level, uint16_t code, const char *fmt, ...)
{
    char msg[LOG_MESSAGE_MAX_LEN + 1u];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);

    return ble_log_event(level, code, msg);
}

int ble_aabe_loop(void)
{
    uint8_t wire[LOG_WIRE_MAX_LEN];
    uint16_t wire_len;

    k_mutex_lock(&s_aabe_lock, K_FOREVER);
    if (!s_aabe.notify_enabled || !s_aabe.notify_ready) {
        k_mutex_unlock(&s_aabe_lock);
        return -EAGAIN;
    }
    wire_len = s_aabe.notify_len;
    memcpy(wire, s_aabe.notify_buf, wire_len);
    s_aabe.notify_ready = false;
    k_mutex_unlock(&s_aabe_lock);

    int err = bt_gatt_notify(NULL, &aabb_service.attrs[LOG_AABE_INDEX], wire, wire_len);
    if (err) {
        LOG_WRN("AABE notify err: %d", err);
    }
    return err;
}

int ble_aabe_init(void)
{
    memset(&s_aabe, 0, sizeof(s_aabe));
    s_aabe.next_id = 1u;
    return 0;
}
