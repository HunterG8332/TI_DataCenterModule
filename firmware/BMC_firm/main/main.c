#include <string.h>

#include "bmc_board.h"
#include "bmc_i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tmp117.h"


static const char *TAG = "bmc_firm";

#define TMP117_I2C_ADDR      0x48     /* ADD0 -> GND */
#define TMP117_TASK_PRIORITY 4
#define TMP117_TASK_CORE     1
#define TMP117_PERIOD_MS     1000

#define HOLD_TASK_PRIORITY   1
#define HOLD_TASK_CORE        0

/*one emulated TMP117 on I2C, read the temperature register at 1 Hz. */

static void sensor_task(void *arg)
{
    tmp117_t tmp117;
    esp_err_t err = tmp117_init(bmc_i2c_bus(), TMP117_I2C_ADDR, &tmp117);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tmp117_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "tmp117 found at 0x%02x on core %d", TMP117_I2C_ADDR, xPortGetCoreID());

    while (1) {
        float temp_c;
        err = tmp117_read_c(&tmp117, &temp_c);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "tmp117: %.4f C", temp_c);
        } else {
            ESP_LOGW(TAG, "tmp117 read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(TMP117_PERIOD_MS));
    }
}

static void hold_task(void *arg)
{
    ESP_LOGI(TAG, "Indefinately holding Core %d", xPortGetCoreID());

    while (1) {
        vTaskDelay(500);
        ESP_LOGI(TAG, "Still holding Core %d", xPortGetCoreID());
    }

}

void app_main(void)
{
    board_init();

    ESP_LOGI(TAG, "BMC bring-up, stage 3: TMP117 over I2C");
    ESP_ERROR_CHECK(bmc_i2c_init());
    xTaskCreatePinnedToCore(sensor_task, "tmp117", 4096, NULL,
                             TMP117_TASK_PRIORITY, NULL, TMP117_TASK_CORE);

    xTaskCreatePinnedToCore(hold_task, "hold", 4096, NULL,
                             HOLD_TASK_PRIORITY, NULL, HOLD_TASK_CORE);
}
