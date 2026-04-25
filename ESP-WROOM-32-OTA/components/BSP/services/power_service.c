#include "power_service.h"

#include "app_config.h"
#include "esp_log.h"

static const char *TAG = "POWER_SVC";

void power_service_init(void)
{
    ESP_LOGI(TAG, "Power service registered");
}

void power_service_start(void)
{
    if (APP_FEATURE_ENABLE_POWER_SERVICE_STARTUP == 0U)
    {
        ESP_LOGI(TAG, "Power service keeps legacy host-driven behavior in phase 1");
    }
}
