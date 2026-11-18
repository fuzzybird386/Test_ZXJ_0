#pragma once
#ifndef BLE_AABB_PRIV_H__
#define BLE_AABB_PRIV_H__

#include "ble_aabb.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

/*
 * Attribute indices within aabb_service (BT_GATT_SERVICE_DEFINE):
 *   [0] Primary Service
 *   [1] Char declaration  (AAB0)
 *   [2] Char value        (AAB0)  ← CTRL_AAB0_INDEX
 *   [3] CCC               (AAB0)  ← CTRL_AAB0_CCC_INDEX
 *   [4] Char declaration  (AABF)
 *   [5] Char value        (AABF)  ← DATA_AABF_INDEX
 *   [6] CCC               (AABF)  ← DATA_AABF_CCC_INDEX
 *   [7] Char declaration  (AABE)
 *   [8] Char value        (AABE)  ← LOG_AABE_INDEX
 *   [9] CCC               (AABE)  ← LOG_AABE_CCC_INDEX
 */
#define CTRL_AAB0_INDEX      2
#define CTRL_AAB0_CCC_INDEX  3
#define DATA_AABF_INDEX      5
#define DATA_AABF_CCC_INDEX  6
#define LOG_AABE_INDEX       8
#define LOG_AABE_CCC_INDEX   9

extern struct bt_uuid_128 aabb_service_uuid;
extern struct bt_uuid_128 aab0_char_uuid;
extern struct bt_uuid_128 aabf_char_uuid;
extern struct bt_uuid_128 aabe_char_uuid;

extern const struct bt_gatt_service_static aabb_service;

/* AAB0 — Control characteristic callbacks */
void    ble_ccc_cfg_changed_aab0(const struct bt_gatt_attr *attr, uint16_t value);
ssize_t ble_on_aab0_read_request(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  void *buf, uint16_t len, uint16_t offset);
ssize_t ble_on_aab0_write_request(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset,
                                   uint8_t flags);

/* AABF — Data stream characteristic callbacks */
void ble_ccc_cfg_changed_aabf(const struct bt_gatt_attr *attr, uint16_t value);

/* AABE — Device log characteristic callbacks */
void    ble_ccc_cfg_changed_aabe(const struct bt_gatt_attr *attr, uint16_t value);
ssize_t ble_on_aabe_read_request(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  void *buf, uint16_t len, uint16_t offset);
ssize_t ble_on_aabe_write_request(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset,
                                   uint8_t flags);

/* Sub-module init / loop */
int ble_aab0_init(void);
int ble_aabf_init(void);
int ble_aabf_loop(void);
int ble_aabe_init(void);
int ble_aabe_loop(void);

#endif /* BLE_AABB_PRIV_H__ */
