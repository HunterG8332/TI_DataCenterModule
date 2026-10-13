#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

/* Hand-off between the control side (core 1) and the network side (core 0).
 * The producer never blocks: telemetry_publish() is a zero-timeout queue push
 * and, if the queue is full, only increments the drop counter. A stalled or
 * absent network therefore cannot extend a control period. */

typedef struct {
    uint32_t seq;          /* stamped by telemetry_publish(); advances even when the sample is dropped */
    int64_t  t_us;         /* esp_timer microseconds since boot, stamped by telemetry_publish() */
    float    temp_c;
    bool     temp_valid;
    float    power_w;
    bool     power_valid;
    float    fan_duty_pct;
    bool     fan_enabled;
    uint32_t fan_rpm;
    float    heater_pct;   /* power actually being demanded of the emulator (0 when inhibited) */
    uint32_t flags;        /* TELEM_FLAG_* */
} telemetry_sample_t;

/* flags bits: heater request/arbitration state, so the dashboard can say why
 * the heater is off. */
#define TELEM_FLAG_HEATER_REQUESTED (1u << 0)  /* dashboard switch is on (heartbeat fresh) */
#define TELEM_FLAG_HEATER_PERMIT    (1u << 1)  /* hardware permit pin asserted */
#define TELEM_FLAG_HEATER_ACTIVE    (1u << 2)  /* heater enable line high, power applied */
#define TELEM_FLAG_FAN_STALL        (1u << 3)
#define TELEM_FLAG_OVER_TEMP        (1u << 4)
#define TELEM_FLAG_TEMP_INVALID     (1u << 5)
#define TELEM_FLAG_FAN_DISABLED     (1u << 6)  /* fan enable interlock open */
#define TELEM_FLAG_HEATER_TRIPPED   (1u << 7)  /* fault latched; switch off then on to re-arm */

void telemetry_init(void);

/* Fills s->seq and s->t_us, then pushes without blocking. */
void telemetry_publish(telemetry_sample_t *s);

/* Consumer side. Returns false on timeout. */
bool telemetry_receive(telemetry_sample_t *out, TickType_t timeout);

/* Samples lost before reaching the wire (queue full or send failure). */
uint32_t telemetry_drops(void);
void telemetry_count_drop(void);
