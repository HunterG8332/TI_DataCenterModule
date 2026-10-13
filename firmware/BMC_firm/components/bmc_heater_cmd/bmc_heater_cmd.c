#include "bmc_heater_cmd.h"

#define HEATER_CMD_PWM_FREQ_HZ 40000
#define HEATER_CMD_LEDC_MODE   LEDC_LOW_SPEED_MODE
/* 80e6/40e3 = 2000, so 10 bits (1024) is the largest resolution that fits:
 * ~3.2 mV steps after the filter, well inside the ~1% requirement. The high
 * carrier keeps ripple through the RC filter around a millivolt. */
#define HEATER_CMD_LEDC_RES    LEDC_TIMER_10_BIT
#define HEATER_CMD_DUTY_MAX    ((1u << 10) - 1)

static ledc_channel_t s_channel;

esp_err_t bmc_heater_cmd_init(int pwm_gpio, ledc_timer_t ledc_timer,
                              ledc_channel_t ledc_channel)
{
    s_channel = ledc_channel;

    ledc_timer_config_t timer_cfg = {
        .speed_mode = HEATER_CMD_LEDC_MODE,
        .duty_resolution = HEATER_CMD_LEDC_RES,
        .timer_num = ledc_timer,
        .freq_hz = HEATER_CMD_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channel_cfg = {
        .gpio_num = pwm_gpio,
        .speed_mode = HEATER_CMD_LEDC_MODE,
        .channel = ledc_channel,
        .timer_sel = ledc_timer,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel_cfg);
}

esp_err_t bmc_heater_cmd_set_pct(float power_pct)
{
    if (power_pct < 0.0f) {
        power_pct = 0.0f;
    } else if (power_pct > 100.0f) {
        power_pct = 100.0f;
    }

    uint32_t duty = (uint32_t)((power_pct / 100.0f) * HEATER_CMD_DUTY_MAX + 0.5f);
    esp_err_t err = ledc_set_duty(HEATER_CMD_LEDC_MODE, s_channel, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(HEATER_CMD_LEDC_MODE, s_channel);
}
