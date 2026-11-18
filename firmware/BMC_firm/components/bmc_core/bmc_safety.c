#include "bmc_safety.h"

#include <stdio.h>
#include <string.h>

/* Order used to pick the single reported cause when several conditions are
 * active at once. A bus fault outranks the device losses it causes. */
static const bmc_fault_t CAUSE_ORDER[] = {
    BMC_FAULT_OVER_TEMP, BMC_FAULT_I2C, BMC_FAULT_TEMP_LOSS,
    BMC_FAULT_TELEM_LOSS, BMC_FAULT_FAN_STALL, BMC_FAULT_SENSOR_STALE,
};

static bmc_fault_t pick_cause(uint32_t mask)
{
    for (unsigned i = 0; i < sizeof(CAUSE_ORDER) / sizeof(CAUSE_ORDER[0]); i++) {
        if (mask & BMC_FAULT_BIT(CAUSE_ORDER[i])) {
            return CAUSE_ORDER[i];
        }
    }
    return BMC_FAULT_NONE;
}

static void set_mode(safety_t *s, bmc_mode_t mode, const char *why)
{
    if (s->mode != mode) {
        bmc_ev_emit(s->ev, BMC_EV_MODE_CHANGE, s->now_ms, -1, bmc_mode_name(mode),
                    "from=%s why=%s", bmc_mode_name(s->mode), why);
        s->mode = mode;
    }
}

void safety_init(safety_t *s, bmc_events_t *ev, uint32_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->mode = BMC_MODE_STANDBY;
    s->ot_limit_c = BMC_OT_TRIP_C;
    s->ev = ev;
    s->now_ms = now_ms;
}

static uint32_t active_conditions(const safety_t *s, const safety_in_t *in)
{
    uint32_t m = 0;
    bool range_bad = in->plate_valid &&
                     (in->plate_c < BMC_TEMP_VALID_MIN_C || in->plate_c > BMC_TEMP_VALID_MAX_C);
    if (in->plate_valid && in->plate_c >= s->ot_limit_c) {
        m |= BMC_FAULT_BIT(BMC_FAULT_OVER_TEMP);
    }
    if (in->i2c_stuck || (in->plate_fails >= BMC_FAIL_READS_TRIP && in->ina_fails >= BMC_FAIL_READS_TRIP)) {
        m |= BMC_FAULT_BIT(BMC_FAULT_I2C);
    }
    if (in->plate_fails >= BMC_FAIL_READS_TRIP || range_bad) {
        m |= BMC_FAULT_BIT(BMC_FAULT_TEMP_LOSS);
    }
    if (in->ina_fails >= BMC_FAIL_READS_TRIP) {
        m |= BMC_FAULT_BIT(BMC_FAULT_TELEM_LOSS);
    }
    for (int i = 0; i < BMC_FAN_COUNT; i++) {
        if (in->fan_stalled[i]) {
            m |= BMC_FAULT_BIT(BMC_FAULT_FAN_STALL);
        }
    }
    if (in->sensor_age_ms > BMC_SENSOR_STALE_MS) {
        m |= BMC_FAULT_BIT(BMC_FAULT_SENSOR_STALE);
    }
    return m | s->inject_mask;
}

static void enter_failsafe(safety_t *s, const safety_in_t *in, uint32_t mask, bmc_fault_t cause)
{
    char plate[16];
    if (in->plate_valid) {
        snprintf(plate, sizeof(plate), "%.1f", (double)in->plate_c);
    } else {
        snprintf(plate, sizeof(plate), "na");
    }
    bmc_ev_emit(s->ev, BMC_EV_FAILSAFE_ENTER, s->now_ms, -1, bmc_fault_name(cause),
                "mode=%s plate_c=%s limit=%.1f plate_fails=%u ina_fails=%u stuck=%d stall1=%d stall2=%d age_ms=%u",
                bmc_mode_name(s->mode), plate, (double)s->ot_limit_c, (unsigned)in->plate_fails,
                (unsigned)in->ina_fails, in->i2c_stuck ? 1 : 0, in->fan_stalled[0] ? 1 : 0,
                in->fan_stalled[1] ? 1 : 0, (unsigned)in->sensor_age_ms);
    set_mode(s, BMC_MODE_FAILSAFE, bmc_fault_name(cause));
    s->fault_code = cause;
    s->latched_mask = mask;
    s->selftest_ok = false;
    s->safe_to_handle = false;
    s->safe_timing = false;
}

void safety_step(safety_t *s, const safety_in_t *in, safety_out_t *out)
{
    s->now_ms = in->now_ms;
    s->last_in = *in;
    s->last_plate_valid = in->plate_valid;
    s->last_plate_c = in->plate_c;

    uint32_t active = active_conditions(s, in);
    s->active_mask = active;

    if (s->mode != BMC_MODE_FAILSAFE) {
        uint32_t armed = BMC_FAULT_BIT(BMC_FAULT_OVER_TEMP);
        if (bmc_mode_is_actuating(s->mode)) {
            armed = ~0u;
        }
        uint32_t trip = active & armed;
        if (trip) {
            enter_failsafe(s, in, active, pick_cause(trip));
        }
    } else {
        s->latched_mask |= active;
        bool cool = in->plate_valid && in->plate_c < BMC_SAFE_CLEAR_C;
        if (cool) {
            if (!s->safe_timing) {
                s->safe_timing = true;
                s->safe_since_ms = in->now_ms;
            }
            if (!s->safe_to_handle && (uint32_t)(in->now_ms - s->safe_since_ms) >= BMC_SAFE_CLEAR_HOLD_MS) {
                s->safe_to_handle = true;
                bmc_ev_emit(s->ev, BMC_EV_SAFE_TO_HANDLE, in->now_ms, -1, "SAFE",
                            "plate_c=%.1f below=%.1f", (double)in->plate_c, (double)BMC_SAFE_CLEAR_C);
            }
        } else {
            s->safe_timing = false;
            s->safe_to_handle = false;     /* it warmed up again: not safe any more */
        }
    }

    bool ot_warn = in->plate_valid && in->plate_c >= BMC_OT_WARN_C;
    if (ot_warn && !s->ot_warn_seen) {
        bmc_ev_emit(s->ev, BMC_EV_WARN, in->now_ms, -1, "OT_WARN", "plate_c=%.1f warn=%.1f",
                    (double)in->plate_c, (double)BMC_OT_WARN_C);
    }
    s->ot_warn_seen = ot_warn;

    out->mode = s->mode;
    out->force_safe = (s->mode == BMC_MODE_FAILSAFE);
    out->actuating = bmc_mode_is_actuating(s->mode);
    out->heater_allowed = out->actuating;
    out->safe_to_handle = s->safe_to_handle;
    out->selftest_ok = s->selftest_ok;
    out->ot_warn = ot_warn;
    out->fault_code = s->mode == BMC_MODE_FAILSAFE ? s->fault_code : BMC_FAULT_NONE;
    out->fault_mask = active;
    out->ot_limit_c = s->ot_limit_c;
}

const char *safety_request_mode(safety_t *s, bmc_mode_t target, bool profile_running, bool heater_zero)
{
    if (target == BMC_MODE_SELFTEST || target == BMC_MODE_FAILSAFE || (unsigned)target >= BMC_MODE_COUNT) {
        return "bad_mode";
    }
    if (s->mode == BMC_MODE_FAILSAFE) {
        return "latched";
    }
    if (s->mode == BMC_MODE_SELFTEST) {
        return "busy";
    }
    if (target == s->mode) {
        return NULL;
    }
    if (target == BMC_MODE_STANDBY) {
        set_mode(s, BMC_MODE_STANDBY, "request");
        return NULL;
    }
    if (!s->selftest_ok) {
        return "no_selftest";
    }
    if (bmc_mode_is_actuating(s->mode)) {              /* actuating -> actuating */
        if (profile_running) {
            return "profile_running";
        }
        if (!heater_zero) {
            return "heater_on";
        }
    }
    set_mode(s, target, "request");
    return NULL;
}

const char *safety_begin_selftest(safety_t *s)
{
    if (s->mode == BMC_MODE_FAILSAFE) {
        return "latched";
    }
    if (s->mode != BMC_MODE_STANDBY) {
        return "bad_state";
    }
    s->selftest_ok = false;
    set_mode(s, BMC_MODE_SELFTEST, "request");
    return NULL;
}

void safety_selftest_done(safety_t *s, bool pass)
{
    if (s->mode != BMC_MODE_SELFTEST) {
        return;
    }
    s->selftest_ok = pass;
    set_mode(s, BMC_MODE_STANDBY, pass ? "selftest_pass" : "selftest_fail");
}

const char *safety_ack(safety_t *s)
{
    if (s->mode != BMC_MODE_FAILSAFE) {
        return "bad_state";
    }
    if (!s->safe_to_handle) {
        return "not_safe";
    }
    if (!s->last_plate_valid || s->last_plate_c >= BMC_SAFE_CLEAR_C) {
        return "not_safe";
    }
    if (s->active_mask != 0) {
        return "fault_active";
    }
    bmc_ev_emit(s->ev, BMC_EV_ACK_OK, s->now_ms, -1, bmc_fault_name(s->fault_code),
                "plate_c=%.1f latched_mask=0x%x", (double)s->last_plate_c, (unsigned)s->latched_mask);
    s->fault_code = BMC_FAULT_NONE;
    s->latched_mask = 0;
    s->safe_to_handle = false;
    s->safe_timing = false;
    s->selftest_ok = false;                 /* a full self-test must pass again before heat */
    s->ot_limit_c = BMC_OT_TRIP_C;          /* a lowered test limit does not survive a recovery */
    s->inject_mask = 0;
    set_mode(s, BMC_MODE_STANDBY, "ack");
    return NULL;
}

void safety_set_ot_limit(safety_t *s, float limit_c)
{
    if (limit_c > BMC_OT_TRIP_C) {
        limit_c = BMC_OT_TRIP_C;
    }
    s->ot_limit_c = limit_c;
}

void safety_inject(safety_t *s, bmc_fault_t fault, bool on)
{
    if (on) {
        s->inject_mask |= BMC_FAULT_BIT(fault);
    } else {
        s->inject_mask &= ~BMC_FAULT_BIT(fault);
    }
}

void safety_clear_injections(safety_t *s)
{
    s->inject_mask = 0;
}

void safety_note_reset(safety_t *s, bmc_fault_t reason)
{
    bmc_ev_emit(s->ev, BMC_EV_RESET, s->now_ms, -1, bmc_fault_name(reason), "boot into STANDBY, heater off");
}
