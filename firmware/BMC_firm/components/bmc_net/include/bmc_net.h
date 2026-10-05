#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* The BMC's network link. This component is the only place that knows what
 * the link is: today a Wi-Fi SoftAP ("BMC-Telemetry", BMC at 192.168.4.1);
 * the ESP32-S3-ETH port swaps bmc_net_ap.c for a W5500/esp_eth
 * implementation of the same two calls. Everything above it uses plain
 * sockets and never includes esp_wifi.h. */

/* Brings the link up and returns; does not wait for clients. */
esp_err_t bmc_net_init(void);

/* True while the link can carry traffic. */
bool bmc_net_ready(void);
