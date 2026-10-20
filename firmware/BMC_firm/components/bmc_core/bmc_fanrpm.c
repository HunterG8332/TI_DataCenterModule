#include "bmc_fanrpm.h"

#include "bmc_config.h"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void bmc_fanrpm_init(bmc_fanrpm_t *f, float max_rpm)
{
    f->max_rpm = max_rpm;
    f->kp = BMC_FANRPM_KP;
    f->ki = BMC_FANRPM_KI;
    f->i_limit = BMC_FANRPM_I_LIMIT;
    f->integ = 0.0f;
    for (int i = 0; i < FANRPM_MAP_N; i++) {
        f->map_rpm[i] = max_rpm * (float)i / (float)(FANRPM_MAP_N - 1);
        f->map_duty[i] = 100.0f * (float)i / (float)(FANRPM_MAP_N - 1);
    }
}

void bmc_fanrpm_reset(bmc_fanrpm_t *f)
{
    f->integ = 0.0f;
}

float bmc_fanrpm_target(float demand_pct, float ref_rpm)
{
    return clampf(demand_pct, 0.0f, 100.0f) / 100.0f * ref_rpm;
}

float bmc_fanrpm_map_duty(const bmc_fanrpm_t *f, float rpm)
{
    if (rpm <= f->map_rpm[0]) {
        return f->map_duty[0];
    }
    for (int i = 1; i < FANRPM_MAP_N; i++) {
        if (rpm <= f->map_rpm[i]) {
            float span = f->map_rpm[i] - f->map_rpm[i - 1];
            if (span <= 0.0f) {
                return f->map_duty[i];
            }
            float t = (rpm - f->map_rpm[i - 1]) / span;
            return f->map_duty[i - 1] + t * (f->map_duty[i] - f->map_duty[i - 1]);
        }
    }
    return f->map_duty[FANRPM_MAP_N - 1];
}

float bmc_fanrpm_step(bmc_fanrpm_t *f, float target_rpm, float meas_rpm, bool meas_valid, float dt_s)
{
    if (target_rpm <= 0.0f) {
        f->integ = 0.0f;
        return 0.0f;
    }
    float ff = bmc_fanrpm_map_duty(f, target_rpm);
    if (!meas_valid) {
        return clampf(ff + f->integ, 0.0f, 100.0f);
    }
    float err = target_rpm - meas_rpm;
    float trim = f->kp * err;
    float integ_try = clampf(f->integ + f->ki * err * dt_s, -f->i_limit, f->i_limit);
    float u_try = ff + trim + integ_try;
    if (!((u_try > 100.0f && err > 0.0f) || (u_try < 0.0f && err < 0.0f))) {
        f->integ = integ_try;                       /* conditional anti-windup */
    }
    return clampf(ff + trim + f->integ, 0.0f, 100.0f);
}
