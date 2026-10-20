#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Owns the single I2C bus (bmc_board's SDA/SCL pins, 400 kHz Fast-mode).
 * Every device component (tmp117, ina238, ...) adds itself to the bus
 * returned by bmc_i2c_bus(). */

esp_err_t bmc_i2c_init(void);

i2c_master_bus_handle_t bmc_i2c_bus(void);

/* True when SDA or SCL is currently held low (stuck bus). Sample it after a
 * failed transfer; an idle healthy bus reads high on both lines. */
bool bmc_i2c_lines_stuck(void);

/* Resets the bus controller and clocks out a stuck slave (driver bus reset). */
esp_err_t bmc_i2c_recover(void);
