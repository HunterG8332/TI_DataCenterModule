#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Small event log: the failsafe record (cause + timestamp), self-test report,
 * mode changes, run start/end. Events are queued for the network side (pop)
 * and the last BMC_EV_RING are kept for replay (recent). Detail text never
 * contains commas, so it can ride in a CSV-style line. */

typedef enum {
    BMC_EV_FAILSAFE_ENTER,
    BMC_EV_SAFE_TO_HANDLE,
    BMC_EV_ACK_OK,
    BMC_EV_SELFTEST,
    BMC_EV_MODE_CHANGE,
    BMC_EV_RUN_START,
    BMC_EV_RUN_END,
    BMC_EV_RESET,
    BMC_EV_WARN,
    BMC_EV_COUNT
} bmc_ev_type_t;

#define BMC_EV_RING 16
#define BMC_EV_CODE_LEN 20
#define BMC_EV_DETAIL_LEN 144

typedef struct {
    uint32_t id;                    /* 1, 2, 3 ... never reused; hosts de-duplicate on it */
    bmc_ev_type_t type;
    uint32_t t_ms;                  /* BMC milliseconds since boot */
    int64_t run_ms;                 /* -1 when no run is active */
    char code[BMC_EV_CODE_LEN];
    char detail[BMC_EV_DETAIL_LEN];
} bmc_event_t;

typedef struct {
    bmc_event_t ring[BMC_EV_RING];
    uint32_t next_id;               /* id the next event will get */
    uint32_t sent_id;               /* last id handed out by bmc_ev_pop */
} bmc_events_t;

const char *bmc_ev_type_name(bmc_ev_type_t t);
void bmc_ev_init(bmc_events_t *e);
void bmc_ev_emit(bmc_events_t *e, bmc_ev_type_t type, uint32_t t_ms, int64_t run_ms,
                 const char *code, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
bool bmc_ev_pop(bmc_events_t *e, bmc_event_t *out);                 /* next event not yet popped */
int bmc_ev_recent(const bmc_events_t *e, bmc_event_t *out, int max); /* oldest first */
