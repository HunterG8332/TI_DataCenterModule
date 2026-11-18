#pragma once

#include <stdbool.h>
#include "bmc_config.h"

/* Pluggable feedforward for the proactive law. The form is deliberately not
 * fixed (docs/bmc_plan/04 section 5): variants are registered here, selected at
 * runtime with FF <id>, and tuned at runtime with FFPARAM, so 2-3 approaches can
 * be compared without reflashing. Input is the 48 V load current; output is a
 * fan demand offset in percent (>= 0).
 *
 * id 0 none      : always 0
 * id 1 linear    : p0 = gain [%/A], p1 = idle current [A]
 * id 2 table     : p0..p7 = current points [A, ascending], p8..p15 = demand [%]
 * id 3 lead-lag  : linear (p0, p1) followed by lead p2 [s] / lag p3 [s] shaping
 * All default values are PLACEHOLDERS until the plant is characterized. */

typedef struct {
    float z;
    bool init;
} bmc_ff_state_t;

int bmc_ff_count(void);
const char *bmc_ff_name(int id);
void bmc_ff_defaults(float params[BMC_FF_VARIANTS][BMC_FF_PARAMS]);
void bmc_ff_reset(bmc_ff_state_t *st);
float bmc_ff_compute(int id, const float *params, bmc_ff_state_t *st, float current_a, float dt_s);
