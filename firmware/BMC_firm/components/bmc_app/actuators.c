#include "bmc_app.h"

#include "bmc_board.h"
#include "bmc_heater_cmd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "actuators";

#define FAN_LEDC_TIMER          LEDC_TIMER_0       /* both fans share the 25 kHz timer */
#define HEATER_CMD_LEDC_TIMER   LEDC_TIMER_1
#define HEATER_CMD_LEDC_CHANNEL LEDC_CHANNEL_1
/* fan i uses LEDC channel 0 (fan 1) and 2 (fan 2); channel 1 is the heater command */

static bmc_fan_t s_fan[BMC_FAN_COUNT];
static TaskHandle_t s_owner;
static bool s_fan_fault[BMC_FAN_COUNT];

esp_err_t act_init(void)
{
    /* Heater first, so nothing below can leave it in an unknown state. */
    esp_err_t err = bmc_heater_cmd_init(board_heater_cmd_gpio(), HEATER_CMD_LEDC_TIMER, HEATER_CMD_LEDC_CHANNEL);
    if (err != ESP_OK) {
        return err;
    }
    bmc_heater_cmd_set_pct(0.0f);

    for (int i = 0; i < BMC_FAN_COUNT; i++) {
        ledc_channel_t ch = (i == 0) ? LEDC_CHANNEL_0 : LEDC_CHANNEL_2;
        err = bmc_fan_init(&s_fan[i], board_fan_pwm_gpio(i), board_fan_tach_gpio(i),
                           board_fan_enable_gpio(), FAN_LEDC_TIMER, ch);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fan %d init failed: %s", i + 1, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "fan %d pwm=gpio%d tach=gpio%d enable=gpio%d", i + 1, board_fan_pwm_gpio(i),
                 board_fan_tach_gpio(i), board_fan_enable_gpio());
    }
    return ESP_OK;
}

bmc_fan_t *act_fan(int index)
{
    return &s_fan[index];
}

void act_apply(const act_cmd_t *c)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (s_owner == NULL) {
        s_owner = me;
    } else if (s_owner != me) {
        ESP_LOGE(TAG, "refused: act_apply called from a second task (single-writer rule)");
        return;
    }

    /* Heater: the command level is the only control; zero means off. */
    bmc_heater_cmd_set_pct(c->heater_enable ? c->heater_pct : 0.0f);

    for (int i = 0; i < BMC_FAN_COUNT; i++) {
        esp_err_t err = bmc_fan_set_duty_pct(&s_fan[i], c->fan_duty[i]);
        if (err != ESP_OK && !s_fan_fault[i]) {
            ESP_LOGW(TAG, "fan %d set duty failed: %s", i + 1, esp_err_to_name(err));
        }
        s_fan_fault[i] = (err != ESP_OK);
    }
}
