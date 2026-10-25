#include "bmc_app.h"

#include <string.h>
#include "bmc_board.h"
#include "bmc_cmd.h"
#include "bmc_config.h"
#include "bmc_fanrpm.h"
#include "bmc_sample.h"
#include "bmc_stall.h"
#include "bmc_stream.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "supervisor";

#define SUPERVISOR_PRIORITY 10          /* highest application priority */
#define SUPERVISOR_CORE     1
#define CMD_QUEUE_LEN       8
#define CMDS_PER_CYCLE      4

typedef struct {
    char line[65];
    uint8_t len;                        /* 65 means "longer than allowed" */
    bmc_peer_t from;
} cmd_msg_t;

static brain_t s_brain;
static QueueHandle_t s_cmd_q;
static bmc_stall_t s_stall[BMC_FAN_COUNT];
static bmc_fanrpm_t s_loop[BMC_FAN_COUNT];
static app_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

void app_status_get(app_status_t *out)
{
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
}

/* Called from the net task: copy and return, never block. */
static void on_command(const char *line, size_t len, bmc_peer_t from)
{
    cmd_msg_t m;
    memset(&m, 0, sizeof(m));
    size_t n = len > 64 ? 64 : len;
    memcpy(m.line, line, n);
    m.len = len > 64 ? 65 : (uint8_t)len;   /* an over-long line must be rejected, not truncated into a valid one */
    m.from = from;
    xQueueSend(s_cmd_q, &m, 0);
}

static void run_command(const cmd_msg_t *m, uint32_t now_ms)
{
    bmc_cmd_t cmd;
    if (!bmc_cmd_parse(m->line, m->len, &cmd)) {
        return;                                      /* empty line */
    }
    char reply[512];
    int n = brain_command(&s_brain, &cmd, now_ms, reply, sizeof(reply));
    if (n > 0) {
        bmc_stream_reply(m->from, reply, (size_t)n);
    }
}

static bmc_fault_t fault_from_reset(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
        return BMC_FAULT_RESET_WDT;
    case ESP_RST_BROWNOUT:
        return BMC_FAULT_RESET_BROWNOUT;
    case ESP_RST_PANIC:
        return BMC_FAULT_RESET_PANIC;
    default:
        return BMC_FAULT_NONE;
    }
}

static void drain_events(void)
{
    bmc_event_t e;
    while (bmc_ev_pop(&s_brain.ev, &e)) {
        char line[300];
        int n = bmc_fmt_event(line, sizeof(line), &e);
        if (n > 0) {
            bmc_stream_post_event(line, (size_t)n);
        }
        /* Logged after the outputs are already safe. */
        if (e.type == BMC_EV_FAILSAFE_ENTER) {
            ESP_LOGE(TAG, "FAILSAFE %s: %s", e.code, e.detail);
        } else {
            ESP_LOGI(TAG, "%s %s: %s", bmc_ev_type_name(e.type), e.code, e.detail);
        }
    }
}

static void supervisor_task(void *arg)
{
    esp_task_wdt_add(NULL);
    float duty[BMC_FAN_COUNT] = { 0 };
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        esp_task_wdt_reset();
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* Commands first, so a request is in force on the very next outputs. */
        cmd_msg_t m;
        for (int budget = CMDS_PER_CYCLE; budget > 0 && xQueueReceive(s_cmd_q, &m, 0) == pdTRUE; budget--) {
            run_command(&m, now_ms);
        }

        sensor_snapshot_t sn;
        sensors_get(&sn);
        bool interlock = bmc_fan_is_enabled(act_fan(0));

        brain_in_t in;
        memset(&in, 0, sizeof(in));
        in.now_ms = now_ms;
        in.sample_seq = sn.seq;
        in.safety.plate_valid = sn.plate.valid;
        in.safety.plate_c = sn.plate_c;
        in.safety.plate_fails = sn.plate.fails;
        in.safety.ina_valid = sn.ina.valid;
        in.safety.ina_fails = sn.ina.fails;
        in.safety.i2c_stuck = sn.i2c_stuck;
        in.safety.sensor_age_ms = now_ms - sn.t_ms;
        in.safety.fan_interlock_ok = interlock;
        in.safety.heater_permit = board_run_jumper_closed();
        in.inlet_fitted = sn.inlet_fitted;
        in.inlet_valid = sn.inlet.valid;
        in.inlet_c = sn.inlet_c;
        in.v48_v = sn.v48_v;
        in.i48_a = sn.i48_a;
        in.power_w = sn.power_w;

        uint32_t rpm[BMC_FAN_COUNT];
        bool full[BMC_FAN_COUNT];
        for (int i = 0; i < BMC_FAN_COUNT; i++) {
            if (bmc_fan_read_rpm(act_fan(i), BMC_FAN_TACH_WINDOW_MS, &rpm[i], &full[i]) != ESP_OK) {
                rpm[i] = 0;
                full[i] = false;
            }
            bmc_stall_update(&s_stall[i], now_ms, interlock, duty[i], rpm[i], full[i]);
            in.safety.fan_stalled[i] = s_stall[i].stalled;
            in.fan[i].rpm = rpm[i];
            in.fan[i].window_full = full[i];
            in.fan_duty_applied[i] = duty[i];
        }

        brain_out_t out;
        brain_step(&s_brain, &in, &out);

        /* Fan layer: failsafe and self-test drive the fans open-loop; otherwise both
         * fans are RPM-matched to the demanded percentage of BMC_RPM_REF. */
        act_cmd_t ac;
        memset(&ac, 0, sizeof(ac));
        ac.heater_pct = out.heater_pct;
        ac.heater_enable = out.heater_enable;
        const float dt = BMC_SAFETY_PERIOD_MS / 1000.0f;
        const float target = bmc_fanrpm_target(out.fan_demand_pct, BMC_RPM_REF);
        for (int i = 0; i < BMC_FAN_COUNT; i++) {
            if (out.fan_full) {
                ac.fan_duty[i] = BMC_FAILSAFE_FAN_DUTY_PCT;
                bmc_fanrpm_reset(&s_loop[i]);
            } else if (out.fan_selftest) {
                ac.fan_duty[i] = out.fan_test_duty[i];
                bmc_fanrpm_reset(&s_loop[i]);
            } else {
                ac.fan_duty[i] = bmc_fanrpm_step(&s_loop[i], target, (float)rpm[i], full[i], dt);
            }
        }
        act_apply(&ac);
        for (int i = 0; i < BMC_FAN_COUNT; i++) {
            duty[i] = interlock ? ac.fan_duty[i] : 0.0f;
        }

        taskENTER_CRITICAL(&s_status_lock);
        s_status.st = out.st;
        s_status.fan_enabled = interlock;
        for (int i = 0; i < BMC_FAN_COUNT; i++) {
            s_status.fan_duty_pct[i] = duty[i];
            s_status.fan_rpm[i] = rpm[i];
        }
        taskEXIT_CRITICAL(&s_status_lock);

        drain_events();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BMC_SAFETY_PERIOD_MS));
    }
}

esp_err_t supervisor_start(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
#ifdef CONFIG_BMC_TEST_HOOKS
    const bool hooks = true;
#else
    const bool hooks = false;
#endif
    brain_init(&s_brain, now_ms, hooks);
    bmc_fault_t reason = fault_from_reset(esp_reset_reason());
    if (reason != BMC_FAULT_NONE) {
        brain_note_reset(&s_brain, reason);
    }
    float max_rpm[BMC_FAN_COUNT] = { BMC_FAN1_MAX_RPM, BMC_FAN2_MAX_RPM };
    for (int i = 0; i < BMC_FAN_COUNT; i++) {
        bmc_stall_init(&s_stall[i]);
        bmc_fanrpm_init(&s_loop[i], max_rpm[i]);
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.st.run_ms = -1;

    s_cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_msg_t));
    if (!s_cmd_q) {
        return ESP_ERR_NO_MEM;
    }
    bmc_stream_set_command_handler(on_command);
    if (hooks) {
        ESP_LOGW(TAG, "test hooks are ENABLED (TESTSET / TESTINJECT); disable CONFIG_BMC_TEST_HOOKS for the demo build");
    }
    BaseType_t ok = xTaskCreatePinnedToCore(supervisor_task, "supervisor", 8192, NULL, SUPERVISOR_PRIORITY,
                                            NULL, SUPERVISOR_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
