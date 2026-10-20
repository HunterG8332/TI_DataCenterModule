#include "bmc_profile.h"

#include <string.h>

void bmc_profile_clear(bmc_profile_t *p)
{
    memset(p, 0, sizeof(*p));
}

bool bmc_profile_add(bmc_profile_t *p, uint32_t dur_s, float heater_pct)
{
    if (p->running || p->n >= BMC_PROFILE_MAX_STEPS || dur_s == 0 || dur_s > 24u * 3600u) {
        return false;
    }
    if (!(heater_pct >= 0.0f) || heater_pct > 100.0f) {      /* also rejects NaN */
        return false;
    }
    p->steps[p->n].dur_s = dur_s;
    p->steps[p->n].heater_pct = heater_pct;
    p->n++;
    return true;
}

bool bmc_profile_preset(bmc_profile_t *p, int id)
{
    if (p->running) {
        return false;
    }
    bmc_profile_clear(p);
    switch (id) {
    case 1:
        return bmc_profile_add(p, 60, 0) && bmc_profile_add(p, 600, 100) && bmc_profile_add(p, 300, 0);
    case 2:
        return bmc_profile_add(p, 10, 0) && bmc_profile_add(p, 30, 50) && bmc_profile_add(p, 20, 0);
    case 3: {
        static const float levels[] = { 0, 25, 50, 75, 100, 0 };
        for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
            if (!bmc_profile_add(p, 180, levels[i])) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool bmc_profile_start(bmc_profile_t *p, uint32_t now_ms)
{
    if (p->running || p->n == 0) {
        return false;
    }
    p->running = true;
    p->cur = 0;
    p->start_ms = now_ms;
    p->step_start_ms = now_ms;
    return true;
}

void bmc_profile_stop(bmc_profile_t *p)
{
    p->running = false;
}

bool bmc_profile_step(bmc_profile_t *p, uint32_t now_ms, float *heater_pct, uint32_t *run_ms)
{
    if (!p->running) {
        return false;
    }
    while (p->cur < p->n &&
           (uint32_t)(now_ms - p->step_start_ms) >= p->steps[p->cur].dur_s * 1000u) {
        p->step_start_ms += p->steps[p->cur].dur_s * 1000u;
        p->cur++;
    }
    if (p->cur >= p->n) {
        p->running = false;
        return false;
    }
    *heater_pct = p->steps[p->cur].heater_pct;
    *run_ms = now_ms - p->start_ms;
    return true;
}

uint32_t bmc_profile_total_s(const bmc_profile_t *p)
{
    uint32_t t = 0;
    for (unsigned i = 0; i < p->n; i++) {
        t += p->steps[i].dur_s;
    }
    return t;
}
