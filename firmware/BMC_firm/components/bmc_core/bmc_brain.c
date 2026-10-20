#include "bmc_brain.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "bmc_fanrpm.h"
#include "bmc_sample.h"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Legacy temperature curve: 0% at LOW, 100% at HIGH. Used where no control law is
 * running (STANDBY, SELFTEST, FAILSAFE bookkeeping, CHAR before FAN is set). */
static float curve_demand(bool valid, float temp_c)
{
    if (!valid) {
        return 0.0f;
    }
    return clampf((temp_c - BMC_FAN_CURVE_LOW_C) * 100.0f / (BMC_FAN_CURVE_HIGH_C - BMC_FAN_CURVE_LOW_C),
                  0.0f, 100.0f);
}

static void reset_control(brain_t *b)
{
    bmc_pid_reset(&b->pid);
    bmc_ff_reset(&b->ff);
    b->fan_demand = 0.0f;
    b->ctl_started = false;
}

void brain_init(brain_t *b, uint32_t now_ms, bool test_hooks)
{
    memset(b, 0, sizeof(*b));
    bmc_ev_init(&b->ev);
    safety_init(&b->safety, &b->ev, now_ms);
    bmc_pid_init(&b->pid, BMC_PID_KP, BMC_PID_KI, BMC_PID_KD);
    bmc_ff_defaults(b->ff_params);
    bmc_ff_reset(&b->ff);
    b->setpoint_c = BMC_DEFAULT_SETPOINT_C;
    b->rpm_ref = BMC_RPM_REF;
    b->test_hooks = test_hooks;
    b->manual_fan_auto = true;
    b->st.run_ms = -1;
    b->st.setpoint_c = b->setpoint_c;
    b->st.ot_limit_c = BMC_OT_TRIP_C;
}

void brain_note_reset(brain_t *b, bmc_fault_t reason)
{
    safety_note_reset(&b->safety, reason);
}

static void end_run(brain_t *b, uint32_t now_ms, const char *why)
{
    if (b->run_active) {
        bmc_ev_emit(&b->ev, BMC_EV_RUN_END, now_ms, (int64_t)(now_ms - b->run_start_ms), why,
                    "mode=%s", bmc_mode_name(b->safety.mode));
    }
    b->run_active = false;
    bmc_profile_stop(&b->profile);
}

void brain_step(brain_t *b, const brain_in_t *in, brain_out_t *out)
{
    const uint32_t now = in->now_ms;
    b->last_in = *in;
    memset(out, 0, sizeof(*out));

    safety_in_t si = in->safety;
    si.now_ms = now;
    safety_out_t so;
    safety_step(&b->safety, &si, &so);

    if (so.force_safe) {
        end_run(b, now, "failsafe");
        b->manual_heater_on = false;
    }

    /* ---- self-test ---- */
    if (so.mode == BMC_MODE_SELFTEST) {
        selftest_in_t ti = {
            .now_ms = now, .plate_valid = in->safety.plate_valid, .plate_c = in->safety.plate_c,
            .ina_valid = in->safety.ina_valid, .v48_v = in->v48_v, .inlet_fitted = in->inlet_fitted,
            .inlet_valid = in->inlet_valid, .inlet_c = in->inlet_c,
            .fan_interlock_ok = in->safety.fan_interlock_ok, .heater_applied_pct = b->heater_out,
        };
        for (int i = 0; i < BMC_FAN_COUNT; i++) {
            ti.fan[i].window_full = in->fan[i].window_full;
            ti.fan[i].rpm = in->fan[i].rpm;
        }
        selftest_out_t to;
        bmc_selftest_step(&b->selftest, &ti, &to);
        out->fan_selftest = true;
        for (int i = 0; i < BMC_FAN_COUNT; i++) {
            out->fan_test_duty[i] = to.fan_duty[i];
        }
        if (to.done) {
            bmc_ev_emit(&b->ev, BMC_EV_SELFTEST, now, -1, to.pass ? "PASS" : "FAIL", "%s", to.msg);
            safety_selftest_done(&b->safety, to.pass);
            so.mode = b->safety.mode;
            so.selftest_ok = b->safety.selftest_ok;
        }
    }

    /* ---- control law, once per fresh sensor sample ---- */
    bool new_sample = (in->sample_seq != b->last_sample_seq);
    if (new_sample) {
        b->last_sample_seq = in->sample_seq;
        float dt = b->ctl_started ? (float)(now - b->last_ctl_ms) / 1000.0f : 0.1f;
        dt = clampf(dt, 0.02f, 0.5f);
        b->last_ctl_ms = now;
        bool plate_ok = in->safety.plate_valid;
        float plate = in->safety.plate_c;
        bool ina_ok = in->safety.ina_valid;

        switch (so.mode) {
        case BMC_MODE_REACTIVE:
            b->pid.offset = 0.0f;
            if (plate_ok) {
                b->fan_demand = bmc_pid_step(&b->pid, plate, b->setpoint_c, dt);
            }
            break;
        case BMC_MODE_PROACTIVE: {
            float ff = ina_ok ? bmc_ff_compute(b->ff_variant, b->ff_params[b->ff_variant], &b->ff,
                                               in->i48_a, dt) : 0.0f;
            b->pid.offset = ff;
            if (plate_ok) {
                b->fan_demand = bmc_pid_step(&b->pid, plate, b->setpoint_c, dt);
            }
            break;
        }
        case BMC_MODE_CHAR:
            b->fan_demand = b->manual_fan_auto ? curve_demand(plate_ok, plate) : b->manual_fan_pct;
            break;
        default:
            reset_control(b);
            b->fan_demand = curve_demand(plate_ok, plate);
            break;
        }
        b->ctl_started = true;
    }

    /* ---- heater proposal ---- */
    float heater_cmd = 0.0f;
    uint32_t run_ms = 0;
    if (b->profile.running) {
        float p = 0.0f;
        if (bmc_profile_step(&b->profile, now, &p, &run_ms)) {
            heater_cmd = p;
        } else {
            end_run(b, now, "complete");
        }
    }
    if (!b->profile.running && so.mode == BMC_MODE_CHAR && b->manual_heater_on &&
        (uint32_t)(now - b->manual_heater_ms) <= BMC_HEATER_REQ_TIMEOUT_MS) {
        heater_cmd = b->manual_heater_pct;
    }
    b->heater_cmd = heater_cmd;

    /* ---- arbitration: safety has the last word ---- */
    bool allow = so.heater_allowed && !so.force_safe && in->safety.heater_permit &&
                 in->safety.fan_interlock_ok && in->safety.plate_valid;
    float applied = allow ? clampf(heater_cmd, 0.0f, BMC_HEATER_MAX_PCT_BRINGUP) : 0.0f;
    b->heater_out = applied;
    out->heater_pct = applied;
    out->heater_enable = allow && applied > 0.0f;
    out->fan_full = so.force_safe;
    out->fan_demand_pct = so.force_safe ? 100.0f : b->fan_demand;

    /* ---- status ---- */
    brain_status_t *st = &out->st;
    st->mode = so.mode;
    st->fault_code = (uint32_t)so.fault_code;
    st->fault_mask = so.fault_mask;
    st->selftest_ok = so.selftest_ok;
    st->safe_to_handle = so.safe_to_handle;
    st->profile_running = b->profile.running;
    st->ot_warn = so.ot_warn;
    st->run_ms = b->profile.running ? (int64_t)run_ms : -1;
    st->heater_cmd_pct = heater_cmd;
    st->heater_pct = applied;
    st->fan_demand_pct = out->fan_demand_pct;
    st->fan_target_rpm = (uint32_t)(bmc_fanrpm_target(out->fan_demand_pct, b->rpm_ref) + 0.5f);
    st->setpoint_c = b->setpoint_c;
    st->ctl_p = b->pid.p_term;
    st->ctl_i = b->pid.i_term;
    st->ctl_ff = (so.mode == BMC_MODE_PROACTIVE) ? b->pid.offset : 0.0f;
    st->ff_variant = b->ff_variant;
    st->ot_limit_c = so.ot_limit_c;

    uint32_t f = 0;
    f |= (heater_cmd > 0.0f && so.actuating) ? TELEM_FLAG_HEATER_REQUESTED : 0;
    f |= in->safety.heater_permit ? TELEM_FLAG_HEATER_PERMIT : 0;
    f |= out->heater_enable ? TELEM_FLAG_HEATER_ACTIVE : 0;
    f |= (in->safety.fan_stalled[0] || in->safety.fan_stalled[1]) ? TELEM_FLAG_FAN_STALL : 0;
    f |= (so.fault_mask & BMC_FAULT_BIT(BMC_FAULT_OVER_TEMP)) ? TELEM_FLAG_OVER_TEMP : 0;
    f |= !in->safety.plate_valid ? TELEM_FLAG_TEMP_INVALID : 0;
    f |= !in->safety.fan_interlock_ok ? TELEM_FLAG_FAN_DISABLED : 0;
    f |= so.force_safe ? TELEM_FLAG_HEATER_TRIPPED : 0;
    f |= so.selftest_ok ? TELEM_FLAG_SELFTEST_OK : 0;
    f |= so.safe_to_handle ? TELEM_FLAG_SAFE_TO_HANDLE : 0;
    f |= b->profile.running ? TELEM_FLAG_PROFILE_RUNNING : 0;
    f |= so.ot_warn ? TELEM_FLAG_OT_WARN : 0;
    f |= b->test_hooks ? TELEM_FLAG_TEST_HOOKS : 0;
    f |= (so.ot_limit_c < BMC_OT_TRIP_C) ? TELEM_FLAG_OT_LOWERED : 0;
    st->flags = f;
    b->st = *st;
}

/* ---- commands ------------------------------------------------------------- */

static int reply_ok(char *r, size_t n, const bmc_cmd_t *c, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int reply_ok(char *r, size_t n, const bmc_cmd_t *c, const char *fmt, ...)
{
    int len = 0;
    if (c->has_token) {
        len = snprintf(r, n, "#%lu ", (unsigned long)c->token);
    }
    len += snprintf(r + len, n - (size_t)len, "OK %s", bmc_cmd_name(c->id));
    if (fmt && fmt[0]) {
        va_list ap;
        va_start(ap, fmt);
        len += snprintf(r + len, n - (size_t)len, " ");
        len += vsnprintf(r + len, n - (size_t)len, fmt, ap);
        va_end(ap);
    }
    return len;
}

static int reply_err(char *r, size_t n, const bmc_cmd_t *c, const char *reason)
{
    int len = 0;
    if (c->has_token) {
        len = snprintf(r, n, "#%lu ", (unsigned long)c->token);
    }
    len += snprintf(r + len, n - (size_t)len, "ERR %s %s", bmc_cmd_name(c->id), reason);
    return len;
}

static bool run_blocked(const brain_t *b, const char **why)
{
    if (b->profile.running) {
        *why = "profile_running";
        return true;
    }
    if (b->safety.mode == BMC_MODE_FAILSAFE) {
        *why = "latched";
        return true;
    }
    if (b->safety.mode == BMC_MODE_SELFTEST) {
        *why = "busy";
        return true;
    }
    return false;
}

int brain_command(brain_t *b, const bmc_cmd_t *c, uint32_t now_ms, char *r, size_t n)
{
    const char *why = NULL;
    bmc_mode_t mode = b->safety.mode;

    switch (c->id) {
    case BMC_CMD_HELLO:
        return 0;

    case BMC_CMD_STATUS: {
        const brain_status_t *s = &b->st;
        return reply_ok(r, n, c,
                        "mode=%s selftest=%d fault=%s mask=0x%x safe_to_handle=%d profile=%s run_ms=%lld "
                        "plate_c=%.1f setpoint_c=%.1f ot_limit_c=%.1f heater_pct=%.1f fan_demand_pct=%.1f "
                        "ff=%d test_hooks=%d",
                        bmc_mode_name(s->mode), s->selftest_ok ? 1 : 0,
                        bmc_fault_name((bmc_fault_t)s->fault_code), (unsigned)s->fault_mask,
                        s->safe_to_handle ? 1 : 0, s->profile_running ? "running" : "idle",
                        (long long)s->run_ms, (double)b->last_in.safety.plate_c, (double)s->setpoint_c,
                        (double)s->ot_limit_c, (double)s->heater_pct, (double)s->fan_demand_pct,
                        s->ff_variant, b->test_hooks ? 1 : 0);
    }

    case BMC_CMD_EVENTS: {
        bmc_event_t evs[BMC_EV_RING];
        int cnt = bmc_ev_recent(&b->ev, evs, BMC_EV_RING);
        int len = reply_ok(r, n, c, "count=%d", cnt);
        for (int i = 0; i < cnt; i++) {
            char line[260];
            int l = bmc_fmt_event(line, sizeof(line), &evs[i]);
            if (l > 0 && (size_t)(len + 1 + l) < n) {
                r[len++] = '\n';
                memcpy(r + len, line, (size_t)l - 1);       /* drop the event's own newline */
                len += l - 1;
                r[len] = '\0';
            }
        }
        return len;
    }

    case BMC_CMD_SELFTEST:
        why = safety_begin_selftest(&b->safety);
        if (why) {
            return reply_err(r, n, c, why);
        }
        end_run(b, now_ms, "selftest");
        b->manual_heater_on = false;
        selftest_in_t ti = { .now_ms = now_ms };
        bmc_selftest_start(&b->selftest, &ti);
        return reply_ok(r, n, c, "started");

    case BMC_CMD_MODE:
        why = safety_request_mode(&b->safety, c->mode, b->profile.running, b->heater_out <= 0.0f);
        if (why) {
            return reply_err(r, n, c, why);
        }
        if (c->mode != mode) {
            end_run(b, now_ms, "mode_change");
            b->manual_heater_on = false;
            b->manual_fan_auto = true;
            reset_control(b);
        }
        return reply_ok(r, n, c, "%s", bmc_mode_name(b->safety.mode));

    case BMC_CMD_ACK:
        why = safety_ack(&b->safety);
        if (why) {
            return reply_err(r, n, c, why);
        }
        reset_control(b);
        return reply_ok(r, n, c, "STANDBY");

    case BMC_CMD_HEATER:
        if (!c->flag) {                              /* "off" is always accepted, and silently */
            b->manual_heater_on = false;
            return 0;
        }
        if (mode != BMC_MODE_CHAR) {
            return reply_err(r, n, c, "mode");
        }
        b->manual_heater_on = true;
        b->manual_heater_pct = clampf(c->f[0], 0.0f, 100.0f);
        b->manual_heater_ms = now_ms;
        return 0;                                    /* keep-alive: no reply traffic */

    case BMC_CMD_FAN:
        if (mode != BMC_MODE_CHAR) {
            return reply_err(r, n, c, "mode");
        }
        if (c->flag) {
            b->manual_fan_auto = true;
            return reply_ok(r, n, c, "AUTO");
        }
        if (c->f[0] < 0.0f || c->f[0] > 100.0f) {
            return reply_err(r, n, c, "range");
        }
        b->manual_fan_auto = false;
        b->manual_fan_pct = c->f[0];
        return reply_ok(r, n, c, "%.1f", (double)c->f[0]);

    case BMC_CMD_SETPOINT:
        if (run_blocked(b, &why)) {
            return reply_err(r, n, c, why);
        }
        if (c->f[0] < BMC_SETPOINT_MIN_C || c->f[0] > BMC_SETPOINT_MAX_C) {
            return reply_err(r, n, c, "range");
        }
        b->setpoint_c = c->f[0];
        return reply_ok(r, n, c, "%.1f", (double)b->setpoint_c);

    case BMC_CMD_GAINS:
        if (run_blocked(b, &why)) {
            return reply_err(r, n, c, why);
        }
        for (int i = 0; i < 3; i++) {
            if (c->f[i] < 0.0f || c->f[i] > BMC_GAIN_MAX) {
                return reply_err(r, n, c, "range");
            }
        }
        b->pid.kp = c->f[0];
        b->pid.ki = c->f[1];
        b->pid.kd = c->f[2];
        bmc_pid_reset(&b->pid);
        return reply_ok(r, n, c, "kp=%.3f ki=%.3f kd=%.3f", (double)c->f[0], (double)c->f[1], (double)c->f[2]);

    case BMC_CMD_FF:
        if (run_blocked(b, &why)) {
            return reply_err(r, n, c, why);
        }
        if (c->i[0] < 0 || c->i[0] >= bmc_ff_count()) {
            return reply_err(r, n, c, "range");
        }
        b->ff_variant = c->i[0];
        bmc_ff_reset(&b->ff);
        return reply_ok(r, n, c, "%d %s", b->ff_variant, bmc_ff_name(b->ff_variant));

    case BMC_CMD_FFPARAM:
        if (run_blocked(b, &why)) {
            return reply_err(r, n, c, why);
        }
        if (c->i[0] < 0 || c->i[0] >= bmc_ff_count() || c->i[1] < 0 || c->i[1] >= BMC_FF_PARAMS) {
            return reply_err(r, n, c, "range");
        }
        b->ff_params[c->i[0]][c->i[1]] = c->f[0];
        return reply_ok(r, n, c, "%d[%d]=%.4f", c->i[0], c->i[1], (double)c->f[0]);

    case BMC_CMD_PROFILE_CLEAR:
        if (run_blocked(b, &why)) {
            return reply_err(r, n, c, why);
        }
        bmc_profile_clear(&b->profile);
        return reply_ok(r, n, c, "CLEAR");

    case BMC_CMD_PROFILE_ADD:
        if (run_blocked(b, &why)) {
            return reply_err(r, n, c, why);
        }
        if (!bmc_profile_add(&b->profile, (uint32_t)c->i[0], c->f[0])) {
            return reply_err(r, n, c, "range");
        }
        return reply_ok(r, n, c, "ADD steps=%d", b->profile.n);

    case BMC_CMD_PROFILE_PRESET:
        if (run_blocked(b, &why)) {
            return reply_err(r, n, c, why);
        }
        if (!bmc_profile_preset(&b->profile, c->i[0])) {
            return reply_err(r, n, c, "range");
        }
        return reply_ok(r, n, c, "PRESET %d steps=%d total_s=%lu", c->i[0], b->profile.n,
                        (unsigned long)bmc_profile_total_s(&b->profile));

    case BMC_CMD_PROFILE_START: {
        if (b->profile.running) {
            return reply_err(r, n, c, "profile_running");
        }
        if (!bmc_mode_is_actuating(mode)) {
            return reply_err(r, n, c, mode == BMC_MODE_FAILSAFE ? "latched" : "mode");
        }
        if (b->profile.n == 0) {
            return reply_err(r, n, c, "no_profile");
        }
        const brain_in_t *in = &b->last_in;
        bool at_ambient = in->safety.plate_valid &&
            (in->inlet_valid ? (in->safety.plate_c - in->inlet_c) <= BMC_START_MAX_ABOVE_AMBIENT_C
                             : in->safety.plate_c <= BMC_START_MAX_PLATE_NO_INLET_C);
        if (!at_ambient) {
            return reply_err(r, n, c, "not_at_ambient");
        }
        bmc_profile_start(&b->profile, now_ms);
        b->run_active = true;
        b->run_start_ms = now_ms;
        bmc_pid_reset(&b->pid);
        bmc_ff_reset(&b->ff);
        bmc_ev_emit(&b->ev, BMC_EV_RUN_START, now_ms, 0, bmc_mode_name(mode),
                    "profile_steps=%d total_s=%lu setpoint=%.1f kp=%.3f ki=%.3f kd=%.3f ff=%d(%s) rpm_ref=%.0f ot_limit=%.1f plate_c=%.1f inlet_c=%.1f",
                    b->profile.n, (unsigned long)bmc_profile_total_s(&b->profile), (double)b->setpoint_c,
                    (double)b->pid.kp, (double)b->pid.ki, (double)b->pid.kd, b->ff_variant,
                    bmc_ff_name(b->ff_variant), (double)b->rpm_ref, (double)b->safety.ot_limit_c,
                    (double)in->safety.plate_c, in->inlet_valid ? (double)in->inlet_c : -1000.0);
        return reply_ok(r, n, c, "START");
    }

    case BMC_CMD_PROFILE_STOP:
        end_run(b, now_ms, "stopped");
        b->manual_heater_on = false;
        return reply_ok(r, n, c, "STOP");

    case BMC_CMD_TESTSET_OT:
        if (!b->test_hooks) {
            return reply_err(r, n, c, "disabled");
        }
        if (c->f[0] < 0.0f) {
            return reply_err(r, n, c, "range");
        }
        safety_set_ot_limit(&b->safety, c->f[0]);
        return reply_ok(r, n, c, "ot_limit_c=%.1f", (double)b->safety.ot_limit_c);

    case BMC_CMD_TESTINJECT:
        if (!b->test_hooks) {
            return reply_err(r, n, c, "disabled");
        }
        if (c->flag) {
            safety_clear_injections(&b->safety);
            return reply_ok(r, n, c, "cleared");
        }
        safety_inject(&b->safety, c->fault, true);
        return reply_ok(r, n, c, "%s", bmc_fault_name(c->fault));

    case BMC_CMD_UNKNOWN:
        return reply_err(r, n, c, "unknown");
    case BMC_CMD_BAD_ARGS:
    default:
        return reply_err(r, n, c, "args");
    }
}
