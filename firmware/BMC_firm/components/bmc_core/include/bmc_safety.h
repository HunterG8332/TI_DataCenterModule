#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bmc_config.h"
#include "bmc_events.h"
#include "bmc_types.h"

/* The mode state machine and fault logic. Pure logic: no hardware, no RTOS, no
 * clocks of its own (time comes in with every step), so every path can be
 * exercised on a PC. The supervisor task calls safety_step() every
 * BMC_SAFETY_PERIOD_MS with a fresh snapshot of the world.
 *
 * Rules (docs/bmc_plan/02):
 *  - Over-temperature trips in every mode. The other faults trip only in an
 *    actuating mode (CHAR / REACTIVE / PROACTIVE); in STANDBY and SELFTEST there
 *    is no heat to remove, and the faults must stay observable so the self-test
 *    can name them.
 *  - FAILSAFE latches. It is left only by safety_ack(), and only when the plate
 *    has been below BMC_SAFE_CLEAR_C for BMC_SAFE_CLEAR_HOLD_MS, the plate
 *    sensor is readable, and no fault condition is still active. */

typedef struct {
    uint32_t now_ms;
    bool plate_valid;
    float plate_c;
    uint8_t plate_fails;            /* consecutive failed reads (255 = never present) */
    bool ina_valid;
    uint8_t ina_fails;
    bool i2c_stuck;                 /* SDA or SCL held low */
    uint32_t sensor_age_ms;         /* age of the newest sensor snapshot */
    bool fan_stalled[BMC_FAN_COUNT];
    bool fan_interlock_ok;          /* fan enable pin closed */
    bool heater_permit;             /* hardware permit pin closed */
} safety_in_t;

typedef struct {
    bmc_mode_t mode;
    bool force_safe;                /* failsafe: heater off, fans full */
    bool actuating;
    bool heater_allowed;            /* mode permits heat (further gated by arbitration) */
    bool safe_to_handle;
    bool selftest_ok;
    bool ot_warn;
    bmc_fault_t fault_code;         /* latched first cause, NONE when not in failsafe */
    uint32_t fault_mask;            /* conditions active right now */
    float ot_limit_c;
} safety_out_t;

typedef struct {
    bmc_mode_t mode;
    bmc_fault_t fault_code;
    uint32_t latched_mask;
    uint32_t active_mask;
    bool selftest_ok;
    bool safe_to_handle;
    bool safe_timing;
    uint32_t safe_since_ms;
    float ot_limit_c;
    uint32_t inject_mask;           /* test hooks: forced fault conditions */
    bool last_plate_valid;
    float last_plate_c;
    uint32_t now_ms;
    bool ot_warn_seen;
    bmc_events_t *ev;
    safety_in_t last_in;
} safety_t;

void safety_init(safety_t *s, bmc_events_t *ev, uint32_t now_ms);
void safety_step(safety_t *s, const safety_in_t *in, safety_out_t *out);

/* Requests. Each returns NULL on success or a short reason token on refusal
 * ("latched", "busy", "no_selftest", "bad_mode", "bad_state", "not_safe",
 * "fault_active", "heater_on", "profile_running"). Refusals change nothing. */
const char *safety_request_mode(safety_t *s, bmc_mode_t target, bool profile_running, bool heater_zero);
const char *safety_begin_selftest(safety_t *s);
void safety_selftest_done(safety_t *s, bool pass);
const char *safety_ack(safety_t *s);

/* Test hooks (only reachable when the build enables them; see bmc_brain). */
void safety_set_ot_limit(safety_t *s, float limit_c);   /* can only lower, never exceed BMC_OT_TRIP_C */
void safety_inject(safety_t *s, bmc_fault_t fault, bool on);
void safety_clear_injections(safety_t *s);

void safety_note_reset(safety_t *s, bmc_fault_t reason);  /* RESET_* event, no latch */
