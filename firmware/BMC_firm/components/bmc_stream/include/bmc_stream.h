#pragma once

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

/* Streams telemetry samples as CSV over UDP to every host that has said
 * HELLO. Uses only BSD sockets, so it runs unchanged over Wi-Fi or Ethernet.
 * Wire format: dashboard/README.md. */

esp_err_t bmc_stream_start(int core, UBaseType_t priority);
