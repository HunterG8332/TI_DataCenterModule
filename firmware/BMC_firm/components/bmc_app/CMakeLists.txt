idf_component_register(SRCS "sensors.c" "actuators.c" "supervisor.c" "bmc_aux.c"
                        INCLUDE_DIRS "include"
                        REQUIRES bmc_core bmc_fan esp_common
                        PRIV_REQUIRES bmc_board bmc_i2c bmc_telemetry bmc_stream bmc_heater_cmd
                                      bmc_fan_link tmp117 ina238 ssd1306 esp_timer esp_system
                                      freertos log)
