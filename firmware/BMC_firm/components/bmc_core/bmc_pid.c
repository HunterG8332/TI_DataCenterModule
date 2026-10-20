#include "bmc_pid.h"

#include <string.h>
#include "bmc_config.h"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void bmc_pid_init(bmc_pid_t *p, float kp, float ki, float kd)
{
    memset(p, 0, sizeof(*p));
    p->kp = kp;
    p->ki = ki;
    p->kd = kd;
    p->d_tau_s = BMC_PID_D_TAU_S;
    p->i_min = 0.0f;
    p->i_max = BMC_PID_I_MAX;
    p->out_min = 0.0f;
    p->out_max = 100.0f;
    p->slew_up = BMC_PID_SLEW_PCT_PER_S;
    p->slew_down = BMC_PID_SLEW_PCT_PER_S;
    p->first = true;
}

void bmc_pid_reset(bmc_pid_t *p)
{
    p->integ = 0.0f;
    p->d_filt = p->d_prev = 0.0f;
    p->out_prev = 0.0f;
    p->first = true;
    p->p_term = p->i_term = p->d_term = 0.0f;
}

float bmc_pid_step(bmc_pid_t *p, float meas, float setpoint, float dt_s)
{
    if (dt_s <= 0.0f) {
        return p->out_prev;
    }
    float err = meas - setpoint;

    float d = 0.0f;
    if (p->first) {
        p->d_filt = p->d_prev = meas;
        p->first = false;
    } else {
        float a = dt_s / (p->d_tau_s + dt_s);
        p->d_filt += a * (meas - p->d_filt);
        d = p->kd * (p->d_filt - p->d_prev) / dt_s;
        p->d_prev = p->d_filt;
    }

    float pterm = p->kp * err;
    float integ_try = clampf(p->integ + p->ki * err * dt_s, p->i_min, p->i_max);
    float u_try = pterm + integ_try + d + p->offset;
    if (!((u_try > p->out_max && err > 0.0f) || (u_try < p->out_min && err < 0.0f))) {
        p->integ = integ_try;                       /* not saturating against the error: integrate */
    }

    float u = clampf(pterm + p->integ + d + p->offset, p->out_min, p->out_max);
    if (p->slew_up > 0.0f && u > p->out_prev + p->slew_up * dt_s) {
        u = p->out_prev + p->slew_up * dt_s;
    }
    if (p->slew_down > 0.0f && u < p->out_prev - p->slew_down * dt_s) {
        u = p->out_prev - p->slew_down * dt_s;
    }
    p->out_prev = u;
    p->p_term = pterm;
    p->i_term = p->integ;
    p->d_term = d;
    return u;
}
