idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES esp_timer bmc_board bmc_i2c tmp117)
