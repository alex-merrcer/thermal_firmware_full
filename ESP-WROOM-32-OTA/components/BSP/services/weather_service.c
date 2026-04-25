#include "weather_service.h"

#include "app_config.h"
#include "esp_log.h"

static const char *TAG = "WEATHER_SVC";

void weather_service_init(void)
{
    ESP_LOGI(TAG, "Weather service registered");
}

void weather_service_start(void)
{
    if (APP_FEATURE_ENABLE_WEATHER_SERVICE_STARTUP == 0U)
    {
        ESP_LOGI(TAG, "Weather service is disabled by default in phase 1");
        return;
    }

    if (!app_config_weather_is_configured())
    {
        ESP_LOGW(TAG, "Weather service is not configured");
    }
}
