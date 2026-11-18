#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "method/inc/ec_method.h"
#include "method/inc/ec_params.h"

LOG_MODULE_REGISTER(ec_test, LOG_LEVEL_INF);

#define DPV_BOOT_RESTART_GAP_MS 200u
#define DPV_BOOT_IDLE_POLL_MS   20u

static ec_dpv_params_t s_dpv = {
    .we_index = 0,
    .rtia_gear = 0,
    .e_begin_mV = -200,
    .e_end_mV = 800,
    .step_mV = 100,
    .pulse_amp_mV = 200,
    .pulse_width_ms = 40,
    .period_ms = 80,
    .quiet_time_ms = 200,
};

static void dpv_boot_test_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(s_test_work, dpv_boot_test_handler);

typedef enum {
    DPV_BOOT_ACTION_START = 0,
    DPV_BOOT_ACTION_WAIT_IDLE,
} dpv_boot_action_t;

static dpv_boot_action_t s_next_action = DPV_BOOT_ACTION_START;
static uint32_t s_run_count;

static void dpv_boot_test_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (s_next_action == DPV_BOOT_ACTION_WAIT_IDLE) {
        if (ec_method_state() != EC_STATE_IDLE) {
            k_work_reschedule(&s_test_work, K_MSEC(DPV_BOOT_IDLE_POLL_MS));
            return;
        }

        s_next_action = DPV_BOOT_ACTION_START;
        k_work_reschedule(&s_test_work, K_MSEC(DPV_BOOT_RESTART_GAP_MS));
        return;
    }

    int err = ec_method_start(EC_METHOD_DPV, 0u,
                              (const uint8_t *)&s_dpv, sizeof(s_dpv));
    if (err) {
        LOG_ERR("[TEST] start DPV failed: %d", err);
        k_work_reschedule(&s_test_work, K_MSEC(DPV_BOOT_RESTART_GAP_MS));
        return;
    }

    s_run_count++;
    s_next_action = DPV_BOOT_ACTION_WAIT_IDLE;
    k_work_reschedule(&s_test_work, K_MSEC(DPV_BOOT_IDLE_POLL_MS));

    LOG_INF("[TEST] DPV boot test #%u started: WE%u %d..%d mV step=%u pulse=%u mV/%u ms period=%u ms",
            (unsigned)s_run_count,
            s_dpv.we_index, s_dpv.e_begin_mV, s_dpv.e_end_mV,
            s_dpv.step_mV, s_dpv.pulse_amp_mV,
            s_dpv.pulse_width_ms, s_dpv.period_ms);
}

int ec_test_init(void)
{
    int err = k_work_reschedule(&s_test_work, K_SECONDS(3));
    if (err < 0) {
        LOG_ERR("Failed to schedule DPV boot test: %d", err);
        return err;
    }

    LOG_INF("DPV boot test loop armed (start in 3 s, restart gap %u ms)",
            DPV_BOOT_RESTART_GAP_MS);
    return 0;
}
