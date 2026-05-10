#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pmu_manager.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Boot");

    ESP_ERROR_CHECK(pmu_manager_init());
    ESP_ERROR_CHECK(pmu_manager_enable_modem_power());
    ESP_ERROR_CHECK(pmu_manager_enable_gnss_power());

    ESP_LOGI(TAG, "PMU test OK. Modem and GNSS rails enabled.");

    vTaskDelay(pdMS_TO_TICKS(5000));

    ESP_ERROR_CHECK(pmu_manager_prepare_for_sleep());

    ESP_LOGI(TAG, "Going to deep sleep for 60 seconds - test mode");

    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    esp_deep_sleep_start();
}