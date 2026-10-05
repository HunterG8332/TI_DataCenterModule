#include "bmc_telemetry.h"

#include <stdatomic.h>
#include "esp_timer.h"
#include "freertos/queue.h"

#define TELEMETRY_QUEUE_LEN 32   /* 3.2 s of headroom at 10 Hz */

static QueueHandle_t s_queue;
static atomic_uint s_seq;
static atomic_uint s_drops;

void telemetry_init(void)
{
    s_queue = xQueueCreate(TELEMETRY_QUEUE_LEN, sizeof(telemetry_sample_t));
}

void telemetry_publish(telemetry_sample_t *s)
{
    s->seq = atomic_fetch_add(&s_seq, 1) + 1;
    s->t_us = esp_timer_get_time();
    if (xQueueSend(s_queue, s, 0) != pdTRUE) {
        atomic_fetch_add(&s_drops, 1);
    }
}

bool telemetry_receive(telemetry_sample_t *out, TickType_t timeout)
{
    return xQueueReceive(s_queue, out, timeout) == pdTRUE;
}

uint32_t telemetry_drops(void)
{
    return atomic_load(&s_drops);
}

void telemetry_count_drop(void)
{
    atomic_fetch_add(&s_drops, 1);
}
