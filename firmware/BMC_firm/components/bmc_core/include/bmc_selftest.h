#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bmc_config.h"

/* Initialization self-test (FSR 3.2.5.1.1). Runs in SELFTEST mode with the
 * heater forced off. It checks, in order: every required device answers, each
 * temperature reads a plausible ambient value, 48 V sense plausibility (when
 * enabled), the fan interlock, each fan on its own (it must turn, and the other
 * fan's tach must not follow it, which catches swapped tach wires), and that the
 * heater output really is off. Every failure is collected and named. */

typedef struct {
    uint32_t now_ms;
    bool plate_valid;
    float plate_c;
    bool ina_valid;
    float v48_v;
    bool inlet_fitted;
    bool inlet_valid;
    float inlet_c;
    bool fan_interlock_ok;
    struct { bool window_full; uint32_t rpm; } fan[BMC_FAN_COUNT];
    float heater_applied_pct;
} selftest_in_t;

typedef struct {
    bool done;
    bool pass;
    float fan_duty[BMC_FAN_COUNT];  /* open-loop duty the supervisor must apply while testing */
    char msg[176];                  /* valid when done: "PASS ..." or "FAIL source:reason; ..." */
} selftest_out_t;

typedef enum {
    ST_IDLE, ST_FAN1, ST_COAST, ST_FAN2, ST_DONE
} selftest_stage_t;

typedef struct {
    selftest_stage_t stage;
    uint32_t stage_ms;
    char fails[160];
    int nfails;
    bool fans_skipped;
} bmc_selftest_t;

void bmc_selftest_start(bmc_selftest_t *t, const selftest_in_t *in);
void bmc_selftest_step(bmc_selftest_t *t, const selftest_in_t *in, selftest_out_t *out);
