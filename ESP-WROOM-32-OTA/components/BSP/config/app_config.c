#include "app_config.h"

#include <stdio.h>
#include <string.h>

const char *app_config_wifi_ssid(void)
{
    return APP_PRIVATE_WIFI_SSID;
}

const char *app_config_wifi_password(void)
{
    return APP_PRIVATE_WIFI_PASSWORD;
}

const char *app_config_cloud_product_key(void)
{
    return APP_PRIVATE_ALIYUN_PRODUCT_KEY;
}

const char *app_config_cloud_device_name(void)
{
    return APP_PRIVATE_ALIYUN_DEVICE_NAME;
}

const char *app_config_cloud_device_secret(void)
{
    return APP_PRIVATE_ALIYUN_DEVICE_SECRET;
}

const char *app_config_cloud_region_id(void)
{
    return APP_PRIVATE_ALIYUN_REGION_ID;
}

const char *app_config_cloud_mqtt_host(void)
{
    return APP_PRIVATE_ALIYUN_MQTT_HOST;
}

const char *app_config_weather_base_url(void)
{
    return APP_PRIVATE_WEATHER_BASE_URL;
}

const char *app_config_weather_api_key(void)
{
    return APP_PRIVATE_WEATHER_API_KEY;
}

const char *app_config_deepseek_api_url(void)
{
    return APP_PRIVATE_DEEPSEEK_API_URL;
}

const char *app_config_deepseek_api_key(void)
{
    return APP_PRIVATE_DEEPSEEK_API_KEY;
}

bool app_config_is_placeholder_value(const char *value)
{
    return value == NULL ||
           value[0] == '\0' ||
           strcmp(value, APP_CONFIG_PLACEHOLDER_TEXT) == 0;
}

bool app_config_wifi_is_configured(void)
{
    return !app_config_is_placeholder_value(app_config_wifi_ssid()) &&
           !app_config_is_placeholder_value(app_config_wifi_password());
}

bool app_config_cloud_mqtt_is_configured(void)
{
    return !app_config_is_placeholder_value(app_config_cloud_product_key()) &&
           !app_config_is_placeholder_value(app_config_cloud_device_name()) &&
           !app_config_is_placeholder_value(app_config_cloud_device_secret()) &&
           (!app_config_is_placeholder_value(app_config_cloud_mqtt_host()) ||
            !app_config_is_placeholder_value(app_config_cloud_region_id()));
}

bool app_config_weather_is_configured(void)
{
    return !app_config_is_placeholder_value(app_config_weather_base_url()) &&
           !app_config_is_placeholder_value(app_config_weather_api_key());
}

bool app_config_deepseek_is_configured(void)
{
    return !app_config_is_placeholder_value(app_config_deepseek_api_url()) &&
           !app_config_is_placeholder_value(app_config_deepseek_api_key());
}

bool app_config_build_weather_now_url(char *buffer, size_t buffer_len, const char *city_name)
{
    int written = 0;

    if (buffer == NULL || buffer_len == 0U || city_name == NULL || city_name[0] == '\0')
    {
        return false;
    }

    if (!app_config_weather_is_configured())
    {
        return false;
    }

    written = snprintf(buffer,
                       buffer_len,
                       "%s/v3/weather/now.json?key=%s&location=%s&language=zh-Hans&unit=c",
                       app_config_weather_base_url(),
                       app_config_weather_api_key(),
                       city_name);

    return written > 0 && (size_t)written < buffer_len;
}
