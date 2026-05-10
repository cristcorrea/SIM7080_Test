#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include "pmu_manager.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_SDA                     15
#define I2C_SCL                     7
#define PMU_INPUT_PIN               6

#define BOARD_MODEM_PWR_PIN         41
#define BOARD_MODEM_DTR_PIN         42
#define BOARD_MODEM_RI_PIN          3
#define BOARD_MODEM_RXD_PIN         4
#define BOARD_MODEM_TXD_PIN         5

static const char *TAG = "pmu_manager";

static XPowersPMU PMU;
static bool pmu_initialized = false;

esp_err_t pmu_manager_init(void)
{
    if (pmu_initialized) {
        return ESP_OK;
    }

    bool ok = PMU.begin(I2C_NUM_0, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
    if (!ok) {
        ESP_LOGE(TAG, "AXP2101 not found");
        return ESP_FAIL;
    }

    uint8_t chip_id = PMU.getChipID();
    ESP_LOGI(TAG, "AXP2101 initialized, chip ID: 0x%02X", chip_id);

    PMU.disableDC2();
    PMU.disableDC4();
    PMU.disableDC5();

    PMU.disableALDO1();

    PMU.disableCPUSLDO();
    PMU.disableDLDO1();
    PMU.disableDLDO2();

    PMU.disableTSPinMeasure();

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask =
        (1ULL << BOARD_MODEM_PWR_PIN) |
        (1ULL << BOARD_MODEM_DTR_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    gpio_set_level((gpio_num_t)BOARD_MODEM_PWR_PIN, 0);

    // DTR en LOW mantiene despierto al SIM7080.
    gpio_set_level((gpio_num_t)BOARD_MODEM_DTR_PIN, 0);

    pmu_initialized = true;

    return ESP_OK;
}

esp_err_t pmu_manager_enable_modem_power(void)
{
    if (!pmu_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // BLDO1 alimenta la conversión de nivel.
    PMU.setBLDO1Voltage(3300);
    PMU.enableBLDO1();

    // DC3 alimenta el módem SIM7080.
    PMU.setDC3Voltage(3000);
    PMU.enableDC3();

    ESP_LOGI(TAG, "Modem power enabled: DC3=3000mV, BLDO1=3300mV");

    return ESP_OK;
}

esp_err_t pmu_manager_enable_gnss_power(void)
{
    if (!pmu_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // BLDO2 alimenta la antena GNSS.
    PMU.setBLDO2Voltage(3300);
    PMU.enableBLDO2();

    ESP_LOGI(TAG, "GNSS antenna power enabled: BLDO2=3300mV");

    return ESP_OK;
}

esp_err_t pmu_manager_disable_gnss_power(void)
{
    if (!pmu_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    PMU.disableBLDO2();

    ESP_LOGI(TAG, "GNSS antenna power disabled");

    return ESP_OK;
}

esp_err_t pmu_manager_prepare_for_sleep(void)
{
    if (!pmu_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // De momento NO apagamos DC3 ni BLDO1.
    // Queremos probar primero que el módem queda controlable.
    PMU.disableBLDO2();

    // DTR HIGH permite que el módem pueda entrar en sleep si CSCLK está habilitado.
    gpio_set_level((gpio_num_t)BOARD_MODEM_DTR_PIN, 1);

    ESP_LOGI(TAG, "PMU prepared for sleep");

    return ESP_OK;
}