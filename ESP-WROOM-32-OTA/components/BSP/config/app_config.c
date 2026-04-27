#include "app_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "APP_CONFIG";

#define APP_WIFI_NVS_NAMESPACE    "app_wifi"
#define APP_WIFI_NVS_KEY_SSID     "ssid"
#define APP_WIFI_NVS_KEY_PASSWORD "password"
#define APP_WIFI_NVS_KEY_STATE    "state"

#define APP_WIFI_STATE_UNSET      0U
#define APP_WIFI_STATE_CONFIGURED 1U
#define APP_WIFI_STATE_CLEARED    2U

static char s_wifi_ssid[APP_CONFIG_WIFI_SSID_MAX_LEN + 1U];
static char s_wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX_LEN + 1U];
static bool s_wifi_loaded = false;
static bool s_wifi_configured = false;
static bool s_wifi_using_bootstrap = false;

static bool app_config_private_wifi_is_configured(void)
{
    return !app_config_is_placeholder_value(APP_PRIVATE_WIFI_SSID) &&
           !app_config_is_placeholder_value(APP_PRIVATE_WIFI_PASSWORD);
}

static void app_config_wifi_cache_reset(void)
{
    s_wifi_ssid[0] = '\0';
    s_wifi_password[0] = '\0';
    s_wifi_configured = false;
    s_wifi_using_bootstrap = false;
}

static void app_config_wifi_cache_set(const char *ssid, const char *password, bool using_bootstrap)
{
    size_t ssid_len = 0U;
    size_t password_len = 0U;

    app_config_wifi_cache_reset();

    if (ssid == NULL || password == NULL)
    {
        return;
    }

    ssid_len = strlen(ssid);
    password_len = strlen(password);
    if (ssid_len == 0U || password_len == 0U)
    {
        return;
    }

    memcpy(s_wifi_ssid, ssid, ssid_len);
    s_wifi_ssid[ssid_len] = '\0';
    memcpy(s_wifi_password, password, password_len);
    s_wifi_password[password_len] = '\0';
    s_wifi_configured = true;
    s_wifi_using_bootstrap = using_bootstrap;
}

static esp_err_t app_config_wifi_persist_credentials(const char *ssid,
                                                     const char *password,
                                                     uint8_t state)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;

    err = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Open WiFi config NVS failed: %s", esp_err_to_name(err));
        return err;
    }

    if (state == APP_WIFI_STATE_CLEARED)
    {
        err = nvs_erase_key(handle, APP_WIFI_NVS_KEY_SSID);
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            err = ESP_OK;
        }
        if (err == ESP_OK)
        {
            err = nvs_erase_key(handle, APP_WIFI_NVS_KEY_PASSWORD);
            if (err == ESP_ERR_NVS_NOT_FOUND)
            {
                err = ESP_OK;
            }
        }
    }
    else
    {
        err = nvs_set_str(handle, APP_WIFI_NVS_KEY_SSID, ssid);
        if (err == ESP_OK)
        {
            err = nvs_set_str(handle, APP_WIFI_NVS_KEY_PASSWORD, password);
        }
    }

    if (err == ESP_OK)
    {
        err = nvs_set_u8(handle, APP_WIFI_NVS_KEY_STATE, state);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Persist WiFi config NVS failed: %s", esp_err_to_name(err));
    }

    return err;
}

static esp_err_t app_config_wifi_load_from_nvs(char *ssid,
                                               size_t ssid_len,
                                               char *password,
                                               size_t password_len,
                                               uint8_t *state_out)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;
    esp_err_t ssid_err = ESP_OK;
    esp_err_t password_err = ESP_OK;
    size_t ssid_size = ssid_len;
    size_t password_size = password_len;
    uint8_t state = APP_WIFI_STATE_UNSET;

    if (state_out != NULL)
    {
        *state_out = APP_WIFI_STATE_UNSET;
    }
    if (ssid == NULL || password == NULL || ssid_len == 0U || password_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ssid[0] = '\0';
    password[0] = '\0';

    err = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        if (err != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGW(TAG, "Open WiFi config NVS for read failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    err = nvs_get_u8(handle, APP_WIFI_NVS_KEY_STATE, &state);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        state = APP_WIFI_STATE_UNSET;
        err = ESP_OK;
    }
    if (err != ESP_OK)
    {
        nvs_close(handle);
        ESP_LOGW(TAG, "Read WiFi config state failed: %s", esp_err_to_name(err));
        return err;
    }

    ssid_err = nvs_get_str(handle, APP_WIFI_NVS_KEY_SSID, ssid, &ssid_size);
    password_err = nvs_get_str(handle, APP_WIFI_NVS_KEY_PASSWORD, password, &password_size);
    nvs_close(handle);

    if (state_out != NULL)
    {
        *state_out = state;
    }

    if (ssid_err == ESP_OK && password_err == ESP_OK && ssid[0] != '\0' && password[0] != '\0')
    {
        return ESP_OK;
    }

    if (ssid_err != ESP_ERR_NVS_NOT_FOUND && ssid_err != ESP_OK)
    {
        ESP_LOGW(TAG, "Read WiFi SSID failed: %s", esp_err_to_name(ssid_err));
        return ssid_err;
    }
    if (password_err != ESP_ERR_NVS_NOT_FOUND && password_err != ESP_OK)
    {
        ESP_LOGW(TAG, "Read WiFi password failed: %s", esp_err_to_name(password_err));
        return password_err;
    }

    return ESP_ERR_NVS_NOT_FOUND;
}

static void app_config_wifi_ensure_loaded(void)
{
    if (!s_wifi_loaded)
    {
        (void)app_config_wifi_reload();
    }
}

const char *app_config_wifi_ssid(void)
{
    app_config_wifi_ensure_loaded();
    return s_wifi_configured ? s_wifi_ssid : NULL;
}

const char *app_config_wifi_password(void)
{
    app_config_wifi_ensure_loaded();
    return s_wifi_configured ? s_wifi_password : NULL;
}

esp_err_t app_config_wifi_reload(void)
{
    char ssid[APP_CONFIG_WIFI_SSID_MAX_LEN + 1U] = {0};
    char password[APP_CONFIG_WIFI_PASSWORD_MAX_LEN + 1U] = {0};
    uint8_t state = APP_WIFI_STATE_UNSET;
    esp_err_t err = ESP_OK;
    esp_err_t unexpected_err = ESP_OK;

    app_config_wifi_cache_reset();

    err = app_config_wifi_load_from_nvs(ssid, sizeof(ssid), password, sizeof(password), &state);
    if (err == ESP_OK)
    {
        app_config_wifi_cache_set(ssid, password, false);
        s_wifi_loaded = true;
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND)
    {
        unexpected_err = err;
    }

    if (state != APP_WIFI_STATE_CLEARED && app_config_private_wifi_is_configured())
    {
        err = app_config_wifi_persist_credentials(APP_PRIVATE_WIFI_SSID,
                                                  APP_PRIVATE_WIFI_PASSWORD,
                                                  APP_WIFI_STATE_CONFIGURED);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Seed WiFi credentials from app_private_config.h into NVS");
        }
        else
        {
            ESP_LOGW(TAG, "Use legacy WiFi bootstrap credentials in memory only");
        }

        app_config_wifi_cache_set(APP_PRIVATE_WIFI_SSID, APP_PRIVATE_WIFI_PASSWORD, true);
        s_wifi_loaded = true;
        return err;
    }

    s_wifi_loaded = true;
    return (unexpected_err != ESP_OK) ? unexpected_err : ESP_ERR_NOT_FOUND;
}

esp_err_t app_config_wifi_set_credentials(const char *ssid, const char *password)
{
    size_t ssid_len = 0U;
    size_t password_len = 0U;
    esp_err_t err = ESP_OK;

    if (app_config_is_placeholder_value(ssid) || app_config_is_placeholder_value(password))
    {
        return ESP_ERR_INVALID_ARG;
    }

    ssid_len = strlen(ssid);
    password_len = strlen(password);
    if (ssid_len == 0U || password_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (ssid_len > APP_CONFIG_WIFI_SSID_MAX_LEN || password_len > APP_CONFIG_WIFI_PASSWORD_MAX_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    err = app_config_wifi_persist_credentials(ssid, password, APP_WIFI_STATE_CONFIGURED);
    if (err != ESP_OK)
    {
        return err;
    }

    app_config_wifi_cache_set(ssid, password, false);
    s_wifi_loaded = true;
    return ESP_OK;
}

esp_err_t app_config_wifi_clear_credentials(void)
{
    esp_err_t err = app_config_wifi_persist_credentials(NULL, NULL, APP_WIFI_STATE_CLEARED);

    if (err != ESP_OK)
    {
        return err;
    }

    app_config_wifi_cache_reset();
    s_wifi_loaded = true;
    return ESP_OK;
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

bool app_config_is_placeholder_value(const char *value)
{
    return value == NULL ||
           value[0] == '\0' ||
           strcmp(value, APP_CONFIG_PLACEHOLDER_TEXT) == 0;
}

bool app_config_wifi_is_configured(void)
{
    app_config_wifi_ensure_loaded();
    return s_wifi_configured;
}

bool app_config_wifi_is_using_bootstrap(void)
{
    app_config_wifi_ensure_loaded();
    return s_wifi_using_bootstrap;
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
