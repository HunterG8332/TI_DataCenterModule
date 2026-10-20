#pragma once

#include <stdbool.h>

/* PID in the OpenBMC phosphor-pid-control shape: P, I and D terms plus a
 * feed-forward offset, integral clamp, output clamp and slew limits.
 * Direct action: error = measurement - setpoint, so a plate hotter than the
 * setpoint raises the output. D acts on a filtered measurement (no kick on a
 * setpoint change). The integrator stops growing while the output is saturated
 * in the direction of the error (conditional anti-windup).
 *
 * The same structure is the reactive law and the PI trim of the proactive law
 * (offset = feedforward term), so both are tuned by the same method. */

typedef struct {
    float kp, ki, kd;
    float d_tau_s;                  /* derivative filter time constant */
    float i_min, i_max;
    float out_min, out_max;
    float slew_up, slew_down;       /* output units per second; <= 0 disables */
    float offset;                   /* added before saturation (feedforward) */
    /* state */
    float integ;
    float d_filt, d_prev;
    float out_prev;
    bool first;
    /* last terms, for telemetry */
    float p_term, i_term, d_term;
} bmc_pid_t;

void bmc_pid_init(bmc_pid_t *p, float kp, float ki, float kd);
void bmc_pid_reset(bmc_pid_t *p);
float bmc_pid_step(bmc_pid_t *p, float meas, float setpoint, float dt_s);
