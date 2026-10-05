#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* TI TMP117 driver: register 0x00 (temperature result, 7.8125 m*C/LSB,
 * two's complement) and register 0x0F (device ID, expected 0x0117). */

typedef struct {
    i2c_master_dev_handle_t i2c_dev;
} tmp117_t;

/* Adds the device to bus at i2c_addr (e.g. 0x48 for ADD0 -> GND) and
 * verifies the device ID register so a wiring mistake fails here rather
 * than as a plausible-looking bad reading later. Also programs the chip for
 * continuous conversions every ~15.5 ms with no averaging (see tmp117.c). */
esp_err_t tmp117_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr, tmp117_t *dev);

esp_err_t tmp117_read_c(tmp117_t *dev, float *out_temp_c);
