#include "sim7080_manager.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"

static const char *TAG = "sim7080_manager";

#define SIM7080_CONNECTED_BIT    BIT0
#define SIM7080_DISCONNECTED_BIT BIT1

typedef struct {
    bool initialized;
    bool ppp_started;
    sim7080_manager_config_t cfg;
    esp_netif_t *netif;
    esp_modem_dce_t *dce;
    EventGroupHandle_t event_group;
    esp_event_handler_instance_t got_ip_handler;
    esp_event_handler_instance_t lost_ip_handler;
    esp_event_handler_instance_t ppp_status_handler;
} sim7080_ctx_t;

static sim7080_ctx_t s_ctx;

static esp_err_t sim7080_netif_event_loop_init_once(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    return ESP_OK;
}

static void sim7080_on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_dns_info_t dns_info;

        ESP_LOGI(TAG, "PPP conectado");
        ESP_LOGI(TAG, "IP      : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway : " IPSTR, IP2STR(&event->ip_info.gw));

        if (esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
            ESP_LOGI(TAG, "DNS     : " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        }

        xEventGroupSetBits(s_ctx.event_group, SIM7080_CONNECTED_BIT);
        xEventGroupClearBits(s_ctx.event_group, SIM7080_DISCONNECTED_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP perdió IP");
        xEventGroupClearBits(s_ctx.event_group, SIM7080_CONNECTED_BIT);
        xEventGroupSetBits(s_ctx.event_group, SIM7080_DISCONNECTED_BIT);
    }
}

static void sim7080_on_ppp_changed(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;
    ESP_LOGD(TAG, "PPP status event: %" PRIi32, event_id);
}

static esp_err_t sim7080_register_event_handlers(void)
{
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                            &sim7080_on_ip_event, NULL,
                                            &s_ctx.got_ip_handler),
        TAG, "No se pudo registrar IP_EVENT_PPP_GOT_IP");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                            &sim7080_on_ip_event, NULL,
                                            &s_ctx.lost_ip_handler),
        TAG, "No se pudo registrar IP_EVENT_PPP_LOST_IP");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                            &sim7080_on_ppp_changed, NULL,
                                            &s_ctx.ppp_status_handler),
        TAG, "No se pudo registrar NETIF_PPP_STATUS");

    return ESP_OK;
}

static void sim7080_unregister_event_handlers(void)
{
    if (s_ctx.got_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP, s_ctx.got_ip_handler);
        s_ctx.got_ip_handler = NULL;
    }
    if (s_ctx.lost_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_PPP_LOST_IP, s_ctx.lost_ip_handler);
        s_ctx.lost_ip_handler = NULL;
    }
    if (s_ctx.ppp_status_handler) {
        esp_event_handler_instance_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, s_ctx.ppp_status_handler);
        s_ctx.ppp_status_handler = NULL;
    }
}

static esp_err_t sim7080_gpio_init(void)
{
    uint64_t output_mask = (1ULL << s_ctx.cfg.pwrkey_pin) | (1ULL << s_ctx.cfg.dtr_pin);

    gpio_config_t out_conf = {
        .pin_bit_mask = output_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_conf), TAG, "No se pudieron configurar GPIO de salida del modem");

    if (s_ctx.cfg.ri_pin != GPIO_NUM_NC) {
        gpio_config_t in_conf = {
            .pin_bit_mask = (1ULL << s_ctx.cfg.ri_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&in_conf), TAG, "No se pudo configurar RI");
    }

    // En la placa LILYGO el ejemplo original deja DTR en LOW para mantener despierto el modem.
    ESP_RETURN_ON_ERROR(gpio_set_level(s_ctx.cfg.dtr_pin, 0), TAG, "No se pudo poner DTR en LOW");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_ctx.cfg.pwrkey_pin, 0), TAG, "No se pudo inicializar PWRKEY");

    return ESP_OK;
}

static esp_err_t sim7080_pwrkey_pulse(void)
{
    // Secuencia tomada del ejemplo LILYGO: LOW 100 ms, HIGH 1000 ms, LOW.
    ESP_RETURN_ON_ERROR(gpio_set_level(s_ctx.cfg.pwrkey_pin, 0), TAG, "PWRKEY LOW falló");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(gpio_set_level(s_ctx.cfg.pwrkey_pin, 1), TAG, "PWRKEY HIGH falló");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_RETURN_ON_ERROR(gpio_set_level(s_ctx.cfg.pwrkey_pin, 0), TAG, "PWRKEY LOW final falló");
    vTaskDelay(pdMS_TO_TICKS(3000));
    return ESP_OK;
}

static esp_err_t sim7080_sync_with_modem(void)
{
    esp_err_t ret = ESP_FAIL;

    for (uint32_t i = 0; i < s_ctx.cfg.sync_retry_count; ++i) {
        ret = esp_modem_sync(s_ctx.dce);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "AT sync OK");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Esperando modem AT... intento %" PRIu32 "/%" PRIu32,
                 i + 1, s_ctx.cfg.sync_retry_count);
        vTaskDelay(pdMS_TO_TICKS(s_ctx.cfg.sync_retry_delay_ms));
    }

    ESP_LOGW(TAG, "No respondió AT. Reintentando pulso PWRKEY");
    ESP_RETURN_ON_ERROR(sim7080_pwrkey_pulse(), TAG, "No se pudo pulsar PWRKEY");

    for (uint32_t i = 0; i < s_ctx.cfg.sync_retry_count; ++i) {
        ret = esp_modem_sync(s_ctx.dce);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "AT sync OK después de PWRKEY");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(s_ctx.cfg.sync_retry_delay_ms));
    }

    return ret;
}

esp_err_t sim7080_manager_init(const sim7080_manager_config_t *config)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config es NULL");
    ESP_RETURN_ON_FALSE(config->apn != NULL && strlen(config->apn) > 0,
                        ESP_ERR_INVALID_ARG, TAG, "APN inválido");
    ESP_RETURN_ON_FALSE(!s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "sim7080_manager ya fue inicializado");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *config;

    if (s_ctx.cfg.power_on_cb) {
        ESP_RETURN_ON_ERROR(s_ctx.cfg.power_on_cb(s_ctx.cfg.power_user_ctx), TAG, "power_on_cb falló");
    }

    ESP_RETURN_ON_ERROR(sim7080_gpio_init(), TAG, "GPIO init falló");
    ESP_RETURN_ON_ERROR(sim7080_pwrkey_pulse(), TAG, "PWRKEY init falló");
    ESP_RETURN_ON_ERROR(sim7080_netif_event_loop_init_once(), TAG, "netif/event loop init falló");

    s_ctx.event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_ctx.event_group != NULL, ESP_ERR_NO_MEM, TAG, "No hay memoria para event_group");

    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    s_ctx.netif = esp_netif_new(&netif_ppp_config);
    ESP_GOTO_ON_FALSE(s_ctx.netif != NULL, ESP_ERR_NO_MEM, fail, TAG, "No se pudo crear netif PPP");

    ESP_GOTO_ON_ERROR(sim7080_register_event_handlers(), fail, TAG, "No se pudieron registrar eventos");

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(s_ctx.cfg.apn);
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();

    dte_config.uart_config.port_num = s_ctx.cfg.uart_port;
    dte_config.uart_config.baud_rate = s_ctx.cfg.uart_baud;
    dte_config.uart_config.tx_io_num = s_ctx.cfg.tx_pin;
    dte_config.uart_config.rx_io_num = s_ctx.cfg.rx_pin;
    dte_config.uart_config.rts_io_num = s_ctx.cfg.rts_pin;
    dte_config.uart_config.cts_io_num = s_ctx.cfg.cts_pin;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_config.uart_config.rx_buffer_size = 2048;
    dte_config.uart_config.tx_buffer_size = 1024;
    dte_config.uart_config.event_queue_size = 30;
    dte_config.task_stack_size = 4096;
    dte_config.task_priority = 5;
    dte_config.dte_buffer_size = 1024;

    // SIM7080 pertenece a la familia SIM7070/SIM7080/SIM7090; esp_modem expone SIM7070.
    s_ctx.dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7070, &dte_config, &dce_config, s_ctx.netif);
    ESP_GOTO_ON_FALSE(s_ctx.dce != NULL, ESP_ERR_NO_MEM, fail, TAG, "No se pudo crear DCE SIM7070/SIM7080");

    ESP_GOTO_ON_ERROR(sim7080_sync_with_modem(), fail, TAG, "El modem no responde AT");

    // Dejar el modem despierto y sin eco antes de iniciar PPP.
    ESP_GOTO_ON_ERROR(esp_modem_set_echo(s_ctx.dce, false), fail, TAG, "No se pudo desactivar echo");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_modem_at(s_ctx.dce, "AT+CSCLK=0", NULL, 3000));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_modem_at(s_ctx.dce, "AT+CNETLIGHT=1", NULL, 3000));

    bool pin_ok = false;
    if (esp_modem_read_pin(s_ctx.dce, &pin_ok) == ESP_OK && !pin_ok) {
        ESP_LOGE(TAG, "La SIM requiere PIN. Desbloquearla antes o agregar esp_modem_set_pin().");
        goto fail;
    }

    ESP_GOTO_ON_ERROR(esp_modem_set_radio_state(s_ctx.dce, 1), fail, TAG, "No se pudo poner CFUN=1");
    ESP_GOTO_ON_ERROR(esp_modem_set_apn(s_ctx.dce, s_ctx.cfg.apn), fail, TAG, "No se pudo configurar APN");

    int rssi = 99;
    int ber = 99;
    if (esp_modem_get_signal_quality(s_ctx.dce, &rssi, &ber) == ESP_OK) {
        ESP_LOGI(TAG, "CSQ rssi=%d ber=%d", rssi, ber);
    } else {
        ESP_LOGW(TAG, "No se pudo leer CSQ; sigo igualmente hasta PPP");
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "sim7080_manager inicializado");
    return ESP_OK;

fail:
    sim7080_manager_deinit();
    return ret;
}

esp_err_t sim7080_manager_start_ppp(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "manager no inicializado");
    ESP_RETURN_ON_FALSE(s_ctx.dce != NULL, ESP_ERR_INVALID_STATE, TAG, "DCE no inicializado");

    xEventGroupClearBits(s_ctx.event_group, SIM7080_CONNECTED_BIT | SIM7080_DISCONNECTED_BIT);

    ESP_RETURN_ON_ERROR(esp_modem_set_mode(s_ctx.dce, ESP_MODEM_MODE_DATA), TAG, "No se pudo entrar a modo DATA/PPP");
    s_ctx.ppp_started = true;

    return sim7080_manager_wait_connected(s_ctx.cfg.ppp_timeout_ms);
}

esp_err_t sim7080_manager_wait_connected(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "manager no inicializado");

    EventBits_t bits = xEventGroupWaitBits(
        s_ctx.event_group,
        SIM7080_CONNECTED_BIT | SIM7080_DISCONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & SIM7080_CONNECTED_BIT) {
        return ESP_OK;
    }

    if (bits & SIM7080_DISCONNECTED_BIT) {
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "Timeout esperando IP por PPP");
    return ESP_ERR_TIMEOUT;
}

esp_err_t sim7080_manager_stop_ppp(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "manager no inicializado");
    ESP_RETURN_ON_FALSE(s_ctx.dce != NULL, ESP_ERR_INVALID_STATE, TAG, "DCE no inicializado");

    esp_err_t ret = esp_modem_set_mode(s_ctx.dce, ESP_MODEM_MODE_COMMAND);
    if (ret == ESP_OK) {
        s_ctx.ppp_started = false;
        xEventGroupClearBits(s_ctx.event_group, SIM7080_CONNECTED_BIT);
    }
    return ret;
}

esp_err_t sim7080_manager_deinit(void)
{
    if (s_ctx.dce) {
        if (s_ctx.ppp_started) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_modem_set_mode(s_ctx.dce, ESP_MODEM_MODE_COMMAND));
        }
        esp_modem_destroy(s_ctx.dce);
        s_ctx.dce = NULL;
    }

    sim7080_unregister_event_handlers();

    if (s_ctx.netif) {
        esp_netif_destroy(s_ctx.netif);
        s_ctx.netif = NULL;
    }

    if (s_ctx.event_group) {
        vEventGroupDelete(s_ctx.event_group);
        s_ctx.event_group = NULL;
    }

    if (s_ctx.cfg.power_off_cb) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(s_ctx.cfg.power_off_cb(s_ctx.cfg.power_user_ctx));
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    return ESP_OK;
}

bool sim7080_manager_is_connected(void)
{
    if (!s_ctx.event_group) {
        return false;
    }
    return (xEventGroupGetBits(s_ctx.event_group) & SIM7080_CONNECTED_BIT) != 0;
}

esp_netif_t *sim7080_manager_get_netif(void)
{
    return s_ctx.netif;
}
