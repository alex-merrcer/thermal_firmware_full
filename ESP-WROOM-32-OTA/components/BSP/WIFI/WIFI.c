#include "WIFI.h"

#include <string.h>

#include "app_config.h"
#include "app_service_bus.h"
#include "ble_provision_service.h"
#include "../../../../protocol/ota_ctrl_protocol.h"

static const char *TAG = "WIFI_HOST";

int server_socket = -1;
int server_connect_socket = -1;
EventGroupHandle_t xCreatedEventGroup_WifiConnect = NULL;
EventGroupHandle_t Smart_config_WifiConnect = NULL;

static uint8_t s_wifi_stack_ready = 0U;
static uint8_t s_wifi_handlers_ready = 0U;
static uint8_t s_wifi_started = 0U;
static uint8_t s_wifi_enabled = 0U;
static uint8_t s_wifi_connected = 0U;
static uint8_t s_wifi_power_policy = OTA_HOST_POWER_POLICY_BALANCED;
static uint8_t s_host_state = OTA_HOST_STATE_ACTIVE;
static uint8_t s_wifi_netif_created = 0U;
static uint8_t s_wifi_ota_guard = 0U;

static const char *wifi_service_config_source_text(void)
{
    return app_config_wifi_is_using_bootstrap() ? "bootstrap" : "nvs";
}

static void wifi_service_init_once(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = ESP_OK;

    if (xCreatedEventGroup_WifiConnect == NULL)
    {
        xCreatedEventGroup_WifiConnect = xEventGroupCreate();
    }
    if (Smart_config_WifiConnect == NULL)
    {
        Smart_config_WifiConnect = xEventGroupCreate();
    }

    if (s_wifi_stack_ready != 0U)
    {
        return;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }
    if (s_wifi_netif_created == 0U)
    {
        esp_netif_create_default_wifi_sta();
        s_wifi_netif_created = 1U;
    }
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    s_wifi_stack_ready = 1U;

    if (s_wifi_handlers_ready == 0U)
    {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
        s_wifi_handlers_ready = 1U;
    }
}

static esp_err_t wifi_service_apply_config(void)
{
    wifi_config_t wifi_config =
    {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK
        }
    };
    const char *ssid = app_config_wifi_ssid();
    const char *password = app_config_wifi_password();
    size_t ssid_len = 0U;
    size_t password_len = 0U;

    if (ssid == NULL || password == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ssid_len = strlen(ssid);
    password_len = strlen(password);
    if (ssid_len > sizeof(wifi_config.sta.ssid) ||
        password_len > sizeof(wifi_config.sta.password))
    {
        ESP_LOGE(TAG, "WiFi credentials exceed ESP-IDF limits");
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password, password, password_len);
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static esp_err_t wifi_service_apply_ps_mode(void)
{
    wifi_ps_type_t ps_type = WIFI_PS_MIN_MODEM;

    if (s_wifi_started == 0U || s_wifi_enabled == 0U)
    {
        return ESP_OK;
    }

    if (s_wifi_ota_guard != 0U)
    {
        ps_type = WIFI_PS_NONE;
    }
    else if (s_wifi_power_policy == OTA_HOST_POWER_POLICY_PERFORMANCE)
    {
        ps_type = WIFI_PS_NONE;
    }
    else if (s_host_state == OTA_HOST_STATE_STOP_IDLE ||
             s_host_state == OTA_HOST_STATE_STANDBY_PREP)
    {
        ps_type = WIFI_PS_MAX_MODEM;
    }
    else if (s_wifi_power_policy == OTA_HOST_POWER_POLICY_ECO &&
             s_host_state == OTA_HOST_STATE_SCREEN_OFF)
    {
        ps_type = WIFI_PS_MAX_MODEM;
    }

    return esp_wifi_set_ps(ps_type);
}

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            ESP_LOGI(TAG, "WiFi STA started");
            if (s_wifi_enabled != 0U)
            {
                ESP_LOGI(TAG, "WiFi connect request on STA start");
                esp_wifi_connect();
            }
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;

            s_wifi_connected = 0U;
            ble_provision_service_on_wifi_disconnected((disc != NULL) ? disc->reason : 0U);
            app_service_bus_clear_bits(APP_EVENT_WIFI_CONNECTED);
            if (xCreatedEventGroup_WifiConnect != NULL)
            {
                xEventGroupClearBits(xCreatedEventGroup_WifiConnect, WIFI_CONNECTED_BIT);
                xEventGroupSetBits(xCreatedEventGroup_WifiConnect, WIFI_FAIL_BIT);
            }
            ESP_LOGW(TAG,
                     "WiFi disconnected, reason=%u, auto-retry=%u",
                     (disc != NULL) ? (unsigned int)disc->reason : 0U,
                     (unsigned int)s_wifi_enabled);
            if (s_wifi_enabled != 0U)
            {
                esp_wifi_connect();
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_wifi_connected = 1U;
        ble_provision_service_on_wifi_connected();
        app_service_bus_set_bits(APP_EVENT_WIFI_CONNECTED);
        if (xCreatedEventGroup_WifiConnect != NULL)
        {
            xEventGroupClearBits(xCreatedEventGroup_WifiConnect, WIFI_FAIL_BIT);
            xEventGroupSetBits(xCreatedEventGroup_WifiConnect, WIFI_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "WiFi connected");
    }
}

esp_err_t wifi_service_set_enabled(uint8_t enabled)
{
    esp_err_t err = ESP_OK;

    wifi_service_init_once();
    ESP_LOGI(TAG, "wifi_service_set_enabled(%u)", (unsigned int)((enabled != 0U) ? 1U : 0U));

    if (enabled != 0U)
    {
        if (wifi_service_has_credentials() == 0U)
        {
            ESP_LOGW(TAG, "WiFi credentials are not configured, waiting for provisioning");
            return ESP_ERR_INVALID_ARG;
        }

        s_wifi_enabled = 1U;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK)
        {
            s_wifi_enabled = 0U;
            ESP_LOGE(TAG, "esp_wifi_set_mode(STA) failed: 0x%04X", (unsigned int)err);
            return err;
        }

        err = wifi_service_apply_config();
        if (err != ESP_OK)
        {
            s_wifi_enabled = 0U;
            ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: 0x%04X", (unsigned int)err);
            return err;
        }
        ESP_LOGI(TAG,
                 "Apply WiFi config, ssid=%s, source=%s",
                 app_config_wifi_ssid(),
                 wifi_service_config_source_text());

        if (s_wifi_started == 0U)
        {
            err = esp_wifi_start();
            if (err != ESP_OK)
            {
                s_wifi_enabled = 0U;
                ESP_LOGE(TAG, "esp_wifi_start failed: 0x%04X", (unsigned int)err);
                return err;
            }
            s_wifi_started = 1U;
            ESP_LOGI(TAG, "esp_wifi_start ok");
        }
        else
        {
            err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
            {
                s_wifi_enabled = 0U;
                ESP_LOGE(TAG, "esp_wifi_connect failed: 0x%04X", (unsigned int)err);
                return err;
            }
            ESP_LOGI(TAG, "esp_wifi_connect request sent");
        }

        err = wifi_service_apply_ps_mode();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_wifi_set_ps failed: 0x%04X", (unsigned int)err);
        }
        return ESP_OK;
    }

    if (s_wifi_ota_guard != 0U)
    {
        ESP_LOGI(TAG, "Skip WiFi disable because OTA guard is active");
        return wifi_service_apply_ps_mode();
    }

    s_wifi_enabled = 0U;
    s_wifi_connected = 0U;
    app_service_bus_clear_bits(APP_EVENT_WIFI_CONNECTED);
    ESP_LOGI(TAG, "Disable WiFi service");
    if (xCreatedEventGroup_WifiConnect != NULL)
    {
        xEventGroupClearBits(xCreatedEventGroup_WifiConnect, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    if (s_wifi_started != 0U)
    {
        err = esp_wifi_disconnect();
        if (err != ESP_OK &&
            err != ESP_ERR_WIFI_NOT_STARTED &&
            err != ESP_ERR_WIFI_CONN)
        {
            ESP_LOGW(TAG, "esp_wifi_disconnect failed: 0x%04X", (unsigned int)err);
        }

        err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT)
        {
            ESP_LOGE(TAG, "esp_wifi_stop failed: 0x%04X", (unsigned int)err);
            return err;
        }
        s_wifi_started = 0U;
    }

    err = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_mode(NULL) failed: 0x%04X", (unsigned int)err);
        return err;
    }
    return ESP_OK;
}

esp_err_t wifi_service_set_ota_guard(uint8_t enabled)
{
    s_wifi_ota_guard = (enabled != 0U) ? 1U : 0U;
    return wifi_service_apply_ps_mode();
}

esp_err_t wifi_service_apply_host_power(uint8_t power_policy, uint8_t host_state)
{
    s_wifi_power_policy = power_policy;
    s_host_state = host_state;
    return wifi_service_apply_ps_mode();
}

esp_err_t wifi_service_store_credentials(const char *ssid, const char *password)
{
    esp_err_t err = app_config_wifi_set_credentials(ssid, password);

    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "Stored WiFi credentials into NVS");

    if (s_wifi_enabled != 0U)
    {
        err = wifi_service_apply_config();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Re-apply WiFi config after credential update failed: 0x%04X", (unsigned int)err);
            return err;
        }

        err = esp_wifi_disconnect();
        if (err != ESP_OK &&
            err != ESP_ERR_WIFI_NOT_STARTED &&
            err != ESP_ERR_WIFI_CONN)
        {
            ESP_LOGW(TAG, "esp_wifi_disconnect after credential update failed: 0x%04X", (unsigned int)err);
        }

        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
        {
            ESP_LOGE(TAG, "esp_wifi_connect after credential update failed: 0x%04X", (unsigned int)err);
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t wifi_service_clear_credentials(void)
{
    esp_err_t err = ESP_OK;

    if (s_wifi_enabled != 0U || s_wifi_started != 0U)
    {
        err = wifi_service_set_enabled(0U);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    err = app_config_wifi_clear_credentials();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Cleared WiFi credentials from NVS");
    }
    return err;
}

uint8_t wifi_service_is_enabled(void)
{
    return s_wifi_enabled;
}

uint8_t wifi_service_is_connected(void)
{
    return s_wifi_connected;
}

uint8_t wifi_service_has_credentials(void)
{
    return app_config_wifi_is_configured() ? 1U : 0U;
}

uint8_t wifi_service_needs_provisioning(void)
{
    return (wifi_service_has_credentials() == 0U) ? 1U : 0U;
}

void wifi_init_mode(void)
{
    (void)wifi_service_set_enabled(1U);
}

void WIFI_init_smartconfig(void)
{
}

void wifi_init_sta(void)
{
    (void)wifi_service_set_enabled(1U);
}

void wifi_init_softap(void)
{
}

esp_err_t CreateTcpServer(bool isCreatServer, uint16_t port)
{
    (void)isCreatServer;
    (void)port;
    return ESP_OK;
}
