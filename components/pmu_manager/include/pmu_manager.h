#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pmu_manager_init(void);

esp_err_t pmu_manager_enable_modem_power(void);
esp_err_t pmu_manager_enable_gnss_power(void);
esp_err_t pmu_manager_disable_gnss_power(void);

esp_err_t pmu_manager_prepare_for_sleep(void);

#ifdef __cplusplus
}
#endif