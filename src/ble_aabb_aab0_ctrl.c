/*
 * 0xAAB0 — Control characteristic
 *
 * Wire protocol (docs/ble-protocol.md §4):
 *   Write format (host → device) — ALL commands share a common 2-byte prefix:
 *     [session_id_lo][session_id_hi][opcode][...]
 *
 *     START  [sid_lo][sid_hi][0x03][method_id][param_len_lo][param_len_hi][params...]
 *     STOP   [sid_lo][sid_hi][0x04][method_id]
 *     RESET  [sid_lo][sid_hi][0x05]
 *     PING   [sid_lo][sid_hi][0xFE]
 *
 *   Read response:
 *     [0] state               (0=idle, 2=run, 0xFF=err)
 *     [1] current_method_id
 *     [2] last_error          (signed)
 *     [3] firmware_minor
 *     [4] firmware_major
 *
 *   Notify ACK (4 bytes):
 *     [0-1] session_id LE u16  [2] opcode  [3] status
 */

#include "inc/ble_aabb_priv.h"
#include "method/inc/ec_method.h"
#include "inc/led.h"

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aab0, LOG_LEVEL_INF);

#define FW_MAJOR  1
#define FW_MINOR  2

#define OP_START  0x03
#define OP_STOP   0x04
#define OP_RESET  0x05
#define OP_PING   0xFE

#define STATUS_ENOTSUP  (-95)  /* Wire status is int8; Zephyr ENOTSUP may be 134. */

#define STATE_WIRE_LEN 5u

static volatile bool s_notify_enabled;

void ble_ccc_cfg_changed_aab0(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    s_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Notify[AAB0] %s", s_notify_enabled ? "on" : "off");
}

static uint16_t build_state_wire(uint8_t *out)
{
    out[0] = (uint8_t)ec_method_state();
    out[1] = (uint8_t)ec_method_current();
    out[2] = (uint8_t)((int8_t)ec_method_last_error());
    out[3] = (uint8_t)FW_MINOR;
    out[4] = (uint8_t)FW_MAJOR;
    return STATE_WIRE_LEN;
}

ssize_t ble_on_aab0_read_request(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset)
{
    uint8_t wire[STATE_WIRE_LEN];
    uint16_t wire_len = build_state_wire(wire);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wire_len);
}

static void send_ack(uint8_t opcode, int8_t status, uint16_t session_id)
{
    if (!s_notify_enabled) return;
    uint8_t pkt[4] = {
        (uint8_t)(session_id & 0xFF),
        (uint8_t)((session_id >> 8) & 0xFF),
        opcode,
        (uint8_t)status,
    };
    (void)bt_gatt_notify(NULL, &aabb_service.attrs[CTRL_AAB0_INDEX],
                         pkt, sizeof(pkt));
}

static int8_t wire_status_from_rc(int rc)
{
    if (rc == -ENOTSUP) {
        return STATUS_ENOTSUP;
    }
    if (rc < -128) {
        return -128;
    }
    if (rc > 127) {
        return 127;
    }
    return (int8_t)rc;
}

ssize_t ble_on_aab0_write_request(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset,
                                  uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    if (offset) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    ARG_UNUSED(flags);

    if (len < 3u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    const uint8_t *d = (const uint8_t *)buf;
    uint16_t session_id     = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
    uint8_t  opcode         = d[2];
    uint16_t ack_session_id = session_id;
    int rc = 0;

    switch (opcode) {
    case OP_START: {
        /* [sid_lo][sid_hi][0x03][id][plen_lo][plen_hi][params...] */
        if (len < 6u) { rc = -EINVAL; break; }
        uint8_t  id   = d[3];
        uint16_t plen = (uint16_t)d[4] | ((uint16_t)d[5] << 8);
        if ((uint32_t)len != 6u + (uint32_t)plen) { rc = -EMSGSIZE; break; }
        rc = ec_method_start((ec_method_id_t)id, session_id, &d[6], plen);
        led_set_ec_running(ec_method_state() == EC_STATE_RUNNING);
        break;
    }

    case OP_STOP:
        /* [sid_lo][sid_hi][0x04][id] */
        if (len != 4u) { rc = -EINVAL; break; }
        if (ec_method_current_session_id() && session_id != ec_method_current_session_id()) { rc = -EINVAL; break; }
        rc = ec_method_stop((ec_method_id_t)d[3]);
        if (rc == 0) {
            led_set_ec_running(false);
        }
        break;

    case OP_RESET:
        rc = ec_method_force_stop();
        led_set_ec_running(false);
        break;

    case OP_PING:
        rc = 0;
        break;

    default:
        LOG_WRN("AAB0: unknown opcode 0x%02x", opcode);
        (void)ble_log_eventf(BLE_LOG_LEVEL_WARN, 0x2101u,
                             "Unknown control opcode 0x%02x", opcode);
        rc = -ENOTSUP;
        break;
    }

    int8_t st = wire_status_from_rc(rc);
    send_ack(opcode, st, ack_session_id);
    return (ssize_t)len;
}

int ble_aab0_init(void)
{
    return 0;
}
