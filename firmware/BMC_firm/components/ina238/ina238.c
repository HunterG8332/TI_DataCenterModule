#include "ina238.h"

#define INA238_REG_CONFIG       0x00
#define INA238_REG_SHUNT_CAL    0x02
#define INA238_REG_POWER        0x08  /* 24-bit, unsigned */
#define INA238_REG_MANUF_ID     0x3E
#define INA238_REG_DEVICE_ID    0x3F

#define INA238_MANUF_ID         0x5449  /* "TI" */
#define INA238_DEVICE_ID        0x238   /* DEVICE_ID[15:4]; [3:0] is die revision */

#define INA238_SHUNT_OHMS       0.002f
#define INA238_MAX_CURRENT_A    5.0f
#define INA238_CURRENT_LSB_A    (INA238_MAX_CURRENT_A / 32768.0f)
/* SHUNT_CAL = 819.2e6 * Current_LSB * Rshunt (ADCRANGE = 0) -> 250 */
#define INA238_SHUNT_CAL        250
#define INA238_POWER_LSB_W      (0.2f * INA238_CURRENT_LSB_A)

#define INA238_SCL_SPEED_HZ     400000
#define INA238_XFER_TIMEOUT_MS  1000

static esp_err_t read_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, rx, sizeof(rx),
                                                 INA238_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    *out = ((uint16_t)rx[0] << 8) | rx[1];
    return ESP_OK;
}

static esp_err_t write_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val)
{
    uint8_t tx[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
    return i2c_master_transmit(dev, tx, sizeof(tx), INA238_XFER_TIMEOUT_MS);
}

esp_err_t ina238_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr, ina238_t *dev)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = INA238_SCL_SPEED_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t id;
    err = read_reg16(dev->i2c_dev, INA238_REG_MANUF_ID, &id);
    if (err != ESP_OK) {
        return err;
    }
    if (id != INA238_MANUF_ID) {
        return ESP_ERR_NOT_FOUND;
    }
    err = read_reg16(dev->i2c_dev, INA238_REG_DEVICE_ID, &id);
    if (err != ESP_OK) {
        return err;
    }
    if ((id >> 4) != INA238_DEVICE_ID) {
        return ESP_ERR_NOT_FOUND;
    }

    /* CONFIG = 0: ADCRANGE = 0 (+/-163.84 mV), no conversion delay. ADC_CONFIG
     * is left at its power-on default (continuous, all channels). */
    err = write_reg16(dev->i2c_dev, INA238_REG_CONFIG, 0x0000);
    if (err != ESP_OK) {
        return err;
    }
    return write_reg16(dev->i2c_dev, INA238_REG_SHUNT_CAL, INA238_SHUNT_CAL);
}

esp_err_t ina238_read_power_w(ina238_t *dev, float *out_power_w)
{
    uint8_t reg = INA238_REG_POWER;
    uint8_t rx[3];
    esp_err_t err = i2c_master_transmit_receive(dev->i2c_dev, &reg, 1, rx, sizeof(rx),
                                                 INA238_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t raw = ((uint32_t)rx[0] << 16) | ((uint32_t)rx[1] << 8) | rx[2];
    *out_power_w = raw * INA238_POWER_LSB_W;
    return ESP_OK;
}
