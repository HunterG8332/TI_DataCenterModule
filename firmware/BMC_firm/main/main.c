#include <stdbool.h>
#include <stdio.h>

#include "bmc_board.h"
#include "bmc_fan.h"
#include "bmc_fan_link.h"
#include "bmc_i2c.h"
#include "bmc_net.h"
#include "bmc_stream.h"
#include "bmc_telemetry.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#define SAMPLE_PERIOD_MS     100      /* 10 Hz telemetry */
#define OLED_EVERY_N_SAMPLES 10       /* OLED redraw at 1 Hz so it cannot stretch the 100 ms period */
#define LOG_EVERY_N_SAMPLES  10

/* Core 0 carries Wi-Fi/lwIP and everything that can block on a socket. */
#define NET_TASK_PRIORITY    5
#define NET_TASK_CORE        0

#define FAN_LEDC_TIMER    LEDC_TIMER_0
#define FAN_LEDC_CHANNEL  LEDC_CHANNEL_0
#define FAN_CONTROL_PERIOD_MS 50  /* also bounds how quickly the enable interlock
                                        (checked fresh on every bmc_fan_set_duty_pct
                                        call) takes effect */
#define FAN_TACH_WINDOW_MS 250   /* sliding window: 120 RPM resolution (2 edges/rev) */
/* Set to 1 to run the duty-sweep validation once at boot. */
#define FAN_SWEEP_TEST          1
#define FAN_SWEEP_STEP_PCT      10
#define FAN_SWEEP_SETTLE_MS     4000  /* spin-up/settle portion of each step */
#define FAN_SWEEP_SAMPLES       10    /* steady-state samples (at 10 Hz) averaged per step */
#define FAN_SWEEP_SAMPLE_MS     100
#define FAN_SWEEP_MONO_TOL_RPM  120   /* one tach quantum (250 ms window, 2 edges/rev) */

#define FAN_STALL_RECOVER_CYCLES 5    /* consecutive nonzero-RPM cycles to clear a stall */
#define FAN_STALL_MIN_DUTY_PCT 10.0f  /* below this the fan may legitimately idle/stop */
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
static volatile uint32_t s_fan_rpm = 0;
static volatile bool s_fan_stalled = false;  /* set on stall, cleared when tach returns; any heater enable must check this */

/* Set >= 0 to override the temperature curve with a fixed duty (used by the
 * sweep test). Negative = normal curve control. */
static volatile float s_fan_duty_override = -1.0f;

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

    TickType_t last_wake = xTaskGetTickCount();
    unsigned iter = 0;
    while (1) {
        bool log_now = (iter % LOG_EVERY_N_SAMPLES) == 0;

        float power_w = 0.0f;
        bool power_valid = false;
        if (ina_ok) {
            esp_err_t perr = ina238_read_power_w(&ina238, &power_w);
            if (perr == ESP_OK) {
                power_valid = true;
                if (log_now) {
                    ESP_LOGI(TAG, "ina238: %.3f W", power_w);
                }
            } else {
                ESP_LOGW(TAG, "ina238 read failed: %s", esp_err_to_name(perr));
            }
        }

        float temp_c = 0.0f;
        bool temp_valid = false;
        err = tmp117_read_c(&tmp117, &temp_c);
        if (err == ESP_OK) {
            temp_valid = true;
            if (log_now) {
                ESP_LOGI(TAG, "tmp117: %.4f C", temp_c);
            }
            s_temp_c = temp_c;  /* raw value, published before the display-only clamp below */
        } else {
            ESP_LOGW(TAG, "tmp117 read failed: %s", esp_err_to_name(err));
        }

        telemetry_sample_t sample = {
            .temp_c = temp_c,
            .temp_valid = temp_valid,
            .power_w = power_w,
            .power_valid = power_valid,
            .fan_duty_pct = s_fan_duty_pct,
            .fan_enabled = s_fan_enabled,
            .fan_rpm = s_fan_rpm,
        };
        telemetry_publish(&sample);   /* non-blocking; never waits on the network */

        if (oled_ok && temp_valid && (iter % OLED_EVERY_N_SAMPLES) == 0) {
            char line[16];
            /* Cosmetic display clamp only -- s_temp_c above already carries
             * the true value for the fan curve. Panel can't show 3 integer
             * digits at this font scale, so cap what's rendered here. */
            float shown_c = temp_c > 99.9f ? 99.9f : temp_c;
            snprintf(line, sizeof(line), "%.1fC", shown_c);
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

        iter++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
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
    TickType_t above_floor_since = 0;
    unsigned recover_cycles = 0;
    board_heater_enable_set(false);  /* heater is off until explicitly enabled */

    while (1) {
        float temp_c = s_temp_c;
        float duty = fan_curve_duty_pct(temp_c);
        float override_duty = s_fan_duty_override;
        if (override_duty >= 0.0f) {
            duty = override_duty;
        }

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
        bool window_full = false;
        err = bmc_fan_read_rpm(&fan, FAN_TACH_WINDOW_MS, &rpm, &window_full);
        if (err == ESP_OK) {
            s_fan_rpm = rpm;

            /* Stall: fan permitted and commanded above the floor for at least
             * one full tach window, yet zero edges in that window. Trips on
             * the first such window and latches until reboot. */
            TickType_t now = xTaskGetTickCount();
            if (s_fan_enabled && duty >= FAN_STALL_MIN_DUTY_PCT) {
                if (above_floor_since == 0) {
                    above_floor_since = now ? now : 1;
                }
            } else {
                above_floor_since = 0;
            }
            bool armed = above_floor_since != 0 &&
                         (now - above_floor_since) >= pdMS_TO_TICKS(FAN_TACH_WINDOW_MS);
            if (!s_fan_stalled && armed && window_full && rpm == 0) {
                s_fan_stalled = true;
                board_heater_enable_set(false);
                ESP_LOGE(TAG, "FAN STALL: duty=%.0f%% rpm=0 -- heater disabled", duty);
            }

            /* Recovery: tach edges seen again for FAN_STALL_RECOVER_CYCLES
             * consecutive cycles (~250 ms) clears the stall. The heater is not
             * re-enabled here; whatever commands it must do so explicitly. */
            if (s_fan_stalled) {
                recover_cycles = (window_full && rpm > 0) ? recover_cycles + 1 : 0;
                if (recover_cycles >= FAN_STALL_RECOVER_CYCLES) {
                    s_fan_stalled = false;
                    recover_cycles = 0;
                    above_floor_since = 0;
                    ESP_LOGI(TAG, "fan tach recovered: rpm=%lu -- stall cleared",
                             (unsigned long)rpm);
                }
            }
        } else {
            ESP_LOGW(TAG, "bmc_fan_read_rpm failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(FAN_CONTROL_PERIOD_MS));
    }
}

#if FAN_SWEEP_TEST
/* Validation: sweep commanded duty 0..100% in 10% steps, record the measured
 * RPM at each step, and check the response is monotonic. Runs once at boot,
 * then returns the fan to normal curve control.
 *
 * Only one fan channel exists on this board, so "reports independently" can
 * not be exercised here; the results table is per-fan-ready (one column per
 * fan) once a second PWM/tach pair is added. */
static void fan_sweep_test_task(void *arg)
{
    enum { STEPS = 11 };
    const int settle_samples = FAN_SWEEP_SETTLE_MS / FAN_SWEEP_SAMPLE_MS;
    uint32_t rpm_avg[STEPS];

    vTaskDelay(pdMS_TO_TICKS(3000));  /* let fan_task init */
    ESP_LOGW(TAG, "SWEEP: starting fan duty sweep (curve control suspended)");

    /* Every 100 ms (10 Hz) for the whole sweep, including spin-up transients,
     * emit one CSV row live. printf, so rows are not log-prefixed; the host
     * script (tools/sweep_to_csv.py) collects CSV_BEGIN..CSV_END. "steady" is 1
     * for the last FAN_SWEEP_SAMPLES samples of each step, which are averaged
     * for the monotonic check. */
    printf("CSV_BEGIN\nt_ms,commanded_duty_pct,measured_rpm,steady\n");
    int64_t t0_us = esp_timer_get_time();
    TickType_t last_wake = xTaskGetTickCount();

    for (int i = 0; i < STEPS; i++) {
        s_fan_duty_override = (float)(i * FAN_SWEEP_STEP_PCT);
        uint64_t sum = 0;
        for (int n = 0; n < settle_samples + FAN_SWEEP_SAMPLES; n++) {
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FAN_SWEEP_SAMPLE_MS));
            uint32_t r = s_fan_rpm;
            bool steady = n >= settle_samples;
            if (steady) {
                sum += r;
            }
            printf("%lld,%d,%lu,%d\n", (long long)((esp_timer_get_time() - t0_us) / 1000),
                   i * FAN_SWEEP_STEP_PCT, (unsigned long)r, steady ? 1 : 0);
        }
        rpm_avg[i] = (uint32_t)(sum / FAN_SWEEP_SAMPLES);
    }
    s_fan_duty_override = -1.0f;
    printf("CSV_END\n");

    for (int i = 0; i < STEPS; i++) {
        ESP_LOGI(TAG, "SWEEP: duty=%3d%% rpm=%lu", i * FAN_SWEEP_STEP_PCT,
                 (unsigned long)rpm_avg[i]);
    }
    if (s_fan_stalled) {
        ESP_LOGW(TAG, "SWEEP: stall latched during sweep, results suspect");
    }

    bool mono = true;
    for (int i = 1; i < STEPS; i++) {
        if (rpm_avg[i] + FAN_SWEEP_MONO_TOL_RPM < rpm_avg[i - 1]) {
            mono = false;
            ESP_LOGE(TAG, "SWEEP: non-monotonic at %d%%: %lu -> %lu RPM", i * FAN_SWEEP_STEP_PCT,
                     (unsigned long)rpm_avg[i - 1], (unsigned long)rpm_avg[i]);
        }
    }
    bool responds = rpm_avg[STEPS - 1] > rpm_avg[0] + FAN_SWEEP_MONO_TOL_RPM;

    ESP_LOGW(TAG, "SWEEP: monotonic (tol %d RPM): %s | 100%% > 0%%: %s | independence: N/A (1 fan)",
             FAN_SWEEP_MONO_TOL_RPM, mono ? "PASS" : "FAIL", responds ? "PASS" : "FAIL");
    ESP_LOGW(TAG, "SWEEP: done, curve control restored");
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    board_init();

    ESP_ERROR_CHECK(bmc_i2c_init());
    telemetry_init();
    xTaskCreatePinnedToCore(sensor_task, "tmp117", 8192, NULL,
                             TMP117_TASK_PRIORITY, NULL, TMP117_TASK_CORE);

    /* The BMC must keep controlling with no network, so a link failure is logged, not fatal. */
    esp_err_t net_err = bmc_net_init();
    if (net_err == ESP_OK) {
        net_err = bmc_stream_start(NET_TASK_CORE, NET_TASK_PRIORITY);
    }
    if (net_err != ESP_OK) {
        ESP_LOGE(TAG, "network unavailable: %s", esp_err_to_name(net_err));
    }

    xTaskCreatePinnedToCore(fan_task, "fan", 4096, NULL,
                             FAN_TASK_PRIORITY, NULL, FAN_TASK_CORE);

#if FAN_SWEEP_TEST
    xTaskCreatePinnedToCore(fan_sweep_test_task, "fan_sweep", 4096, NULL,
                             FAN_TASK_PRIORITY - 1, NULL, NET_TASK_CORE);
#endif
}
