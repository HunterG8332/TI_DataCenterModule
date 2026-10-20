#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bmc_brain.h"
#include "bmc_fan.h"
#include "esp_err.h"

/* The hardware-facing side of the BMC. The decisions all live in bmc_core
 * (bmc_brain); these files only move data between the hardware and the brain:
 *
 *   sensors.c     reads the I2C devices, counts consecutive failures, builds telemetry
 *   supervisor.c  50 ms task: tach -> brain_step -> fan loops -> actuators; runs commands
 *   actuators.c   the ONE place that writes heater and fan outputs
 *   bmc_aux.c     OLED and the optional fan-link peer (slow I2C kept off the control path)
 */

/* ---- sensors ---------------------------------------------------------------- */
typedef struct {
    bool present;       /* device answered an init at least once */
    uint8_t fails;      /* consecutive failed reads; 255 until it has ever answered */
    bool valid;         /* the latest read succeeded */
} sensor_dev_t;

typedef struct {
    uint32_t seq;       /* increments once per 10 Hz sample */
    uint32_t t_ms;      /* when this snapshot was published (0 before the first one) */
    sensor_dev_t plate, inlet, ina;
    bool inlet_fitted;
    float plate_c, inlet_c, v48_v, i48_a, power_w;
    bool i2c_stuck;     /* SDA or SCL held low after a failed transfer */
} sensor_snapshot_t;

esp_err_t sensors_start(void);
void sensors_get(sensor_snapshot_t *out);

/* ---- supervisor ------------------------------------------------------------- */
typedef struct {
    brain_status_t st;
    float fan_duty_pct[BMC_FAN_COUNT];
    uint32_t fan_rpm[BMC_FAN_COUNT];
    bool fan_enabled;
} app_status_t;

esp_err_t supervisor_start(void);
void app_status_get(app_status_t *out);

/* ---- actuators (single writer) ----------------------------------------------- */
typedef struct {
    float heater_pct;
    bool heater_enable;
    float fan_duty[BMC_FAN_COUNT];
} act_cmd_t;

/* Brings the heater to a known OFF state and sets up both fans. Call first. */
esp_err_t act_init(void);
bmc_fan_t *act_fan(int index);
/* Only the task that first calls this may call it again; any other caller is
 * refused and logged, so a second writer cannot appear by accident. */
void act_apply(const act_cmd_t *cmd);

/* ---- aux ---------------------------------------------------------------------- */
esp_err_t aux_start(void);
