idf_component_register(SRCS "bmc_net_ap.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash)
