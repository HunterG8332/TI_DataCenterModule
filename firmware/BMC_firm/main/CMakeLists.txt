idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES bmc_app bmc_board bmc_i2c bmc_net bmc_stream bmc_telemetry esp_system)
