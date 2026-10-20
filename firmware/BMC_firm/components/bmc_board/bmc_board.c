#include "bmc_board.h"

#include "driver/gpio.h"

/* ESP32-WROOM-32E dev kit pin map. Pin numbers live only in this file. */
#define BMC_LED_GPIO GPIO_NUM_12
#define BMC_I2C_SDA_GPIO GPIO_NUM_22
#define BMC_I2C_SCL_GPIO GPIO_NUM_21
/* Fan 1: the ARCTIC S8038-7K (~7000 RPM max). */
#define BMC_FAN_PWM_GPIO GPIO_NUM_18
#define BMC_FAN_TACH_GPIO GPIO_NUM_19
/* Fan 2: the ~10000 RPM fan. PROPOSED pins -- check against your wiring and change here
 * if they clash. GPIO25/26 are plain I/O on the WROOM-32E, quiet at boot. */
#define BMC_FAN2_PWM_GPIO GPIO_NUM_25
#define BMC_FAN2_TACH_GPIO GPIO_NUM_26
/* One enable interlock gates both fans (active-low, internal pull-up). */
#define BMC_FAN_ENABLE_GPIO GPIO_NUM_16
/* Demanded-power analog out (filtered PWM). GPIO23: no strapping/JTAG/flash
 * role, free of the fan, I2C and LED pins. */
#define BMC_HEATER_CMD_GPIO GPIO_NUM_23
/* Heater permit switch, active-low. GPIO17: plain I/O, quiet at boot (GPIO14
 * would emit a PWM burst during boot). */
#define BMC_HEATER_PERMIT_GPIO GPIO_NUM_17
/* Heater enable output (drives the gate-driver enable once that exists). PROPOSED
 * pin: GPIO27 is plain I/O and quiet at boot. A pull-down on the board keeps the
 * heater off while the pin is hi-Z during reset. */
#define BMC_HEATER_ENABLE_GPIO GPIO_NUM_27
/* While the real heater is not connected the enable line is shown on the LED, so a
 * dummy-heater bench test has something to look at. Set to 0 to leave the LED alone. */
#define BMC_HEATER_ENABLE_MIRRORS_LED 1

void board_init(void)
{
    gpio_reset_pin(BMC_LED_GPIO);
    gpio_set_direction(BMC_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BMC_LED_GPIO, 0);

    /* Heater enable starts low (off) and stays low until firmware explicitly raises it. */
    gpio_reset_pin(BMC_HEATER_ENABLE_GPIO);
    gpio_set_level(BMC_HEATER_ENABLE_GPIO, 0);
    gpio_set_direction(BMC_HEATER_ENABLE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BMC_HEATER_ENABLE_GPIO, 0);

    /* Internal pull-up: an unwired or broken permit line reads "not permitted". */
    gpio_reset_pin(BMC_HEATER_PERMIT_GPIO);
    gpio_set_direction(BMC_HEATER_PERMIT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BMC_HEATER_PERMIT_GPIO, GPIO_PULLUP_ONLY);
}

bool board_heater_permit(void)
{
    return gpio_get_level(BMC_HEATER_PERMIT_GPIO) == 0;
}

void board_led_set(bool on)
{
    gpio_set_level(BMC_LED_GPIO, on ? 1 : 0);
}

void board_heater_enable_set(bool on)
{
    gpio_set_level(BMC_HEATER_ENABLE_GPIO, on ? 1 : 0);
#if BMC_HEATER_ENABLE_MIRRORS_LED
    gpio_set_level(BMC_LED_GPIO, on ? 1 : 0);
#endif
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
    return BMC_FAN_ENABLE_GPIO;
}

gpio_num_t board_heater_cmd_gpio(void)
{
    return BMC_HEATER_CMD_GPIO;
}
