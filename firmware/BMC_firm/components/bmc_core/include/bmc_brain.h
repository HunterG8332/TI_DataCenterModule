#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bmc_cmd.h"
#include "bmc_config.h"
#include "bmc_events.h"
#include "bmc_ff.h"
#include "bmc_pid.h"
#include "bmc_profile.h"
#include "bmc_safety.h"
#include "bmc_selftest.h"
#include "bmc_types.h"

/* The brain: everything the BMC decides, with no hardware or RTOS in it. The
 * supervisor task feeds it the world (brain_in_t) every BMC_SAFETY_PERIOD_MS and
 * applies what comes back (brain_out_t). Commands arrive through brain_command().
 *
 * Authority: the control law and the profile only PROPOSE a fan demand and a
 * heater level. Arbitration at the end of brain_step() applies the safety
 * verdict last, so no proposal can override a failsafe, the permit pin, the fan
 * interlock or an invalid temperature reading. */

typedef struct {
    uint32_t now_ms;
    uint32_t sample_seq;            /* changes when a fresh 10 Hz sensor sample is available */
    safety_in_t safety;             /* sensor health, plate temperature, stall flags, pins */
    bool plate_present;             /* TMP117 on the plate answered at least once since boot */
    bool inlet_fitted;
    bool inlet_valid;
    float inlet_c;
    float v48_v, i48_a, power_w;
    struct {
        bool window_full;
        uint32_t rpm;
    } fan[BMC_FAN_COUNT];
    float fan_duty_applied[BMC_FAN_COUNT];
} brain_in_t;

typedef struct {
    bmc_mode_t mode;
    uint32_t fault_code, fault_mask, flags;
    bool selftest_ok, safe_to_handle, profile_running, ot_warn;
    int64_t run_ms;                 /* < 0 outside a run */
    float heater_cmd_pct;           /* what the profile / manual request asked for */
    float heater_pct;               /* what the arbiter allowed */
    float fan_demand_pct;
    uint32_t fan_target_rpm;
    float setpoint_c, ctl_p, ctl_i, ctl_ff;
    int ff_variant;
    float ot_limit_c;
} brain_status_t;

typedef struct {
    float heater_pct;
    bool heater_enable;
    bool fan_full;                  /* failsafe: both fans open-loop at BMC_FAILSAFE_FAN_DUTY_PCT */
    bool fan_selftest;              /* self-test drives the fans open-loop */
    float fan_test_duty[BMC_FAN_COUNT];
    float fan_demand_pct;           /* otherwise: RPM-matched at this demand */
    brain_status_t st;
} brain_out_t;

typedef struct {
    bmc_events_t ev;
    safety_t safety;
    bmc_profile_t profile;
    bmc_selftest_t selftest;
    bmc_pid_t pid;
    bmc_ff_state_t ff;
    float ff_params[BMC_FF_VARIANTS][BMC_FF_PARAMS];
    int ff_variant;
    float setpoint_c;
    float rpm_ref;
    bool test_hooks;

    bool manual_heater_on;
    float manual_heater_pct;
    uint32_t manual_heater_ms;
    bool manual_fan_auto;
    float manual_fan_pct;

    float fan_demand;
    float heater_cmd;
    float heater_out;
    uint32_t last_sample_seq;
    uint32_t last_ctl_ms;
    bool ctl_started;
    bool run_active;
    uint32_t run_start_ms;
    brain_in_t last_in;
    brain_status_t st;
} brain_t;

void brain_init(brain_t *b, uint32_t now_ms, bool test_hooks);
void brain_step(brain_t *b, const brain_in_t *in, brain_out_t *out);
/* Executes one parsed command and writes the reply text ("OK ..." / "ERR ..."),
 * which may hold several lines (EVENTS). Returns the reply length, or 0 when the
 * command is accepted silently (the 250 ms HEATER keep-alive). */
int brain_command(brain_t *b, const bmc_cmd_t *cmd, uint32_t now_ms, char *reply, size_t reply_n);
void brain_note_reset(brain_t *b, bmc_fault_t reason);
