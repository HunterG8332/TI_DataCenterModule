#include "bmc_fan.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BMC_FAN_PWM_FREQ_HZ     25000
#define BMC_FAN_LEDC_MODE       LEDC_LOW_SPEED_MODE
/* Max duty resolution at 25 kHz from an 80 MHz APB source: 80e6/25e3 = 3200,
 * and 2^11 = 2048 <= 3200 < 2^12 = 4096, so 11 bits is the largest resolution
 * that fits -- matches the design doc's fan PWM spec. */
#define BMC_FAN_LEDC_RES        LEDC_TIMER_11_BIT
#define BMC_FAN_DUTY_MAX        ((1u << 11) - 1)

#define BMC_FAN_PCNT_HIGH_LIMIT 32767
#define BMC_FAN_PCNT_LOW_LIMIT  (-1) /* PCNT requires low_limit < 0; edges only ever increase the count */
#define BMC_FAN_PULSES_PER_REV  2

esp_err_t bmc_fan_init(bmc_fan_t *fan, int pwm_gpio, int tach_gpio, int enable_gpio,
                        ledc_timer_t ledc_timer, ledc_channel_t ledc_channel)
{
    fan->ledc_channel = ledc_channel;
    fan->enable_gpio = enable_gpio;

    /* Active-low enable interlock. Internal pull-up so a floating/unwired
     * pin defaults to disabled (fail-safe), not just whatever the pin
     * happens to read. */
    gpio_set_direction(enable_gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(enable_gpio, GPIO_PULLUP_ONLY);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = BMC_FAN_LEDC_MODE,
        .duty_resolution = BMC_FAN_LEDC_RES,
        .timer_num = ledc_timer,
        .freq_hz = BMC_FAN_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channel_cfg = {
        .gpio_num = pwm_gpio,
        .speed_mode = BMC_FAN_LEDC_MODE,
        .channel = ledc_channel,
        .timer_sel = ledc_timer,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* Tach is open-collector; needs a pull-up. Internal weak pull-up as a
     * backup in case the board doesn't already have an external one. */
    gpio_set_direction(tach_gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(tach_gpio, GPIO_PULLUP_ONLY);

    pcnt_unit_config_t unit_cfg = {
        .clk_src = PCNT_CLK_SRC_DEFAULT,
        .low_limit = BMC_FAN_PCNT_LOW_LIMIT,
        .high_limit = BMC_FAN_PCNT_HIGH_LIMIT,
    };
    err = pcnt_new_unit(&unit_cfg, &fan->pcnt_unit);
    if (err != ESP_OK) {
        return err;
    }

    pcnt_glitch_filter_config_t filter_cfg = {
        .max_glitch_ns = 1000,
    };
    err = pcnt_unit_set_glitch_filter(fan->pcnt_unit, &filter_cfg);
    if (err != ESP_OK) {
        return err;
    }

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = tach_gpio,
        .level_gpio_num = -1,
    };
    err = pcnt_new_channel(fan->pcnt_unit, &chan_cfg, &fan->pcnt_chan);
    if (err != ESP_OK) {
        return err;
    }

    err = pcnt_channel_set_edge_action(fan->pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                        PCNT_CHANNEL_EDGE_ACTION_HOLD);
    if (err != ESP_OK) {
        return err;
    }

    err = pcnt_unit_enable(fan->pcnt_unit);
    if (err != ESP_OK) {
        return err;
    }
    err = pcnt_unit_clear_count(fan->pcnt_unit);
    if (err != ESP_OK) {
        return err;
    }
    return pcnt_unit_start(fan->pcnt_unit);
}

bool bmc_fan_is_enabled(const bmc_fan_t *fan)
{
    return gpio_get_level((gpio_num_t)fan->enable_gpio) == 0;
}

esp_err_t bmc_fan_set_duty_pct(bmc_fan_t *fan, float duty_pct)
{
    if (duty_pct < 0.0f) {
        duty_pct = 0.0f;
    } else if (duty_pct > 100.0f) {
        duty_pct = 100.0f;
    }

    if (!bmc_fan_is_enabled(fan)) {
        duty_pct = 0.0f;
    }

    uint32_t duty = (uint32_t)((duty_pct / 100.0f) * BMC_FAN_DUTY_MAX + 0.5f);
    esp_err_t err = ledc_set_duty(BMC_FAN_LEDC_MODE, fan->ledc_channel, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(BMC_FAN_LEDC_MODE, fan->ledc_channel);
}

esp_err_t bmc_fan_read_rpm(bmc_fan_t *fan, uint32_t gate_ms, uint32_t *out_rpm)
{
    esp_err_t err = pcnt_unit_clear_count(fan->pcnt_unit);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(gate_ms));

    int count = 0;
    err = pcnt_unit_get_count(fan->pcnt_unit, &count);
    if (err != ESP_OK) {
        return err;
    }

    *out_rpm = (uint32_t)(((uint64_t)count * 60000u) /
                           ((uint64_t)gate_ms * BMC_FAN_PULSES_PER_REV));
    return ESP_OK;
}
