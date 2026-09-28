#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

/* Owns the single I2C bus (bmc_board's SDA/SCL pins, 400 kHz Fast-mode).
 * Every device component (tmp117, later ina238) adds itself to the bus
 * returned by bmc_i2c_bus(). */

esp_err_t bmc_i2c_init(void);

i2c_master_bus_handle_t bmc_i2c_bus(void);
