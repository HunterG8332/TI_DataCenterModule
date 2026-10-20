#include "bmc_i2c.h"

#include "bmc_board.h"
#include "driver/gpio.h"

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

bool bmc_i2c_lines_stuck(void)
{
    /* The bus is idle-high through its pull-ups. A line that reads low while no
     * transfer is running is being held by a device or a short. Read right after a
     * failed transfer; the driver leaves both pins configured as inputs/open-drain. */
    return gpio_get_level(board_i2c_sda_gpio()) == 0 || gpio_get_level(board_i2c_scl_gpio()) == 0;
}

esp_err_t bmc_i2c_recover(void)
{
    return i2c_master_bus_reset(s_bus);
}
