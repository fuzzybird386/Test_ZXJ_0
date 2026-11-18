/*
 * Common AD5941 bus / AFE bring-up shared by every EC-method proxy.
 *
 * Port/SPI/GPIO setup is cached, but the AFE itself is reset before every
 * experiment so stale AD5941 state cannot leak between methods.
 */

#include "ec_proxy.h"
#include "../../inc/ble_aabb.h"
#include "../../ad5941_port/ad5941_port.h"
#include "../../../lib/ad5940/ad5940.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ec_proxy_bus, LOG_LEVEL_INF);

static bool s_port_ready;

static void ec_proxy_bus_route_interrupts(void)
{
    /*
     * Methods poll AD5940_INTCTestFlag(...). Low-bandwidth methods use
     * SINC2RDY; EIS uses DFTRDY. Route both to INTC0 to match proxy code, and
     * keep INTC1 enabled for compatibility with ADI calibration/example flows.
     */
    AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_SINC2RDY | AFEINTSRC_DFTRDY, bTRUE);
    AD5940_INTCCfg(AFEINTC_1, AFEINTSRC_SINC2RDY | AFEINTSRC_DFTRDY, bTRUE);
    AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY | AFEINTSRC_DFTRDY);
}

static int ec_proxy_bus_check_id(const char *tag)
{
    uint32_t adiid = AD5940_ReadReg(REG_AFECON_ADIID);
    LOG_INF("%s AD5941 ADIID = 0x%04x", tag ? tag : "EC", (unsigned)adiid);
    if ((adiid & 0xFFFF) != 0x4144) {
        LOG_ERR("AD5941 not detected (ADIID=0x%04x)", (unsigned)adiid);
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3100u,
                             "AD5941 not detected: ADIID=0x%04x",
                             (unsigned)adiid);
        return -ENODEV;
    }
    return 0;
}

int ec_proxy_bus_ready(void)
{
    if (s_port_ready) {
        return 0;
    }

    int err = ad5941_port_init();
    if (err) {
        LOG_ERR("ad5941_port_init failed: %d", err);
        return err;
    }

    /* Initial library bring-up so the ID probe can use the register interface. */
    AD5940_Initialize();
    err = ec_proxy_bus_check_id("Port");
    if (err) return err;

    s_port_ready = true;
    return 0;
}

int ec_proxy_bus_prepare_experiment(const char *tag)
{
    ad5941_port_clear_error();
    int err = ec_proxy_bus_ready();
    if (err) return err;

    const char *name = tag ? tag : "EC";
    LOG_INF("%s: AD5941 hard reset + soft reset", name);

    AD5940_HWReset();
    AD5940_Initialize();
    AD5940Err rst = AD5940_SoftRst();
    if (rst != AD5940ERR_OK) {
        LOG_ERR("%s: AD5941 soft reset failed: %d", name, (int)rst);
        (void)ble_log_eventf(BLE_LOG_LEVEL_ERROR, 0x3101u,
                             "%s AD5941 soft reset failed (%d)",
                             name, (int)rst);
        return -EIO;
    }
    AD5940_Initialize();

    err = ec_proxy_bus_check_id(name);
    if (err) return err;

    ec_proxy_bus_route_interrupts();
    return 0;
}
