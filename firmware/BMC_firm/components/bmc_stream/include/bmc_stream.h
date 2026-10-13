#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

/* Streams telemetry samples as CSV over UDP to every host that has said
 * HELLO. Uses only BSD sockets, so it runs unchanged over Wi-Fi or Ethernet.
 * Wire format: dashboard/README.md. */

esp_err_t bmc_stream_start(int core, UBaseType_t priority);

/* Heater request from the dashboard: a "HEATER <0|1> <pct>\n" datagram, resent
 * every 250 ms as a heartbeat. Returns true (and the percent, clamped to
 * 0-100) only if the latest datagram said ON and arrived within timeout_ms;
 * a lost link or closed dashboard therefore reads as "off". Safe to call from
 * any core. */
bool bmc_stream_heater_request(float *pct, uint32_t timeout_ms);
