#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* TI INA238 driver, power-only. Configured for ADCRANGE = 0 (+/-163.84 mV),
 * a 2 mOhm shunt and a 5 A full-scale current (Current_LSB = 5 A / 2^15),
 * which gives SHUNT_CAL = 250 and a power resolution of 30.518 uW/LSB. */

typedef struct {
    i2c_master_dev_handle_t i2c_dev;
} ina238_t;

/* Adds the device to bus at i2c_addr (e.g. 0x40 for A1/A0 -> GND), verifies the
 * manufacturer/device ID registers, and programs SHUNT_CAL. */
esp_err_t ina238_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr, ina238_t *dev);

esp_err_t ina238_read_power_w(ina238_t *dev, float *out_power_w);
