#include "weather_service.h"

#include "app_config.h"
#include "esp_log.h"

static const char *TAG = "WEATHER_SVC";
static weather_service_mode_t s_weather_mode = WEATHER_SERVICE_MODE_CLOUD_ONLY;

static weather_service_mode_t weather_service_select_mode(void)
{
    if (APP_FEATURE_ENABLE_WEATHER_SERVICE_STARTUP != 0U &&
        app_config_weather_is_configured())
    {
        return WEATHER_SERVICE_MODE_DEVICE_FETCH;
    }

    return WEATHER_SERVICE_MODE_CLOUD_ONLY;
}

void weather_service_init(void)
{
    s_weather_mode = weather_service_select_mode();

    if (s_weather_mode == WEATHER_SERVICE_MODE_DEVICE_FETCH)
    {
        ESP_LOGI(TAG,
                 "Weather service registered in reserved device-fetch mode; cloud-first remains recommended in phase 7");
        return;
    }

    ESP_LOGI(TAG,
             "Weather service registered in cloud-first mode; weather should come from the mini-program cloud function");
}

void weather_service_start(void)
{
    if (s_weather_mode == WEATHER_SERVICE_MODE_CLOUD_ONLY)
    {
        ESP_LOGI(TAG, "Weather service start skipped in cloud-first mode");
        return;
    }

    ESP_LOGW(TAG,
             "Device-local weather fetch is reserved for a later phase; no local task is started in the current build");
}

weather_service_mode_t weather_service_get_mode(void)
{
    return s_weather_mode;
}

uint8_t weather_service_is_cloud_only(void)
{
    return (s_weather_mode == WEATHER_SERVICE_MODE_CLOUD_ONLY) ? 1U : 0U;
}
