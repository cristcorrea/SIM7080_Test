#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pmu_manager.h"
#include "sim7080_manager.h"

static const char *TAG = "main";

/*
 * Reemplazar por el APN real de tu SIM.
 *
 * Ejemplos comunes:
 * - "internet"
 * - "iot.1nce.net"
 * - "m2m.tele2.com"
 * - "web.omnitel.it"
 */
#define CELLULAR_APN "TU_APN"

void app_main(void)
{
    ESP_LOGI(TAG, "Boot");

    /*
     * 1) Inicializar PMU.
     */
    ESP_ERROR_CHECK(pmu_manager_init());

    /*
     * 2) Encender alimentación del SIM7080.
     * Esto debe habilitar los rieles necesarios del AXP2101.
     */
    ESP_ERROR_CHECK(pmu_manager_enable_modem_power());

    /*
     * GNSS no hace falta para enviar a Supabase.
     * Lo dejamos apagado por ahora para reducir consumo y simplificar el test.
     */
    // ESP_ERROR_CHECK(pmu_manager_enable_gnss_power());

    ESP_LOGI(TAG, "PMU OK. Modem rail enabled.");

    /*
     * Pequeña espera para estabilizar alimentación antes de tocar PWRKEY/UART.
     */
    vTaskDelay(pdMS_TO_TICKS(1000));

    /*
     * 3) Configurar SIM7080.
     * Los pines por defecto vienen del sim7080_manager:
     * TX=GPIO5, RX=GPIO4, PWRKEY=GPIO41, DTR=GPIO42, RI=GPIO3.
     */
    sim7080_manager_config_t modem_cfg =
        SIM7080_MANAGER_DEFAULT_CONFIG(CELLULAR_APN);

    /*
     * 4) Inicializar módem:
     * - GPIO
     * - PWRKEY
     * - UART
     * - comandos AT básicos
     * - APN
     */
    ESP_ERROR_CHECK(sim7080_manager_init(&modem_cfg));

    /*
     * 5) Levantar PPP.
     * Si esto funciona, el ESP32 ya tiene Internet a través del SIM7080.
     */
    ESP_LOGI(TAG, "Starting PPP...");

    esp_err_t err = sim7080_manager_start_ppp();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPP failed: %s", esp_err_to_name(err));

        /*
         * En esta etapa de prueba conviene NO dormir inmediatamente.
         * Dejar logs visibles para diagnosticar red, SIM, APN o señal.
         */
        while (1) {
            ESP_LOGE(TAG, "PPP not connected. Check SIM/APN/antenna/network.");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    ESP_LOGI(TAG, "PPP connected. Internet is available through SIM7080.");

    /*
     * Próximo paso:
     * supabase_client_post_telemetry(...)
     */

    vTaskDelay(pdMS_TO_TICKS(5000));

    /*
     * 6) Cerrar PPP antes de dormir.
     */
    ESP_ERROR_CHECK(sim7080_manager_stop_ppp());
    ESP_ERROR_CHECK(sim7080_manager_deinit());

    /*
     * 7) Preparar PMU para sleep.
     */
    ESP_ERROR_CHECK(pmu_manager_prepare_for_sleep());

    ESP_LOGI(TAG, "Going to deep sleep for 60 seconds - modem test mode");

    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    esp_deep_sleep_start();
}