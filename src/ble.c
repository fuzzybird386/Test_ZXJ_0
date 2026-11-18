#include "inc/ble.h"

#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include "inc/ble_conn.h"
#include "inc/ble_bat.h"
#include "inc/ble_aabb.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	LOG_INF("MTU updated: TX=%d RX=%d bytes", tx, rx);
}

static struct bt_gatt_cb gatt_callbacks = { .att_mtu_updated = mtu_updated };

int ble_init(void)
{
  int err;

  err = bt_enable(NULL);
  if (err) {
    return err;
  }
  bt_gatt_cb_register(&gatt_callbacks);

  err = ble_aabb_init();
  if (err) {
    return err;
  }

  err = ble_bat_init();
  if (err) {
    LOG_WRN("Battery service init failed (%d), continuing", err);
  }

  err = ble_adv_init();
  if (err) {
    return err;
  }

  err = ble_adv_start();
  if (err) {
    return err;
  }

  /* 广播已启动，开始未连接超时计时 */
  ble_conn_timeout_start();

  return 0;
}

int ble_uninit(void)
{
  return 0;
}
