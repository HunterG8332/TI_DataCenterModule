#include "bmc_app.h"

#include <string.h>
#include "bmc_config.h"
#include "bmc_i2c.h"
#include "bmc_telemetry.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ina238.h"
#include "tmp117.h"

static const char *TAG = "sensors";

#define TMP117_PLATE_ADDR   0x48     /* ADD0 -> GND; on the spreader plate */
#define INA238_ADDR         0x40     /* A1, A0 -> GND */
#define SENSOR_TASK_PRIORITY 8
#define SENSOR_TASK_CORE     1
#define BUS_RECOVER_PERIOD_MS 2000
#define FAILS_SATURATE       250

typedef struct {
    tmp117_t dev;
    sensor_dev_t st;
    bool tried;
    uint32_t last_try_ms;
    float value;
} tmp_ch_t;

typedef struct {
    ina238_t dev;
    sensor_dev_t st;
    bool tried;
    uint32_t last_try_ms;
    float v, i, p;
} ina_ch_t;

static sensor_snapshot_t s_snap;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void sensors_get(sensor_snapshot_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_snap;
    taskEXIT_CRITICAL(&s_lock);
}

static void note_fail(sensor_dev_t *d)
{
    d->valid = false;
    if (d->fails < FAILS_SATURATE) {
        d->fails++;
    }
}

static void tmp_poll(tmp_ch_t *c, uint8_t addr, uint32_t now_ms, const char *name)
{
    if (!c->st.present) {
        if (!c->tried || now_ms - c->last_try_ms >= BMC_SENSOR_REINIT_MS) {
            c->tried = true;
            c->last_try_ms = now_ms;
            if (tmp117_init(bmc_i2c_bus(), addr, &c->dev) == ESP_OK) {
                c->st.present = true;
                c->st.fails = 0;
                ESP_LOGI(TAG, "%s tmp117 found at 0x%02x", name, addr);
            }
        }
        if (!c->st.present) {
            c->st.valid = false;
            c->st.fails = 255;
            return;
        }
    }
    float t;
    if (tmp117_read_c(&c->dev, &t) == ESP_OK) {
        if (c->st.fails > 0) {
            tmp117_configure(&c->dev);      /* it may have been power-cycled: restore fast conversions */
            ESP_LOGI(TAG, "%s tmp117 reads again", name);
        }
        c->st.fails = 0;
        c->st.valid = true;
        c->value = t;
    } else {
        if (c->st.fails == 0) {
            ESP_LOGW(TAG, "%s tmp117 read failed", name);
        }
        note_fail(&c->st);
    }
}

static void ina_poll(ina_ch_t *c, uint32_t now_ms)
{
    if (!c->st.present) {
        if (!c->tried || now_ms - c->last_try_ms >= BMC_SENSOR_REINIT_MS) {
            c->tried = true;
            c->last_try_ms = now_ms;
            if (ina238_init(bmc_i2c_bus(), INA238_ADDR, &c->dev) == ESP_OK) {
                c->st.present = true;
                c->st.fails = 0;
                ESP_LOGI(TAG, "ina238 found at 0x%02x", INA238_ADDR);
            }
        }
        if (!c->st.present) {
            c->st.valid = false;
            c->st.fails = 255;
            return;
        }
    }
    if (ina238_read_all(&c->dev, &c->v, &c->i, &c->p) == ESP_OK) {
        if (c->st.fails > 0) {
            ina238_configure(&c->dev);
            ESP_LOGI(TAG, "ina238 reads again");
        }
        c->st.fails = 0;
        c->st.valid = true;
    } else {
        if (c->st.fails == 0) {
            ESP_LOGW(TAG, "ina238 read failed");
        }
        note_fail(&c->st);
    }
}

static void sensor_task(void *arg)
{
    esp_task_wdt_add(NULL);
    static tmp_ch_t plate = { .st = { .fails = 255 } };
    static tmp_ch_t inlet = { .st = { .fails = 255 } };
    static ina_ch_t ina = { .st = { .fails = 255 } };
    uint32_t last_recover_ms = 0;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        esp_task_wdt_reset();
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        tmp_poll(&plate, TMP117_PLATE_ADDR, now_ms, "plate");
#if CONFIG_BMC_INLET_SENSOR
        tmp_poll(&inlet, CONFIG_BMC_INLET_I2C_ADDR, now_ms, "inlet");
#endif
        ina_poll(&ina, now_ms);

        /* A failed transfer is the moment to look at the lines: stuck low = bus fault. */
        bool any_fail = !plate.st.valid || !ina.st.valid;
#if CONFIG_BMC_INLET_SENSOR
        any_fail = any_fail || !inlet.st.valid;
#endif
        bool stuck = any_fail && bmc_i2c_lines_stuck();
        bool bus_dead = plate.st.fails >= BMC_FAIL_READS_TRIP && ina.st.fails >= BMC_FAIL_READS_TRIP;
        if ((stuck || bus_dead) && now_ms - last_recover_ms >= BUS_RECOVER_PERIOD_MS) {
            last_recover_ms = now_ms;
            ESP_LOGW(TAG, "bus fault (stuck=%d): resetting the I2C controller", stuck);
            bmc_i2c_recover();
        }

        sensor_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        snap.plate = plate.st;
        snap.inlet = inlet.st;
        snap.ina = ina.st;
#if CONFIG_BMC_INLET_SENSOR
        snap.inlet_fitted = true;
#endif
        snap.plate_c = plate.value;
        snap.inlet_c = inlet.value;
        snap.v48_v = ina.v;
        snap.i48_a = ina.i;
        snap.power_w = ina.p;
        snap.i2c_stuck = stuck;
        snap.t_ms = now_ms ? now_ms : 1;
        taskENTER_CRITICAL(&s_lock);
        snap.seq = s_snap.seq + 1;
        s_snap = snap;
        taskEXIT_CRITICAL(&s_lock);

        /* One telemetry record per sample; the queue push never blocks. */
        app_status_t as;
        app_status_get(&as);
        telemetry_sample_t s;
        memset(&s, 0, sizeof(s));
        s.run_ms = as.st.run_ms;
        s.mode = as.st.mode;
        s.plate_c = snap.plate_c;
        s.plate_valid = snap.plate.valid;
        s.inlet_c = snap.inlet_c;
        s.inlet_valid = snap.inlet_fitted && snap.inlet.valid;
        s.v48_v = snap.v48_v;
        s.i48_a = snap.i48_a;
        s.power_w = snap.power_w;
        s.ina_valid = snap.ina.valid;
        s.rails_valid = false;                       /* VRM monitor not present yet */
        s.heater_cmd_pct = as.st.heater_cmd_pct;
        s.heater_pct = as.st.heater_pct;
        s.fan_demand_pct = as.st.fan_demand_pct;
        s.fan_target_rpm = as.st.fan_target_rpm;
        for (int i = 0; i < BMC_FAN_COUNT; i++) {
            s.fan_duty_pct[i] = as.fan_duty_pct[i];
            s.fan_rpm[i] = as.fan_rpm[i];
        }
        s.fan_enabled = as.fan_enabled;
        s.setpoint_c = as.st.setpoint_c;
        s.ctl_p = as.st.ctl_p;
        s.ctl_i = as.st.ctl_i;
        s.ctl_ff = as.st.ctl_ff;
        s.ff_variant = as.st.ff_variant;
        s.fault_code = as.st.fault_code;
        s.fault_mask = as.st.fault_mask;
        s.flags = as.st.flags;
        telemetry_publish(&s);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BMC_SAMPLE_PERIOD_MS));
    }
}

esp_err_t sensors_start(void)
{
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.plate.fails = s_snap.ina.fails = s_snap.inlet.fails = 255;
    BaseType_t ok = xTaskCreatePinnedToCore(sensor_task, "sensors", 6144, NULL, SENSOR_TASK_PRIORITY,
                                            NULL, SENSOR_TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
