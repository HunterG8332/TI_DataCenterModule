#include "bmc_i2c.h"

#include "bmc_board.h"

#define BMC_I2C_PORT_AUTO   (-1)
#define BMC_I2C_FREQ_HZ     400000
#define BMC_I2C_GLITCH_CNT  7

static i2c_master_bus_handle_t s_bus;

esp_err_t bmc_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BMC_I2C_PORT_AUTO,
        .sda_io_num = board_i2c_sda_gpio(),
        .scl_io_num = board_i2c_scl_gpio(),
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = BMC_I2C_GLITCH_CNT,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_bus);
}

i2c_master_bus_handle_t bmc_i2c_bus(void)
{
    return s_bus;
}
