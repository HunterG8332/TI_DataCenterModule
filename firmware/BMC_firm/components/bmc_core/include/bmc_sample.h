#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "bmc_events.h"
#include "bmc_types.h"

/* One telemetry record (docs/bmc_plan/03 section 1) and its text encodings.
 * "Not valid" is carried as an empty CSV field, never as a fake zero. */

typedef struct {
    uint32_t seq;           /* stamped by telemetry_publish(); advances even when the sample is dropped */
    int64_t t_us;           /* microseconds since boot, stamped by telemetry_publish() */
    int64_t run_ms;         /* milliseconds since PROFILE START, < 0 outside a run */
    bmc_mode_t mode;
    float plate_c;          bool plate_valid;
    float inlet_c;          bool inlet_valid;
    float v48_v, i48_a, power_w;  bool ina_valid;
    float rail_v[3];        bool rails_valid;       /* 3.3 V, 5 V, 12 V from the VRM monitor (reserved) */
    float heater_cmd_pct;   /* commanded by profile / manual request */
    float heater_pct;       /* applied after arbitration (0 when inhibited) */
    float fan_demand_pct;   /* control law output: percent of BMC_RPM_REF */
    uint32_t fan_target_rpm;
    float fan_duty_pct[2];
    uint32_t fan_rpm[2];
    bool fan_enabled;       /* fan interlock closed */
    float setpoint_c, ctl_p, ctl_i, ctl_ff;
    int ff_variant;
    uint32_t fault_code, fault_mask, flags;
} telemetry_sample_t;

#define BMC_FMT_COLUMNS 31
#define BMC_FMT_RECORD_MAX 360

/* All return the length written (excluding NUL) or -1 if the buffer was too small. */
int bmc_fmt_header(char *buf, size_t n);
int bmc_fmt_record(char *buf, size_t n, const char *bmc_id, const telemetry_sample_t *s, uint32_t drops);
/* EVENT,<id>,<t_us>,<run_ms>,<type>,<code>,<detail> */
int bmc_fmt_event(char *buf, size_t n, const bmc_event_t *e);
