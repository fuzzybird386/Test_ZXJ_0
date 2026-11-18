#pragma once
#ifndef BLE_AABB_H__
#define BLE_AABB_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * BLE Service 0xAABB — Electrochemical Workstation
 *
 * Characteristics:
 *   0xAAB0  Control  (READ / WRITE / NOTIFY)
 *   0xAABF  Data     (READ / NOTIFY)
 *   0xAABE  Log      (READ / WRITE / NOTIFY)
 */

typedef enum {
    BLE_LOG_LEVEL_INFO  = 1,
    BLE_LOG_LEVEL_WARN  = 2,
    BLE_LOG_LEVEL_ERROR = 3,
} ble_log_level_t;

int ble_aabb_init(void);

/** Push raw data to AABF notify stream. Returns 0 on success. */
int ble_aabf_notify_commit(const uint8_t *data, uint16_t len);

/** True when notifications are enabled and buffer is free. */
bool ble_aabf_can_accept_frame(void);

/** Push a device log to AABE. ERROR logs latch the pending app indicator. */
int ble_log_event(ble_log_level_t level, uint16_t code, const char *message);
int ble_log_eventf(ble_log_level_t level, uint16_t code, const char *fmt, ...);

#endif /* BLE_AABB_H__ */
