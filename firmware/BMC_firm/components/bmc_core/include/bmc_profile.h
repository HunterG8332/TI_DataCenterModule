#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bmc_config.h"

/* A load profile: a timeline of heater levels, run by the BMC itself so timing
 * does not depend on the network. Heat only ever comes from here (or the manual
 * HEATER lease in CHAR mode); the safety layer can cut it at any moment. */

typedef struct {
    uint32_t dur_s;
    float heater_pct;
} bmc_profile_step_t;

typedef struct {
    bmc_profile_step_t steps[BMC_PROFILE_MAX_STEPS];
    uint8_t n;
    bool running;
    uint32_t start_ms;
    uint32_t step_start_ms;
    uint8_t cur;
} bmc_profile_t;

void bmc_profile_clear(bmc_profile_t *p);
bool bmc_profile_add(bmc_profile_t *p, uint32_t dur_s, float heater_pct);
/* 1 = CONOPS scenario 4.1 (60 s idle, 600 s full, 300 s idle).
 * 2 = short smoke test (10 s idle, 30 s at 50%, 20 s idle).
 * 3 = repeatability sweep 0/25/50/75/100/0 %, 180 s each (PLACEHOLDER durations). */
bool bmc_profile_preset(bmc_profile_t *p, int id);
bool bmc_profile_start(bmc_profile_t *p, uint32_t now_ms);
void bmc_profile_stop(bmc_profile_t *p);
/* Advances the timeline. Returns false once the profile has ended or is not running.
 * On true, *heater_pct is the current level and *run_ms the run clock. */
bool bmc_profile_step(bmc_profile_t *p, uint32_t now_ms, float *heater_pct, uint32_t *run_ms);
uint32_t bmc_profile_total_s(const bmc_profile_t *p);
