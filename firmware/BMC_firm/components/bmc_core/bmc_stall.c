#include "bmc_stall.h"

#include <string.h>
#include "bmc_config.h"

void bmc_stall_init(bmc_stall_t *d)
{
    memset(d, 0, sizeof(*d));
}

void bmc_stall_update(bmc_stall_t *d, uint32_t now_ms, bool fan_enabled, float duty_pct,
                      uint32_t rpm, bool window_full)
{
    if (fan_enabled && duty_pct >= BMC_FAN_STALL_MIN_DUTY_PCT) {
        if (!d->above) {
            d->above = true;
            d->above_since_ms = now_ms;
        }
    } else {
        d->above = false;
    }
    bool armed = d->above && (uint32_t)(now_ms - d->above_since_ms) >= BMC_FAN_TACH_WINDOW_MS;

    if (!d->stalled && armed && window_full && rpm == 0) {
        d->stalled = true;
        d->recover = 0;
    }
    if (d->stalled) {
        d->recover = (window_full && rpm > 0) ? d->recover + 1 : 0;
        if (d->recover >= BMC_FAN_STALL_RECOVER_CYCLES) {
            d->stalled = false;
            d->recover = 0;
            d->above = false;
        }
    }
}
