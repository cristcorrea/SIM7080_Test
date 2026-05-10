idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        pmu_manager
        esp_hw_support
        freertos
        log
)