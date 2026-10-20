#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "bmc_sample.h"   /* telemetry_sample_t and its text encoding (bmc_core, host-testable) */
#include "bmc_types.h"    /* TELEM_FLAG_* */

/* Hand-off between the control side (core 1) and the network side (core 0).
 * The producer never blocks: telemetry_publish() is a zero-timeout queue push
 * and, if the queue is full, only increments the drop counter. A stalled or
 * absent network therefore cannot extend a control period. */

void telemetry_init(void);

/* Fills s->seq and s->t_us, then pushes without blocking. */
void telemetry_publish(telemetry_sample_t *s);

/* Consumer side. Returns false on timeout. */
bool telemetry_receive(telemetry_sample_t *out, TickType_t timeout);

/* Samples lost before reaching the wire (queue full or send failure). */
uint32_t telemetry_drops(void);
void telemetry_count_drop(void);
