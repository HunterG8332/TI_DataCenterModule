#include "bmc_sample.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *b;
    size_t n;
    size_t used;
    bool bad;
} writer_t;

static void put(writer_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(writer_t *w, const char *fmt, ...)
{
    if (w->bad) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(w->b + w->used, w->n - w->used, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= w->n - w->used) {
        w->bad = true;
        return;
    }
    w->used += (size_t)r;
}

static void put_f(writer_t *w, bool valid, float v, int digits)
{
    if (valid) {
        put(w, "%.*f", digits, (double)v);
    }
}

#define HEADER                                                                                      \
    "bmc_id,seq,t_us,run_ms,mode,plate_c,inlet_c,v48_v,i48_a,power_w,v33_v,v5_v,v12_v,"             \
    "heater_cmd_pct,heater_pct,fan_demand_pct,fan_target_rpm,fan1_duty_pct,fan2_duty_pct,"          \
    "fan1_rpm,fan2_rpm,fan_en,setpoint_c,ctl_p,ctl_i,ctl_ff,ff_variant,fault_code,fault_mask,"      \
    "flags,drops\n"

int bmc_fmt_header(char *buf, size_t n)
{
    writer_t w = { buf, n, 0, false };
    put(&w, "%s", HEADER);
    return w.bad ? -1 : (int)w.used;
}

int bmc_fmt_record(char *buf, size_t n, const char *bmc_id, const telemetry_sample_t *s, uint32_t drops)
{
    writer_t w = { buf, n, 0, false };
    put(&w, "%s,%lu,%lld,", bmc_id, (unsigned long)s->seq, (long long)s->t_us);
    if (s->run_ms >= 0) {
        put(&w, "%lld", (long long)s->run_ms);
    }
    put(&w, ",%s,", bmc_mode_name(s->mode));
    put_f(&w, s->plate_valid, s->plate_c, 4);
    put(&w, ",");
    put_f(&w, s->inlet_valid, s->inlet_c, 4);
    put(&w, ",");
    put_f(&w, s->ina_valid, s->v48_v, 3);
    put(&w, ",");
    put_f(&w, s->ina_valid, s->i48_a, 4);
    put(&w, ",");
    put_f(&w, s->ina_valid, s->power_w, 3);
    for (int i = 0; i < 3; i++) {
        put(&w, ",");
        put_f(&w, s->rails_valid, s->rail_v[i], 3);
    }
    put(&w, ",%.1f,%.1f,%.1f,%lu,%.1f,%.1f,%lu,%lu,%d,%.1f,%.2f,%.2f,%.2f,%d,%lu,%lu,%lu,%lu\n",
        (double)s->heater_cmd_pct, (double)s->heater_pct, (double)s->fan_demand_pct,
        (unsigned long)s->fan_target_rpm, (double)s->fan_duty_pct[0], (double)s->fan_duty_pct[1],
        (unsigned long)s->fan_rpm[0], (unsigned long)s->fan_rpm[1], s->fan_enabled ? 1 : 0,
        (double)s->setpoint_c, (double)s->ctl_p, (double)s->ctl_i, (double)s->ctl_ff, s->ff_variant,
        (unsigned long)s->fault_code, (unsigned long)s->fault_mask, (unsigned long)s->flags,
        (unsigned long)drops);
    return w.bad ? -1 : (int)w.used;
}

int bmc_fmt_event(char *buf, size_t n, const bmc_event_t *e)
{
    writer_t w = { buf, n, 0, false };
    put(&w, "EVENT,%lu,%lld,", (unsigned long)e->id, (long long)e->t_ms * 1000LL);
    if (e->run_ms >= 0) {
        put(&w, "%lld", (long long)e->run_ms);
    }
    put(&w, ",%s,%s,%s\n", bmc_ev_type_name(e->type), e->code, e->detail);
    return w.bad ? -1 : (int)w.used;
}
