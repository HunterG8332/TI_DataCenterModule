idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES bmc_board bmc_fan bmc_fan_link bmc_i2c ina238 tmp117 ssd1306)
