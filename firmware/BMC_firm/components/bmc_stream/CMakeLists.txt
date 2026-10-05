idf_component_register(SRCS "bmc_stream.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES bmc_telemetry bmc_net lwip esp_timer freertos)
