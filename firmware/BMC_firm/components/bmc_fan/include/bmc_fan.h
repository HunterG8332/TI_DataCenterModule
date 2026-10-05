#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"

/* PWM + tach control for a 4-wire fan (target 25 kHz PWM; matches the
 * ARCTIC S8038-7K's spec sheet: 25 kHz target, 21-28 kHz acceptable range).
 * One LEDC channel drives PWM; one PCNT unit counts tach edges over a
 * caller-supplied gate window to derive RPM. Assumes 2 tach pulses per
 * revolution, standard for 4-wire PC/industrial fans (Intel 4-wire PWM
 * spec) -- adjust BMC_FAN_PULSES_PER_REV in bmc_fan.c if this fan differs.
 *
 * Safety interlock: enable_gpio is read (active-low, internal pull-up) on
 * every bmc_fan_set_duty_pct() call. If it is not pulled low, the fan is
 * forced to 0% regardless of the requested duty -- this is a hardware-level
 * "never spin" gate, not just a firmware default, since some 4-wire fans
 * still idle-spin at 0% commanded duty rather than fully stopping. */

typedef struct {
    ledc_channel_t ledc_channel;
    pcnt_unit_handle_t pcnt_unit;
    pcnt_channel_handle_t pcnt_chan;
    int enable_gpio;
} bmc_fan_t;

esp_err_t bmc_fan_init(bmc_fan_t *fan, int pwm_gpio, int tach_gpio, int enable_gpio,
                        ledc_timer_t ledc_timer, ledc_channel_t ledc_channel);

/* True when enable_gpio is currently pulled low (fan permitted to spin). */
bool bmc_fan_is_enabled(const bmc_fan_t *fan);

/* Clamped to [0, 100]. Forced to 0 regardless of the request if the enable
 * interlock (see above) is not satisfied. No minimum/stall floor is
 * enforced here -- the fan idle-spins on its own well below any duty we'd
 * pick as a floor, so a firmware-side floor added nothing but a plateau in
 * the commanded curve. */
esp_err_t bmc_fan_set_duty_pct(bmc_fan_t *fan, float duty_pct);

/* Blocks the calling task for gate_ms while counting tach edges, then
 * returns the computed RPM. Not for use inside a hard-real-time loop. */
esp_err_t bmc_fan_read_rpm(bmc_fan_t *fan, uint32_t gate_ms, uint32_t *out_rpm);
