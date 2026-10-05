#pragma once

#include <stdbool.h>
#include "hal/gpio_types.h"

/* Board abstraction for the BMC. This header is the only place main.c
 * (or any other component) should learn about board wiring. Porting to
 * a new target (e.g. WROOM -> S3-ETH in Stage 7) means changing
 * bmc_board.c/.h and nothing else. */

void board_init(void);

void board_led_set(bool on);

gpio_num_t board_i2c_sda_gpio(void);
gpio_num_t board_i2c_scl_gpio(void);

gpio_num_t board_fan_pwm_gpio(void);
gpio_num_t board_fan_tach_gpio(void);
gpio_num_t board_fan_enable_gpio(void);
