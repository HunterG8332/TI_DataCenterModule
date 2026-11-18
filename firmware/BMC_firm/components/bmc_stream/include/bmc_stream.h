#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

/* Streams telemetry samples as CSV over UDP to every host that has said HELLO,
 * carries event lines to the same hosts, and passes commands from those hosts to
 * a handler. Uses only BSD sockets, so it runs unchanged over Wi-Fi or Ethernet.
 * Wire format: dashboard/README.md and docs/bmc_plan/03. */

/* Where a command came from (raw sockaddr_in fields, network byte order). */
typedef struct {
    uint32_t ip;
    uint16_t port;
} bmc_peer_t;

/* Called from the net task for every non-HELLO datagram sent by a current
 * subscriber. Must not block: copy the line into a queue and return. */
typedef void (*bmc_stream_cmd_cb_t)(const char *line, size_t len, bmc_peer_t from);

void bmc_stream_set_command_handler(bmc_stream_cmd_cb_t cb);
esp_err_t bmc_stream_start(int core, UBaseType_t priority);

/* Safe to call from any task. Non-blocking; the reply is simply lost if the socket
 * is not ready (the host retries commands). */
void bmc_stream_reply(bmc_peer_t to, const char *text, size_t len);

/* Queues one complete event line ("EVENT,...\n") for every subscriber. It is sent
 * three times, 100 ms apart, because UDP can lose it; hosts de-duplicate on the
 * event id. Returns false if the queue is full. Safe from any task. */
bool bmc_stream_post_event(const char *line, size_t len);
