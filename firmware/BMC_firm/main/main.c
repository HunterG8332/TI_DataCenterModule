#include <stdbool.h>
#include <stdio.h>

#include "bmc_board.h"
#include "bmc_fan.h"
#include "bmc_fan_link.h"
#include "bmc_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ina238.h"
#include "ssd1306.h"
#include "tmp117.h"


static const char *TAG = "bmc_firm";

#define TMP117_I2C_ADDR      0x48     /* ADD0 -> GND */
#define SSD1306_I2C_ADDR     0x3C
#define INA238_I2C_ADDR      0x40     /* A1, A0 -> GND */
#define FAN_LINK_I2C_ADDR    0x44     /* peer receiving the commanded fan duty */
#define TMP117_TASK_PRIORITY 8
#define TMP117_TASK_CORE     1
#define TMP117_PERIOD_MS     1000

#define HOLD_TASK_PRIORITY   1
#define HOLD_TASK_CORE       0

#define FAN_LEDC_TIMER    LEDC_TIMER_0
#define FAN_LEDC_CHANNEL  LEDC_CHANNEL_0
#define FAN_CONTROL_PERIOD_MS 50  /* also bounds how quickly the enable interlock
                                        (checked fresh on every bmc_fan_set_duty_pct
                                        call) takes effect */
#define FAN_TACH_GATE_MS  25
#define FAN_TASK_PRIORITY 3
#define FAN_TASK_CORE     1

/* Linear fan curve: 0% at FAN_CURVE_LOW_C, 100% at FAN_CURVE_HIGH_C (the
 * operating ceiling), clamped at both ends. */
#define FAN_CURVE_LOW_C   25.0f
#define FAN_CURVE_HIGH_C  100.0f

/* Shared between sensor_task (writer of temp, reader of fan state for the
 * OLED) and fan_task (writer of fan state, reader of temp for the curve).
 * Both tasks run on core 1; a stale read for one refresh/control period is
 * cosmetic at worst -- the enable interlock itself is enforced live, inside
 * bmc_fan_set_duty_pct(), independent of these. */
static volatile float s_temp_c = 0.0f;
static volatile float s_fan_duty_pct = 0.0f;
static volatile bool s_fan_enabled = false;

static float fan_curve_duty_pct(float temp_c)
{
    float duty = (temp_c - FAN_CURVE_LOW_C) * 100.0f / (FAN_CURVE_HIGH_C - FAN_CURVE_LOW_C);
    if (duty < 0.0f) {
        duty = 0.0f;
    } else if (duty > 100.0f) {
        duty = 100.0f;
    }
    return duty;
}

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

    ssd1306_t oled;
    bool oled_ok = (ssd1306_init(bmc_i2c_bus(), SSD1306_I2C_ADDR, &oled) == ESP_OK);
    if (!oled_ok) {
        ESP_LOGW(TAG, "ssd1306_init failed, continuing without display");
    }

    ina238_t ina238;
    esp_err_t ina_err = ina238_init(bmc_i2c_bus(), INA238_I2C_ADDR, &ina238);
    bool ina_ok = (ina_err == ESP_OK);
    if (!ina_ok) {
        ESP_LOGW(TAG, "ina238_init failed at 0x%02x: %s (0x%x), continuing without power reading",
                 INA238_I2C_ADDR, esp_err_to_name(ina_err), ina_err);
    }

    while (1) {
        float power_w = 0.0f;
        bool power_valid = false;
        if (ina_ok) {
            esp_err_t perr = ina238_read_power_w(&ina238, &power_w);
            if (perr == ESP_OK) {
                power_valid = true;
                ESP_LOGI(TAG, "ina238: %.3f W", power_w);
            } else {
                ESP_LOGW(TAG, "ina238 read failed: %s", esp_err_to_name(perr));
            }
        }

        float temp_c;
        err = tmp117_read_c(&tmp117, &temp_c);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "tmp117: %.4f C", temp_c);
            s_temp_c = temp_c;  /* raw value, published before the display-only clamp below */
            if (oled_ok) {
                char line[16];
                /* Cosmetic display clamp only -- s_temp_c above already carries
                 * the true value for the fan curve. Panel can't show 3 integer
                 * digits at this font scale, so cap what's rendered here. */
                if (temp_c > 99.9f) {
                    temp_c = 99.9f;
                }
                snprintf(line, sizeof(line), "%.1fC", temp_c);
                ssd1306_clear(&oled);
                ssd1306_draw_text(&oled, 0, 0, 4, line);

                char fan_line[16];
                if (s_fan_enabled) {
                    snprintf(fan_line, sizeof(fan_line), "FAN %.0f%%", s_fan_duty_pct);
                } else {
                    snprintf(fan_line, sizeof(fan_line), "DISABLED");
                }
                ssd1306_draw_text(&oled, 0, 4, 2, fan_line);

                if (power_valid) {
                    char pwr_line[16];
                    snprintf(pwr_line, sizeof(pwr_line), "%.2fW", power_w);
                    ssd1306_draw_text(&oled, 0, 6, 2, pwr_line);
                }

                if (ssd1306_flush(&oled) != ESP_OK) {
                    ESP_LOGW(TAG, "ssd1306_flush failed");
                }
            }
        } else {
            ESP_LOGW(TAG, "tmp117 read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(TMP117_PERIOD_MS));
    }
}

/* Closed-loop control on one ARCTIC S8038-7K fan: duty is a linear function
 * of the TMP117 reading (see fan_curve_duty_pct), recomputed and re-applied
 * every FAN_CONTROL_PERIOD_MS. No discrete steps -- duty tracks temperature
 * continuously. */

static void fan_task(void *arg)
{
    bmc_fan_t fan;
    esp_err_t err = bmc_fan_init(&fan, board_fan_pwm_gpio(), board_fan_tach_gpio(),
                                  board_fan_enable_gpio(), FAN_LEDC_TIMER, FAN_LEDC_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bmc_fan_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "fan pwm=gpio%d tach=gpio%d enable=gpio%d on core %d",
             board_fan_pwm_gpio(), board_fan_tach_gpio(), board_fan_enable_gpio(),
             xPortGetCoreID());

    bmc_fan_link_t fan_link;
    err = bmc_fan_link_init(bmc_i2c_bus(), FAN_LINK_I2C_ADDR, &fan_link);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bmc_fan_link_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    bool link_ok = true;  /* so the first failed send logs a warning */

    while (1) {
        float temp_c = s_temp_c;
        float duty = fan_curve_duty_pct(temp_c);

        err = bmc_fan_set_duty_pct(&fan, duty);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bmc_fan_set_duty_pct failed: %s", esp_err_to_name(err));
        }
        s_fan_duty_pct = duty;
        s_fan_enabled = bmc_fan_is_enabled(&fan);

        /* Report 0% while the interlock has the fan disabled. Log only on
         * link up/down transitions so a missing peer doesn't spam every 50 ms. */
        err = bmc_fan_link_send(&fan_link, s_fan_enabled ? duty : 0.0f);
        if (err != ESP_OK && link_ok) {
            ESP_LOGW(TAG, "fan link 0x%02x send failed: %s", FAN_LINK_I2C_ADDR,
                     esp_err_to_name(err));
        } else if (err == ESP_OK && !link_ok) {
            ESP_LOGI(TAG, "fan link 0x%02x up", FAN_LINK_I2C_ADDR);
        }
        link_ok = (err == ESP_OK);

        uint32_t rpm = 0;
        err = bmc_fan_read_rpm(&fan, FAN_TACH_GATE_MS, &rpm);
        if (err == ESP_OK) {
            //ESP_LOGI(TAG, "fan: temp=%.1fC duty=%.0f%% enabled=%d rpm=%lu", temp_c, duty,s_fan_enabled, (unsigned long)rpm);
        } else {
            ESP_LOGW(TAG, "bmc_fan_read_rpm failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(FAN_CONTROL_PERIOD_MS));
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

    ESP_ERROR_CHECK(bmc_i2c_init());
    xTaskCreatePinnedToCore(sensor_task, "tmp117", 8192, NULL,
                             TMP117_TASK_PRIORITY, NULL, TMP117_TASK_CORE);

    xTaskCreatePinnedToCore(hold_task, "hold", 4096, NULL,
                             HOLD_TASK_PRIORITY, NULL, HOLD_TASK_CORE);

    xTaskCreatePinnedToCore(fan_task, "fan", 4096, NULL,
                             FAN_TASK_PRIORITY, NULL, FAN_TASK_CORE);
}
