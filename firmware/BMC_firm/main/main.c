#include <stdio.h>

#include "bmc_app.h"
#include "bmc_board.h"
#include "bmc_i2c.h"
#include "bmc_net.h"
#include "bmc_stream.h"
#include "bmc_telemetry.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

/* main only brings things up, in a deliberate order. All behaviour lives in
 * components/bmc_core (decisions, host-testable) and components/bmc_app
 * (hardware glue). See docs/bmc_plan/01_architecture.md.
 *
 *   core 1: supervisor (prio 10, 50 ms)  safety + fan loops + the only actuator writer
 *           sensors    (prio  8, 100 ms) I2C devices, failure counting, telemetry
 *           aux        (prio  2)         OLED and the optional fan-link peer
 *   core 0: Wi-Fi/lwIP and net_tx (prio 5): anything that can block on a socket */

static const char *TAG = "bmc_firm";

#define NET_TASK_PRIORITY 5
#define NET_TASK_CORE     0

/* The supervisor and sensor tasks subscribe themselves; if either stops running for
 * this long the chip resets, which lands in STANDBY with the heater off. (The idle
 * tasks are not watched: Wi-Fi load must not be able to reset the controller.) */
#define CONTROL_WDT_TIMEOUT_MS 1000

void app_main(void)
{
    board_init();
    ESP_ERROR_CHECK(bmc_i2c_init());
    /* Heater off and fans set up before any task that could command them exists. */
    ESP_ERROR_CHECK(act_init());
    telemetry_init();

    const esp_task_wdt_config_t wdt = {
        .timeout_ms = CONTROL_WDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&wdt));

    ESP_ERROR_CHECK(sensors_start());
    ESP_ERROR_CHECK(supervisor_start());    /* registers the command handler before the stream starts */

    /* The BMC must keep controlling with no network, so a link failure is logged, not fatal. */
    esp_err_t net_err = bmc_net_init();
    if (net_err == ESP_OK) {
        net_err = bmc_stream_start(NET_TASK_CORE, NET_TASK_PRIORITY);
    }
    if (net_err != ESP_OK) {
        ESP_LOGE(TAG, "network unavailable: %s", esp_err_to_name(net_err));
    }

    ESP_ERROR_CHECK(aux_start());
    ESP_LOGI(TAG, "BMC up: STANDBY, heater off (self-test required before any heat)");
}
