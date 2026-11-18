#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Sends the commanded fan duty to a peer on the I2C bus as one raw byte
 * (no register byte): whole percent, 0..100.
 * bmc_fan_link_send_frame() sends a 2-byte frame instead: [fan %, heater %]. */

typedef struct {
    i2c_master_dev_handle_t i2c_dev;
} bmc_fan_link_t;

/* Adds the peer at i2c_addr to bus. This does not touch the wire, so it
 * succeeds even if the peer is absent; a missing peer shows up as an error
 * from bmc_fan_link_send(). */
esp_err_t bmc_fan_link_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr, bmc_fan_link_t *dev);

/* Rounds duty_pct to the nearest whole percent, clamps to 0..100, and sends it. */
esp_err_t bmc_fan_link_send(bmc_fan_link_t *dev, float duty_pct);

/* One write of 2 bytes, no register byte: byte 0 = commanded fan demand %,
 * byte 1 = APPLIED heater command % (0 when the heater is inhibited). Each is
 * rounded to a whole percent and clamped to 0..100. */
esp_err_t bmc_fan_link_send_frame(bmc_fan_link_t *dev, float fan_pct, float heater_pct);
