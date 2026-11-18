#include <stdint.h>

int ble_adv_init();

int ble_adv_start();

int ble_adv_stop();

/**
 * ble_conn_timeout_start - 启动 BLE 未连接超时计时器。
 *
 * 应在 BLE 广播开始后调用一次。若在超时前建立连接，计时器自动取消；
 * 断连后计时器会自动重置。超时时长由 BLE_TIMEOUT_NO_CONN_MS 宏控制。
 */
void ble_conn_timeout_start(void);
