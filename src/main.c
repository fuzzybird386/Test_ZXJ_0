#include "ecw.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ecw,LOG_LEVEL_INF);
static atomic_t boot_finished;
bool afe_boot_finished(void) { return atomic_get(&boot_finished)!=0; }
int main(void)
{
    int rc=board_io_init();
    if (rc) { LOG_ERR("Board IO: %d",rc); return rc; }
    /* Make the board discoverable even when AFE power/SPI is faulty. The RX
     * command path refuses START/RESET while initial AFE setup owns SPI. */
    methods_init();
    rc=ble_link_init();
    if (rc) { board_led_error(true); LOG_ERR("BLE startup failed: %d",rc); return rc; }
    LOG_INF("BLE initialized; starting independent AD5941 initialization");
    rc=afe_init();
    atomic_set(&boot_finished,1);
    int afe_rc=rc;
    if (rc) {
        board_led_error(true);
        LOG_ERR("AD5941 not ready: %d; APP diagnosis remains available",rc);
    } else { LOG_INF("AD5941 chip ID: 0x%04x",afe_chip_id()); }
    if (afe_rc) { ble_log(3,(uint16_t)(-afe_rc),"AD5941 initialization failed; check PCB power/SPI/reset"); }
    else { ble_log(1,1,"PCB ready: CV CA EIS DPV OCP on WE0; no STIM/multiplexer"); }
    LOG_INF("ECW APP protocol ready; CV/CA/EIS/DPV/OCP, WE0 only");
    return 0;
}
