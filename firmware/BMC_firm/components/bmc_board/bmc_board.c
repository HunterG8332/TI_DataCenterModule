#include "bmc_board.h"

#include "driver/gpio.h"

/* ESP32-WROOM-32E dev kit pin map. Pin numbers live only in this file. */
#define BMC_LED_GPIO GPIO_NUM_12

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
