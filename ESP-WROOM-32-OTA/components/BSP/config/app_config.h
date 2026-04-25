#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#if __has_include("app_private_config.h")
#include "app_private_config.h"
#endif

#define APP_CONFIG_PLACEHOLDER_TEXT "__APP_CONFIG_NOT_SET__"

#ifndef APP_PRIVATE_WIFI_SSID
#define APP_PRIVATE_WIFI_SSID "TP-LINK_D388"
#endif

#ifndef APP_PRIVATE_WIFI_PASSWORD
#define APP_PRIVATE_WIFI_PASSWORD "13534408305"
#endif

#ifndef APP_PRIVATE_ALIYUN_PRODUCT_KEY
#define APP_PRIVATE_ALIYUN_PRODUCT_KEY "k1jy7ZTTQ8V"
#endif

#ifndef APP_PRIVATE_ALIYUN_DEVICE_NAME
#define APP_PRIVATE_ALIYUN_DEVICE_NAME "ESP32-WROOM-32"
#endif

#ifndef APP_PRIVATE_ALIYUN_DEVICE_SECRET
#define APP_PRIVATE_ALIYUN_DEVICE_SECRET "6d699c5733d561787e8d08acbc807e4c"
#endif

#ifndef APP_PRIVATE_ALIYUN_REGION_ID
#define APP_PRIVATE_ALIYUN_REGION_ID "cn-shanghai"
#endif

#ifndef APP_PRIVATE_ALIYUN_MQTT_HOST
#define APP_PRIVATE_ALIYUN_MQTT_HOST "iot-06z00fpcn2h806i.mqtt.iothub.aliyuncs.com"
#endif

#ifndef APP_PRIVATE_WEATHER_BASE_URL
#define APP_PRIVATE_WEATHER_BASE_URL "http://api.seniverse.com"
#endif

#ifndef APP_PRIVATE_WEATHER_API_KEY
#define APP_PRIVATE_WEATHER_API_KEY APP_CONFIG_PLACEHOLDER_TEXT
#endif

#ifndef APP_PRIVATE_DEEPSEEK_API_URL
#define APP_PRIVATE_DEEPSEEK_API_URL "https://api.deepseek.com/v1/chat/completions"
#endif

#ifndef APP_PRIVATE_DEEPSEEK_API_KEY
#define APP_PRIVATE_DEEPSEEK_API_KEY APP_CONFIG_PLACEHOLDER_TEXT
#endif

#ifndef APP_FEATURE_ENABLE_BLE_PROVISION_STARTUP
#define APP_FEATURE_ENABLE_BLE_PROVISION_STARTUP 0U
#endif

#ifndef APP_FEATURE_ENABLE_WEATHER_SERVICE_STARTUP
#define APP_FEATURE_ENABLE_WEATHER_SERVICE_STARTUP 0U
#endif

#ifndef APP_FEATURE_ENABLE_POWER_SERVICE_STARTUP
#define APP_FEATURE_ENABLE_POWER_SERVICE_STARTUP 0U
#endif

#ifndef APP_FEATURE_ENABLE_DIAGNOSTICS_SERVICE_STARTUP
#define APP_FEATURE_ENABLE_DIAGNOSTICS_SERVICE_STARTUP 0U
#endif

#define APP_THERMAL_PROP_MIN_TEMP "MinTemp"
#define APP_THERMAL_PROP_MAX_TEMP "MaxTemp"
#define APP_THERMAL_PROP_CENTER_TEMP "CenterTemp"

const char *app_config_wifi_ssid(void);
const char *app_config_wifi_password(void);
const char *app_config_cloud_product_key(void);
const char *app_config_cloud_device_name(void);
const char *app_config_cloud_device_secret(void);
const char *app_config_cloud_region_id(void);
const char *app_config_cloud_mqtt_host(void);
const char *app_config_weather_base_url(void);
const char *app_config_weather_api_key(void);
const char *app_config_deepseek_api_url(void);
const char *app_config_deepseek_api_key(void);

bool app_config_is_placeholder_value(const char *value);
bool app_config_wifi_is_configured(void);
bool app_config_cloud_mqtt_is_configured(void);
bool app_config_weather_is_configured(void);
bool app_config_deepseek_is_configured(void);
bool app_config_build_weather_now_url(char *buffer, size_t buffer_len, const char *city_name);

#endif
