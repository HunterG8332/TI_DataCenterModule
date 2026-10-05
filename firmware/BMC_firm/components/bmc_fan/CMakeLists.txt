idf_component_register(SRCS "bmc_fan.c"
                        INCLUDE_DIRS "include"
                        REQUIRES esp_driver_ledc esp_driver_pcnt
                        PRIV_REQUIRES esp_driver_gpio)
