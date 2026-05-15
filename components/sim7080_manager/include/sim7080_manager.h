#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*sim7080_power_cb_t)(void *user_ctx);

typedef struct {
    const char *apn;

    uart_port_t uart_port;
    int uart_baud;
    gpio_num_t tx_pin;      // ESP32-S3 TX -> SIM7080 RX
    gpio_num_t rx_pin;      // ESP32-S3 RX <- SIM7080 TX
    gpio_num_t rts_pin;     // GPIO_NUM_NC si no se usa
    gpio_num_t cts_pin;     // GPIO_NUM_NC si no se usa

    gpio_num_t pwrkey_pin;  // PWRKEY del SIM7080 en la placa LILYGO
    gpio_num_t dtr_pin;     // DTR del SIM7080. LOW = despierto en esta placa
    gpio_num_t ri_pin;      // RI opcional

    uint32_t sync_retry_count;
    uint32_t sync_retry_delay_ms;
    uint32_t ppp_timeout_ms;

    sim7080_power_cb_t power_on_cb;   // Opcional: encender AXP2101/DC3/BLDO1 antes del PWRKEY
    sim7080_power_cb_t power_off_cb;  // Opcional: apagar canales PMU al final
    void *power_user_ctx;
} sim7080_manager_config_t;

#define SIM7080_MANAGER_DEFAULT_CONFIG(APN) {      \
    .apn = (APN),                                  \
    .uart_port = UART_NUM_1,                       \
    .uart_baud = 115200,                           \
    .tx_pin = GPIO_NUM_5,                          \
    .rx_pin = GPIO_NUM_4,                          \
    .rts_pin = GPIO_NUM_NC,                        \
    .cts_pin = GPIO_NUM_NC,                        \
    .pwrkey_pin = GPIO_NUM_41,                     \
    .dtr_pin = GPIO_NUM_42,                        \
    .ri_pin = GPIO_NUM_3,                          \
    .sync_retry_count = 15,                        \
    .sync_retry_delay_ms = 1000,                   \
    .ppp_timeout_ms = 90000,                       \
    .power_on_cb = NULL,                           \
    .power_off_cb = NULL,                          \
    .power_user_ctx = NULL                         \
}

esp_err_t sim7080_manager_init(const sim7080_manager_config_t *config);
esp_err_t sim7080_manager_start_ppp(void);
esp_err_t sim7080_manager_wait_connected(uint32_t timeout_ms);
esp_err_t sim7080_manager_stop_ppp(void);
esp_err_t sim7080_manager_deinit(void);

bool sim7080_manager_is_connected(void);
esp_netif_t *sim7080_manager_get_netif(void);

#ifdef __cplusplus
}
#endif
