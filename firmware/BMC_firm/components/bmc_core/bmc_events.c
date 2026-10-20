#include "bmc_events.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const TYPE_NAMES[BMC_EV_COUNT] = {
    "FAILSAFE_ENTER", "SAFE_TO_HANDLE", "ACK_OK", "SELFTEST", "MODE_CHANGE",
    "RUN_START", "RUN_END", "RESET", "WARN",
};

const char *bmc_ev_type_name(bmc_ev_type_t t)
{
    return (unsigned)t < BMC_EV_COUNT ? TYPE_NAMES[t] : "?";
}

void bmc_ev_init(bmc_events_t *e)
{
    memset(e, 0, sizeof(*e));
    e->next_id = 1;
}

void bmc_ev_emit(bmc_events_t *e, bmc_ev_type_t type, uint32_t t_ms, int64_t run_ms,
                 const char *code, const char *fmt, ...)
{
    bmc_event_t *ev = &e->ring[(e->next_id - 1) % BMC_EV_RING];
    ev->id = e->next_id++;
    ev->type = type;
    ev->t_ms = t_ms;
    ev->run_ms = run_ms;
    snprintf(ev->code, sizeof(ev->code), "%s", code ? code : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev->detail, sizeof(ev->detail), fmt, ap);
    va_end(ap);
    for (char *p = ev->detail; *p; p++) {       /* keep the line CSV-safe */
        if (*p == ',' || *p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
}

bool bmc_ev_pop(bmc_events_t *e, bmc_event_t *out)
{
    uint32_t newest = e->next_id - 1;
    if (e->sent_id >= newest) {
        return false;
    }
    uint32_t oldest_kept = newest > BMC_EV_RING ? newest - BMC_EV_RING + 1 : 1;
    uint32_t id = e->sent_id + 1;
    if (id < oldest_kept) {
        id = oldest_kept;                       /* the network side fell behind; skip the overwritten */
    }
    *out = e->ring[(id - 1) % BMC_EV_RING];
    e->sent_id = id;
    return true;
}

int bmc_ev_recent(const bmc_events_t *e, bmc_event_t *out, int max)
{
    uint32_t newest = e->next_id - 1;
    uint32_t count = newest < BMC_EV_RING ? newest : BMC_EV_RING;
    if ((int)count > max) {
        count = (uint32_t)max;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = newest - count + 1 + i;
        out[i] = e->ring[(id - 1) % BMC_EV_RING];
    }
    return (int)count;
}
