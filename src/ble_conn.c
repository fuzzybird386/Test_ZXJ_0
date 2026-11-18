#include "inc/ble_conn.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>
#include "inc/led.h"
#include "inc/power.h"
#include "inc/ble_aabb.h"
#include "method/inc/ec_method.h"

LOG_MODULE_REGISTER(blec, LOG_LEVEL_INF);

#ifndef ECW_AUTOTEST_ENABLE
#define ECW_AUTOTEST_ENABLE 0
#endif

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static bool device_connected, shutting_down;
static void shutdown_handler(struct k_work *work) {
 ARG_UNUSED(work); led_off_all(); power_shutdown();
}
static K_WORK_DELAYABLE_DEFINE(shutdown_work, shutdown_handler);
/* Advertising data (ADV) */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),

	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(0xAABB),            /* ECW Service */
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL),   /* Battery Service */
		      BT_UUID_16_ENCODE(BT_UUID_DIS_VAL)),  /* Device Information Service */
};

/* Scan response data (SCAN_RSP) */
static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static void connected(struct bt_conn *conn, uint8_t err) {
 if (err) return;
 if (shutting_down) { bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN); return; }
 device_connected = true;
 led_set_ble_connected(true);
}
static void disconnected(struct bt_conn *conn, uint8_t reason) {
 ARG_UNUSED(conn); ARG_UNUSED(reason);
 if (!device_connected) return;
 device_connected = false; shutting_down = true;
 bt_le_adv_stop();
 led_set_ble_connected(false);
 k_work_reschedule(&shutdown_work, K_SECONDS(4));
 ec_method_force_stop();
}
/* Define connection callback */
BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

int ble_adv_start(void)
{
	/*
	 * 最新 Zephyr 推荐：显式构造参数
	 * - CONNECTABLE：可连接
	 * - 使用 FAST_INT_2：常见的快速广播间隔
	 * - 这里不加 BT_LE_ADV_OPT_USE_NAME，因为我们把 NAME 放在 sd[] 里了
	 */
	const struct bt_le_adv_param *adv_param =
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_SCANNABLE |
				BT_LE_ADV_OPT_CONN |
				BT_LE_ADV_OPT_USE_IDENTITY,
				BT_GAP_ADV_FAST_INT_MIN_2,
				BT_GAP_ADV_FAST_INT_MAX_2,
				NULL);

	int err = bt_le_adv_start(adv_param,
			      ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_INF("Failed to start advertising (err %d)\n", err);
		return err;
	}

	LOG_INF("Advertiser started\n");
	return 0;
}

int ble_adv_stop(void)
{
	return bt_le_adv_stop();
}

int ble_adv_init(void) { return 0; }
void ble_conn_timeout_start(void) {} /* Wait indefinitely before first connection. */
