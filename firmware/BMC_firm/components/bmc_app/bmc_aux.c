#include "bmc_app.h"

#include <stdio.h>
#include "bmc_fan_link.h"
#include "bmc_i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1306.h"

/* Slow, optional I2C traffic (OLED redraw, the fan-duty peer used while the sensors
 * are emulated). It runs at low priority in its own task so a missing or slow
 * peripheral can never delay a sensor read or a safety decision. */

static const char *TAG = "aux";

#define SSD1306_I2C_ADDR    0x3C
#define FAN_LINK_I2C_ADDR   0x44     /* peer receiving the commanded fan demand; remove once the sensors are real */
#define AUX_PERIOD_MS       100
#define OLED_EVERY_N        10       /* 1 Hz */
#define LINK_RESEND_MS      500

static ssd1306_t s_oled;

static void aux_task(void *arg)
{
    bool oled_ok = (ssd1306_init(bmc_i2c_bus(), SSD1306_I2C_ADDR, &s_oled) == ESP_OK);
    if (!oled_ok) {
        ESP_LOGW(TAG, "ssd1306_init failed, continuing without display");
    }
    bmc_fan_link_t link;
    bool link_inited = (bmc_fan_link_init(bmc_i2c_bus(), FAN_LINK_I2C_ADDR, &link) == ESP_OK);
    bool link_ok = true;                 /* so the first failed send logs a warning */
    int last_sent = -1;
    int64_t last_sent_us = 0;
    bool oled_fault = false;
    unsigned iter = 0;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        app_status_t as;
        app_status_get(&as);
        sensor_snapshot_t sn;
        sensors_get(&sn);

        if (link_inited) {
            int demand = (int)((as.fan_enabled ? as.st.fan_demand_pct : 0.0f) + 0.5f);
            int64_t now = esp_timer_get_time();
            if (demand != last_sent || now - last_sent_us >= LINK_RESEND_MS * 1000LL) {
                esp_err_t err = bmc_fan_link_send(&link, (float)demand);
                if (err != ESP_OK && link_ok) {
                    ESP_LOGW(TAG, "fan link 0x%02x send failed: %s", FAN_LINK_I2C_ADDR, esp_err_to_name(err));
                } else if (err == ESP_OK && !link_ok) {
                    ESP_LOGI(TAG, "fan link 0x%02x up", FAN_LINK_I2C_ADDR);
                }
                link_ok = (err == ESP_OK);
                last_sent = demand;
                last_sent_us = now;
            }
        }

        if (oled_ok && (iter % OLED_EVERY_N) == 0) {
            char line[16];
            ssd1306_clear(&s_oled);
            if (sn.plate.valid) {
                /* Panel cannot show 3 integer digits at this scale: cap what is drawn only. */
                float shown = sn.plate_c > 99.9f ? 99.9f : sn.plate_c;
                snprintf(line, sizeof(line), "%.1fC", shown);
            } else {
                snprintf(line, sizeof(line), "--.-C");
            }
            ssd1306_draw_text(&s_oled, 0, 0, 4, line);

            if (as.st.mode == BMC_MODE_FAILSAFE) {
                ssd1306_draw_text(&s_oled, 0, 4, 2, "FAILSAFE");
                snprintf(line, sizeof(line), "%s", bmc_fault_name((bmc_fault_t)as.st.fault_code));
                ssd1306_draw_text(&s_oled, 0, 6, 1, line);
            } else {
                if (as.fan_enabled) {
                    snprintf(line, sizeof(line), "FAN %.0f%%", as.st.fan_demand_pct);
                } else {
                    snprintf(line, sizeof(line), "DISABLED");
                }
                ssd1306_draw_text(&s_oled, 0, 4, 2, line);
                if (sn.ina.valid) {
                    snprintf(line, sizeof(line), "%.2fW", sn.power_w);
                    ssd1306_draw_text(&s_oled, 0, 6, 2, line);
                }
            }
            esp_err_t err = ssd1306_flush(&s_oled);
            if (err != ESP_OK && !oled_fault) {
                ESP_LOGW(TAG, "ssd1306_flush failed: %s", esp_err_to_name(err));
            }
            oled_fault = (err != ESP_OK);
        }
        iter++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(AUX_PERIOD_MS));
    }
}

esp_err_t aux_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(aux_task, "aux", 4096, NULL, 2, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
