#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Stage 0 bring-up on the ESP32-WROOM-32E dev kit. Purpose is to prove the
 * toolchain (set-target / build / flash / monitor), not the BMC design. */

static const char *TAG = "hello_world";

void app_main(void)
{
    /* Per-tag log level: this tag runs at DEBUG regardless of the global
     * default set in menuconfig. */
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "BMC bring-up, stage 0: hello world");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "chip: %d core(s), silicon revision v%d.%d",
             chip_info.cores, chip_info.revision / 100, chip_info.revision % 100);

    int count = 0;
    while (1) {
        ESP_LOGD(TAG, "debug detail, tick %d", count);
        ESP_LOGI(TAG, "tick %d", count);

        if (count % 5 == 0) {
            ESP_LOGW(TAG, "warning example at tick %d (expected, not a fault)", count);
        }
        if (count % 10 == 0 && count != 0) {
            ESP_LOGE(TAG, "error example at tick %d (expected, not a fault)", count);
        }

        count++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
