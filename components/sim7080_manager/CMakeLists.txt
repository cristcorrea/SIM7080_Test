idf_component_register(
    SRCS "sim7080_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_event esp_netif esp_modem freertos
)
