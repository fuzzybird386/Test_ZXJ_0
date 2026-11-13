#ifndef ECW_H
#define ECW_H
#include "ecw_wire.h"
#include <zephyr/sys/atomic.h>
extern atomic_t ecw_stop;
int board_io_init(void);
int board_battery_mv(void);
void board_led_error(bool error);
void board_led_connected(bool connected);
void board_led_advertising(bool advertising);
void board_led_shutdown(void);
int board_power_off(void);
int afe_port_init(void);
int afe_port_error(void);
void afe_port_clear_error(void);
int afe_init(void);
int afe_prepare(bool ocp, int32_t rtia_ohm);
int afe_set_potential(int32_t mv, int32_t *actual_uv);
int afe_current(int32_t *na, uint8_t *flags);
int afe_reduce_current_range(void);
int afe_ocp(int32_t *uv, uint8_t *flags);
int afe_eis_point(const struct ecw_request *r, uint32_t mhz, struct ecw_sample *s);
void afe_idle(void);
bool afe_ready(void);
bool afe_boot_finished(void);
uint16_t afe_chip_id(void);
int afe_select_rtia(int32_t ohm);
int methods_submit(const struct ecw_request *r, uint32_t generation);
void methods_release(void);
bool methods_busy(void);
int methods_stop(uint8_t method);
void methods_state(uint8_t out[5]);
void methods_reset_error(void);
void methods_init(void);
int methods_prepare_poweroff(void);
int ble_link_init(void);
int ble_link_stop(void);
uint32_t ble_session(void);
bool ble_stream_ready(void);
int ble_frame(uint32_t generation, uint8_t method, uint8_t type, uint16_t session, uint16_t seq, const struct ecw_sample *sample);
void ble_log(uint8_t level, uint16_t code, const char *message);
bool ble_error_active(void);
bool ble_connected(void);
int ble_tx_error(void);
#endif
