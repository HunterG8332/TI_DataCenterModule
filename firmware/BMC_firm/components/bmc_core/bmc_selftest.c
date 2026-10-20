#include "bmc_selftest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define COAST_MS 1500

static const float FAN_MAX[BMC_FAN_COUNT] = { BMC_FAN1_MAX_RPM, BMC_FAN2_MAX_RPM };

static void fail(bmc_selftest_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(bmc_selftest_t *t, const char *fmt, ...)
{
    size_t used = strlen(t->fails);
    if (used >= sizeof(t->fails) - 1) {
        t->nfails++;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(t->fails + used, sizeof(t->fails) - used, "%s", t->nfails ? "; " : "");
    if (n >= 0 && (size_t)n < sizeof(t->fails) - used) {
        used += (size_t)n;
        vsnprintf(t->fails + used, sizeof(t->fails) - used, fmt, ap);
    }
    va_end(ap);
    t->nfails++;
}

void bmc_selftest_start(bmc_selftest_t *t, const selftest_in_t *in)
{
    memset(t, 0, sizeof(*t));
    t->stage = ST_IDLE;
    t->stage_ms = in->now_ms;
}

static void instant_checks(bmc_selftest_t *t, const selftest_in_t *in)
{
    /* 1 + 2: enumeration and ambient plausibility of the plate sensor */
    if (!in->plate_valid) {
        fail(t, "tmp117_plate:no_response");
    } else if (in->plate_c < BMC_SELFTEST_AMBIENT_MIN_C || in->plate_c > BMC_SELFTEST_AMBIENT_MAX_C) {
        fail(t, "tmp117_plate:implausible_%.1fC", (double)in->plate_c);
    }
    if (in->inlet_fitted) {
        if (!in->inlet_valid) {
            fail(t, "tmp117_inlet:no_response");
        } else if (in->inlet_c < BMC_SELFTEST_AMBIENT_MIN_C || in->inlet_c > BMC_SELFTEST_AMBIENT_MAX_C) {
            fail(t, "tmp117_inlet:implausible_%.1fC", (double)in->inlet_c);
        }
    }
    /* 3: 48 V sense */
    if (!in->ina_valid) {
        fail(t, "ina238:no_response");
    } else if (BMC_SELFTEST_REQUIRE_48V &&
               (in->v48_v < BMC_SELFTEST_V48_MIN || in->v48_v > BMC_SELFTEST_V48_MAX)) {
        fail(t, "ina238:v48_%.1fV_out_of_range", (double)in->v48_v);
    }
    /* 4: interlock */
    if (!in->fan_interlock_ok) {
        fail(t, "fan_interlock:open");
        t->fans_skipped = true;
    }
    /* 6: heater really off */
    if (in->heater_applied_pct > 0.0f) {
        fail(t, "heater:not_off_%.1fpct", (double)in->heater_applied_pct);
    }
}

static void check_fan(bmc_selftest_t *t, const selftest_in_t *in, int k)
{
    int other = 1 - k;
    uint32_t rpm = in->fan[k].rpm;
    uint32_t cross = in->fan[other].rpm;
    bool own_low = !in->fan[k].window_full || (float)rpm < BMC_SELFTEST_FAN_MIN_FRAC * FAN_MAX[k];
    if (own_low && (float)cross >= BMC_SELFTEST_FAN_MIN_FRAC * FAN_MAX[other]) {
        /* the driven fan's own tach is silent but the other channel is counting: wires are swapped */
        fail(t, "fan%d:tach_swapped_with_fan%d rpm_on_other_channel=%u", k + 1, other + 1, (unsigned)cross);
    } else if (own_low) {
        fail(t, "fan%d:no_tach_rpm=%u", k + 1, (unsigned)rpm);
    } else if ((float)cross > BMC_SELFTEST_FAN_CROSS_FRAC * (float)rpm) {
        fail(t, "fan%d:tach_crosstalk_fan%d_rpm=%u", k + 1, other + 1, (unsigned)cross);
    }
}

static void finish(bmc_selftest_t *t, selftest_out_t *out)
{
    out->done = true;
    out->pass = (t->nfails == 0);
    if (out->pass) {
        snprintf(out->msg, sizeof(out->msg), "PASS enum ambient v48 interlock fan1 fan2 heater_off");
    } else {
        snprintf(out->msg, sizeof(out->msg), "FAIL %s", t->fails);
    }
    t->stage = ST_DONE;
}

void bmc_selftest_step(bmc_selftest_t *t, const selftest_in_t *in, selftest_out_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t elapsed = in->now_ms - t->stage_ms;

    switch (t->stage) {
    case ST_IDLE:
        instant_checks(t, in);
        t->stage = t->fans_skipped ? ST_DONE : ST_FAN1;
        t->stage_ms = in->now_ms;
        if (t->stage == ST_DONE) {
            finish(t, out);
        } else {
            out->fan_duty[0] = BMC_SELFTEST_FAN_DUTY_PCT;
        }
        break;
    case ST_FAN1:
        out->fan_duty[0] = BMC_SELFTEST_FAN_DUTY_PCT;
        if (elapsed >= BMC_SELFTEST_FAN_MS) {
            check_fan(t, in, 0);
            t->stage = ST_COAST;
            t->stage_ms = in->now_ms;
            out->fan_duty[0] = 0.0f;
        }
        break;
    case ST_COAST:
        if (elapsed >= COAST_MS) {
            t->stage = ST_FAN2;
            t->stage_ms = in->now_ms;
            out->fan_duty[1] = BMC_SELFTEST_FAN_DUTY_PCT;
        }
        break;
    case ST_FAN2:
        out->fan_duty[1] = BMC_SELFTEST_FAN_DUTY_PCT;
        if (elapsed >= BMC_SELFTEST_FAN_MS) {
            check_fan(t, in, 1);
            out->fan_duty[1] = 0.0f;
            finish(t, out);
        }
        break;
    case ST_DONE:
        out->done = true;
        break;
    }
}
