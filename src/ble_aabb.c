#include "inc/ble_aabb_priv.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aabb, LOG_LEVEL_INF);

/*
 **************************************************
 *  Bluetooth LE — Service 0xAABB
 *  Electrochemical Workstation (ECW)
 **************************************************
 */

/* Service UUID */
struct bt_uuid_128 aabb_service_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x0000AABB, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb));

/* 0xAAB0 — Control (READ / WRITE / NOTIFY) */
struct bt_uuid_128 aab0_char_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x0000AAB0, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb));

/* 0xAABF — Data stream (READ / NOTIFY) */
struct bt_uuid_128 aabf_char_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x0000AABF, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb));

/* 0xAABE — Device log (READ / WRITE / NOTIFY) */
struct bt_uuid_128 aabe_char_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x0000AABE, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb));

BT_GATT_SERVICE_DEFINE(aabb_service,
    BT_GATT_PRIMARY_SERVICE(&aabb_service_uuid),

    /* Control */
    BT_GATT_CHARACTERISTIC(&aab0_char_uuid.uuid,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP |
        BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        ble_on_aab0_read_request, ble_on_aab0_write_request, NULL),
    BT_GATT_CCC(ble_ccc_cfg_changed_aab0, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* Data stream */
    BT_GATT_CHARACTERISTIC(&aabf_char_uuid.uuid,
        BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        NULL, NULL, NULL),
    BT_GATT_CCC(ble_ccc_cfg_changed_aabf, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* Device log */
    BT_GATT_CHARACTERISTIC(&aabe_char_uuid.uuid,
        BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ |
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        ble_on_aabe_read_request, ble_on_aabe_write_request, NULL),
    BT_GATT_CCC(ble_ccc_cfg_changed_aabe, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/*
 **************************************************
 *  Worker loop
 **************************************************
 */

#define LOOPER_INTERVAL_MS 10u

static void looper_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(looper_work, looper_work_handler);

static void looper_work_handler(struct k_work *work)
{
    ble_aabf_loop();
    ble_aabe_loop();
    k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(LOOPER_INTERVAL_MS));
}

int ble_aabb_init(void)
{
    int err;

    err = ble_aab0_init();
    if (err) {
        return err;
    }

    err = ble_aabf_init();
    if (err) {
        return err;
    }

    err = ble_aabe_init();
    if (err) {
        return err;
    }

    /* Delay 200 ms so all Bluetooth internals are settled before the loop starts. */
    k_work_schedule(&looper_work, K_MSEC(200));
    return 0;
}
