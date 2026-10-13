#pragma once

#include "esp_err.h"
#include "driver/ledc.h"

/* Demanded heater power -> emulator, as an analog level. One LEDC channel
 * emits high-frequency PWM; an external RC low-pass (R = 10k, C = 1 uF,
 * fc ~ 16 Hz, ~50 ms settling) turns it into a DC level of 0..3.3 V, where
 * 0 V = 0% and 3.3 V = 100% of full-scale demanded power. The emulator input
 * must load the filter lightly (>> 10k) or sit behind a buffer, and should
 * have a pull-down so the line reads 0% while the BMC pin is hi-Z at boot. */

esp_err_t bmc_heater_cmd_init(int pwm_gpio, ledc_timer_t ledc_timer,
                              ledc_channel_t ledc_channel);

/* Clamped to [0, 100]. Output is 0% until first called. */
esp_err_t bmc_heater_cmd_set_pct(float power_pct);
