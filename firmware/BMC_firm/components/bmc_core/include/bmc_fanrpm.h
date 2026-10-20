#pragma once

#include <stdbool.h>

/* RPM-matching fan layer. A control law produces one fan DEMAND in percent; it
 * means "percent of BMC_RPM_REF". Every fan is then driven to that same RPM:
 *     target_rpm = demand / 100 * ref_rpm
 *     duty = map(target_rpm) + PI(target_rpm - measured_rpm)
 * map() is a per-fan RPM->duty table (feedforward; measured with a duty sweep),
 * so the PI only trims. This lets fans with different maximum speeds spin together. */

#define FANRPM_MAP_N 11

typedef struct {
    float max_rpm;
    float kp, ki, i_limit;
    float map_rpm[FANRPM_MAP_N];    /* ascending */
    float map_duty[FANRPM_MAP_N];
    float integ;
} bmc_fanrpm_t;

/* Default map is linear (duty = rpm / max_rpm); replace it with the measured table. */
void bmc_fanrpm_init(bmc_fanrpm_t *f, float max_rpm);
void bmc_fanrpm_reset(bmc_fanrpm_t *f);
float bmc_fanrpm_target(float demand_pct, float ref_rpm);
float bmc_fanrpm_map_duty(const bmc_fanrpm_t *f, float rpm);
/* Returns duty in [0,100]. meas_valid=false (tach window not full yet) means feedforward only. */
float bmc_fanrpm_step(bmc_fanrpm_t *f, float target_rpm, float meas_rpm, bool meas_valid, float dt_s);
