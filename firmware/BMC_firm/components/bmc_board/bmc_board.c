#include "bmc_board.h"

#include "driver/gpio.h"

/* ESP32-WROOM-32E dev kit pin map. Pin numbers live only in this file. */
#define BMC_LED_GPIO GPIO_NUM_12
#define BMC_I2C_SDA_GPIO GPIO_NUM_22
#define BMC_I2C_SCL_GPIO GPIO_NUM_21
#define BMC_FAN_PWM_GPIO GPIO_NUM_18
#define BMC_FAN_TACH_GPIO GPIO_NUM_19
#define BMC_FAN_ENABLE_GPIO GPIO_NUM_16

void board_init(void)
{
    gpio_reset_pin(BMC_LED_GPIO);
    gpio_set_direction(BMC_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BMC_LED_GPIO, 0);
}

void board_led_set(bool on)
{
    gpio_set_level(BMC_LED_GPIO, on ? 1 : 0);
}

/* Stand-in heater enable: the LED pin, per the dummy-heater stage. The real
 * HEATER ENABLE (GPIO40 on the S3 board) replaces this on the Stage 7 port. */
void board_heater_enable_set(bool on)
{
    gpio_set_level(BMC_LED_GPIO, on ? 1 : 0);
}

gpio_num_t board_i2c_sda_gpio(void)
{
    return BMC_I2C_SDA_GPIO;
}

gpio_num_t board_i2c_scl_gpio(void)
{
    return BMC_I2C_SCL_GPIO;
}

gpio_num_t board_fan_pwm_gpio(void)
{
    return BMC_FAN_PWM_GPIO;
}

gpio_num_t board_fan_tach_gpio(void)
{
    return BMC_FAN_TACH_GPIO;
}

gpio_num_t board_fan_enable_gpio(void)
{
    return BMC_FAN_ENABLE_GPIO;
}
