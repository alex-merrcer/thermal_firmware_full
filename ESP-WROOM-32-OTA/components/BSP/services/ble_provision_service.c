#include "ble_provision_service.h"

#include "app_config.h"
#include "esp_log.h"

static const char *TAG = "BLE_PROVISION";

void ble_provision_service_init(void)
{
    ESP_LOGI(TAG, "BLE provision service registered");
}

void ble_provision_service_start(void)
{
    if (APP_FEATURE_ENABLE_BLE_PROVISION_STARTUP == 0U)
    {
        ESP_LOGI(TAG, "BLE provision service is disabled by default in phase 1");
    }
}
