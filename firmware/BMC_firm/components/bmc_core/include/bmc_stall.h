#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Per-fan stall detector (moved here from main.c so it can be tested).
 * Stalled = the fan is permitted and commanded above the floor for at least one
 * full tach window, yet that window holds zero edges. Cleared after
 * BMC_FAN_STALL_RECOVER_CYCLES consecutive updates with the tach turning. */

typedef struct {
    uint32_t above_since_ms;
    bool above;
    bool stalled;
    unsigned recover;
} bmc_stall_t;

void bmc_stall_init(bmc_stall_t *d);
void bmc_stall_update(bmc_stall_t *d, uint32_t now_ms, bool fan_enabled, float duty_pct,
                      uint32_t rpm, bool window_full);
