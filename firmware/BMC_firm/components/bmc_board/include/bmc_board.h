#pragma once

#include <stdbool.h>
#include "hal/gpio_types.h"

/* Board abstraction for the BMC. This header is the only place the rest of the
 * firmware should learn about board wiring. Porting to a new target means
 * changing bmc_board.c/.h and nothing else. */

void board_init(void);

/* Single run jumper (active-low, pull-up inside). True when bridged to ground. False
 * when open or unwired: fans are forced to 0 RPM and the heater is held off. Not latched;
 * outputs resume when the jumper is replaced. */
bool board_run_jumper_closed(void);

gpio_num_t board_i2c_sda_gpio(void);
gpio_num_t board_i2c_scl_gpio(void);

/* fan is 0 or 1. */
gpio_num_t board_fan_pwm_gpio(int fan);
gpio_num_t board_fan_tach_gpio(int fan);
/* The run jumper pin, read by the fan driver as its interlock. */
gpio_num_t board_fan_enable_gpio(void);

/* Demanded-power analog out to the emulator (PWM + external RC filter). */
gpio_num_t board_heater_cmd_gpio(void);
