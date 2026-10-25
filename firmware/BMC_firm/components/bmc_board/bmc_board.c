#include "bmc_board.h"

#include "driver/gpio.h"

/* ESP32-S3-ETH dev board pin map. Pin numbers live only in this file.
 * The W5500 Ethernet uses GPIO9-14 and the SD slot GPIO4-6; none of the pins below clash. */
#define BMC_I2C_SDA_GPIO GPIO_NUM_2
#define BMC_I2C_SCL_GPIO GPIO_NUM_1
/* Fan 1: the ARCTIC S8038-7K (~7000 RPM max). */
#define BMC_FAN_PWM_GPIO GPIO_NUM_18
#define BMC_FAN_TACH_GPIO GPIO_NUM_16
/* Fan 2: the ~10000 RPM fan. */
#define BMC_FAN2_PWM_GPIO GPIO_NUM_17
#define BMC_FAN2_TACH_GPIO GPIO_NUM_21
/* One run jumper gates both fans and the heater: jumper to ground = run, open (internal
 * pull-up) = fans 0 RPM and heater off. A missing or broken jumper fails safe. */
#define BMC_RUN_JUMPER_GPIO GPIO_NUM_15
/* Demanded-power analog out (filtered PWM). GPIO3 is a strapping pin, so the board needs an
 * external pull-down to keep the heater command low while the pin is hi-Z during reset. */
#define BMC_HEATER_CMD_GPIO GPIO_NUM_3

void board_init(void)
{
    gpio_reset_pin(BMC_RUN_JUMPER_GPIO);
    gpio_set_direction(BMC_RUN_JUMPER_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BMC_RUN_JUMPER_GPIO, GPIO_PULLUP_ONLY);
}

bool board_run_jumper_closed(void)
{
    return gpio_get_level(BMC_RUN_JUMPER_GPIO) == 0;
}

gpio_num_t board_i2c_sda_gpio(void)
{
    return BMC_I2C_SDA_GPIO;
}

gpio_num_t board_i2c_scl_gpio(void)
{
    return BMC_I2C_SCL_GPIO;
}

gpio_num_t board_fan_pwm_gpio(int fan)
{
    return fan == 0 ? BMC_FAN_PWM_GPIO : BMC_FAN2_PWM_GPIO;
}

gpio_num_t board_fan_tach_gpio(int fan)
{
    return fan == 0 ? BMC_FAN_TACH_GPIO : BMC_FAN2_TACH_GPIO;
}

gpio_num_t board_fan_enable_gpio(void)
{
    return BMC_RUN_JUMPER_GPIO;
}

gpio_num_t board_heater_cmd_gpio(void)
{
    return BMC_HEATER_CMD_GPIO;
}
