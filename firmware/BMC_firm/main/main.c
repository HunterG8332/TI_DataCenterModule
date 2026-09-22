#include "bmc_board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Stage 1: first component. Blink moved out of main and into
 * components/bmc_board/ behind board_init()/board_led_set() so main
 * carries no pin numbers. */

static const char *TAG = "bmc_firm";

void app_main(void)
{
    board_init();
    ESP_LOGI(TAG, "BMC bring-up, stage 1: blink via bmc_board component");

    bool led_on = false;
    while (1) {
        led_on = !led_on;
        board_led_set(led_on);
        ESP_LOGI(TAG, "led %s", led_on ? "on" : "off");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
