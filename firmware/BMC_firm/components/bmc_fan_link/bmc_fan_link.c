#include "bmc_fan_link.h"

#define FAN_LINK_SCL_SPEED_HZ    400000
#define FAN_LINK_XFER_TIMEOUT_MS 100

esp_err_t bmc_fan_link_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr, bmc_fan_link_t *dev)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = FAN_LINK_SCL_SPEED_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
}

esp_err_t bmc_fan_link_send(bmc_fan_link_t *dev, float duty_pct)
{
    if (duty_pct < 0.0f) {
        duty_pct = 0.0f;
    } else if (duty_pct > 100.0f) {
        duty_pct = 100.0f;
    }
    uint8_t byte = (uint8_t)(duty_pct + 0.5f);
    return i2c_master_transmit(dev->i2c_dev, &byte, 1, FAN_LINK_XFER_TIMEOUT_MS);
}
