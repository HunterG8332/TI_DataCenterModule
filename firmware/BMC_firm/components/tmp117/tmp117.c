#include "tmp117.h"

#define TMP117_REG_TEMP         0x00
#define TMP117_REG_DEVICE_ID    0x0F
#define TMP117_DEVICE_ID        0x0117
#define TMP117_TEMP_LSB_C       0.0078125f
#define TMP117_SCL_SPEED_HZ     400000
#define TMP117_XFER_TIMEOUT_MS  1000

#define TMP117_REG_CONFIG       0x01
/* Continuous mode, CONV = 000, AVG = 00 (no averaging): a new result about every
 * 15.5 ms. The power-on default (0x0220) is 8x averaging on a 1 s cycle, which
 * only yields a fresh value once per second -- too slow for 10 Hz telemetry. */
#define TMP117_CONFIG_FAST      0x0000

static esp_err_t write_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value)
{
    uint8_t tx[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(dev, tx, sizeof(tx), TMP117_XFER_TIMEOUT_MS);
}

static esp_err_t read_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, rx, sizeof(rx),
                                                 TMP117_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    *out = ((uint16_t)rx[0] << 8) | rx[1];
    return ESP_OK;
}

esp_err_t tmp117_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr, tmp117_t *dev)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = TMP117_SCL_SPEED_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t id;
    err = read_reg16(dev->i2c_dev, TMP117_REG_DEVICE_ID, &id);
    if (err != ESP_OK) {
        return err;
    }
    if (id != TMP117_DEVICE_ID) {
        return ESP_ERR_NOT_FOUND;
    }
    return write_reg16(dev->i2c_dev, TMP117_REG_CONFIG, TMP117_CONFIG_FAST);
}

esp_err_t tmp117_read_c(tmp117_t *dev, float *out_temp_c)
{
    uint16_t raw;
    esp_err_t err = read_reg16(dev->i2c_dev, TMP117_REG_TEMP, &raw);
    if (err != ESP_OK) {
        return err;
    }
    *out_temp_c = (int16_t)raw * TMP117_TEMP_LSB_C;
    return ESP_OK;
}
