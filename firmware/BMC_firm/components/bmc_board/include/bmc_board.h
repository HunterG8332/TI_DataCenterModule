#pragma once

#include <stdbool.h>
#include "hal/gpio_types.h"

/* Board abstraction for the BMC. This header is the only place the rest of the
 * firmware should learn about board wiring. Porting to a new target
 * (e.g. WROOM -> S3-ETH) means changing bmc_board.c/.h and nothing else. */

void board_init(void);

void board_led_set(bool on);

/* Hardware heater permit (active-low switch to ground, pull-up inside). False
 * when open/unwired, so the heater can never run without it. */
bool board_heater_permit(void);

/* Heater enable line. Low at boot. While the real heater is absent it is also
 * shown on the LED (BMC_HEATER_ENABLE_MIRRORS_LED in bmc_board.c). */
void board_heater_enable_set(bool on);

gpio_num_t board_i2c_sda_gpio(void);
gpio_num_t board_i2c_scl_gpio(void);

/* fan is 0 or 1. */
gpio_num_t board_fan_pwm_gpio(int fan);
gpio_num_t board_fan_tach_gpio(int fan);
/* One active-low interlock for both fans. */
gpio_num_t board_fan_enable_gpio(void);

/* Demanded-power analog out to the emulator (PWM + external RC filter). */
gpio_num_t board_heater_cmd_gpio(void);
