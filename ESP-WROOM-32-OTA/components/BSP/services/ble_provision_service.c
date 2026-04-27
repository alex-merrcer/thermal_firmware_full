#include "ble_provision_service.h"

#include <stdio.h>
#include <string.h>

#include "WIFI.h"
#include "app_config.h"
#include "cJSON.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "host_ctrl_service.h"

static const char *TAG = "BLE_PROV";

#define BLE_PROV_APP_ID               0x42U
#define BLE_PROV_LOCAL_MTU            185U
#define BLE_PROV_STATUS_MAX_LEN       20U
#define BLE_PROV_RX_BUFFER_LEN        192U
#define BLE_PROV_DEVICE_NAME          "RedPic1 Setup"
#define BLE_PROV_ADV_CONFIG_FLAG      (1U << 0)
#define BLE_PROV_SCAN_RSP_CONFIG_FLAG (1U << 1)

enum
{
    BLE_PROV_IDX_SVC = 0,
    BLE_PROV_IDX_CHAR_TX,
    BLE_PROV_IDX_CHAR_VAL_TX,
    BLE_PROV_IDX_CHAR_CFG_TX,
    BLE_PROV_IDX_CHAR_RX,
    BLE_PROV_IDX_CHAR_VAL_RX,
    BLE_PROV_IDX_NB
};

static const uint8_t s_service_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
};

static const uint8_t s_tx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
};

static const uint8_t s_rx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
};

static const uint16_t s_primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t s_character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t s_tx_properties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t s_rx_properties = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t s_tx_ccc_default[2] = {0x00, 0x00};
static const uint8_t s_rx_default_value[1] = {0x00};

static char s_status_value[BLE_PROV_STATUS_MAX_LEN + 1U] = "READY|0|0|0";
static char s_rx_buffer[BLE_PROV_RX_BUFFER_LEN];
static size_t s_rx_length = 0U;
static uint16_t s_handle_table[BLE_PROV_IDX_NB] = {0};
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0U;
static uint8_t s_enabled = 0U;
static uint8_t s_connected = 0U;
static uint8_t s_notify_enabled = 0U;
static uint8_t s_waiting_wifi_result = 0U;
static uint8_t s_stack_registered = 0U;
static uint8_t s_attr_table_ready = 0U;
static uint8_t s_service_started = 0U;
static uint8_t s_adv_config_state = 0U;
static uint8_t s_advertising = 0U;

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(s_service_uuid),
    .p_service_uuid = (uint8_t *)s_service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x40,
    .adv_int_max = 0x80,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static const esp_gatts_attr_db_t s_gatt_db[BLE_PROV_IDX_NB] = {
    [BLE_PROV_IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&s_primary_service_uuid,
            ESP_GATT_PERM_READ,
            sizeof(s_service_uuid),
            sizeof(s_service_uuid),
            (uint8_t *)s_service_uuid,
        },
    },

    [BLE_PROV_IDX_CHAR_TX] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&s_character_declaration_uuid,
            ESP_GATT_PERM_READ,
            sizeof(s_tx_properties),
            sizeof(s_tx_properties),
            (uint8_t *)&s_tx_properties,
        },
    },

    [BLE_PROV_IDX_CHAR_VAL_TX] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_128,
            (uint8_t *)s_tx_uuid,
            ESP_GATT_PERM_READ,
            BLE_PROV_STATUS_MAX_LEN,
            11,
            (uint8_t *)s_status_value,
        },
    },

    [BLE_PROV_IDX_CHAR_CFG_TX] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&s_character_client_config_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            sizeof(s_tx_ccc_default),
            sizeof(s_tx_ccc_default),
            (uint8_t *)s_tx_ccc_default,
        },
    },

    [BLE_PROV_IDX_CHAR_RX] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&s_character_declaration_uuid,
            ESP_GATT_PERM_READ,
            sizeof(s_rx_properties),
            sizeof(s_rx_properties),
            (uint8_t *)&s_rx_properties,
        },
    },

    [BLE_PROV_IDX_CHAR_VAL_RX] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_128,
            (uint8_t *)s_rx_uuid,
            ESP_GATT_PERM_WRITE,
            BLE_PROV_RX_BUFFER_LEN,
            sizeof(s_rx_default_value),
            (uint8_t *)s_rx_default_value,
        },
    },
};

static void ble_provision_publish_status(const char *text);

static void ble_provision_refresh_ready_status(void)
{
    snprintf(s_status_value,
             sizeof(s_status_value),
             "READY|%u|%u|%u",
             (unsigned int)wifi_service_has_credentials(),
             (unsigned int)wifi_service_is_connected(),
             (unsigned int)s_waiting_wifi_result);
}

static void ble_provision_request_runtime_apply(void)
{
    host_ctrl_service_request_runtime_apply();
}

static esp_err_t ble_provision_start_advertising(void)
{
    esp_err_t err = ESP_OK;

    if (s_enabled == 0U || s_connected != 0U || s_service_started == 0U)
    {
        return ESP_OK;
    }
    if ((s_adv_config_state & (BLE_PROV_ADV_CONFIG_FLAG | BLE_PROV_SCAN_RSP_CONFIG_FLAG)) != 0U)
    {
        return ESP_OK;
    }
    if (s_advertising != 0U)
    {
        return ESP_OK;
    }

    err = esp_ble_gap_start_advertising(&s_adv_params);
    if (err == ESP_OK)
    {
        s_advertising = 1U;
        ESP_LOGI(TAG, "BLE provisioning advertising started");
    }
    else
    {
        ESP_LOGW(TAG, "Start advertising failed: %s", esp_err_to_name(err));
    }

    return err;
}

static void ble_provision_publish_status(const char *text)
{
    size_t len = 0U;

    if (text == NULL)
    {
        return;
    }

    len = strlen(text);
    if (len > BLE_PROV_STATUS_MAX_LEN)
    {
        len = BLE_PROV_STATUS_MAX_LEN;
    }

    memcpy(s_status_value, text, len);
    s_status_value[len] = '\0';

    if (s_attr_table_ready != 0U && s_handle_table[BLE_PROV_IDX_CHAR_VAL_TX] != 0U)
    {
        (void)esp_ble_gatts_set_attr_value(s_handle_table[BLE_PROV_IDX_CHAR_VAL_TX],
                                           (uint16_t)len,
                                           (const uint8_t *)s_status_value);
    }

    if (s_enabled != 0U && s_connected != 0U && s_notify_enabled != 0U && s_gatts_if != ESP_GATT_IF_NONE)
    {
        esp_err_t err = esp_ble_gatts_send_indicate(s_gatts_if,
                                                    s_conn_id,
                                                    s_handle_table[BLE_PROV_IDX_CHAR_VAL_TX],
                                                    (uint16_t)len,
                                                    (uint8_t *)s_status_value,
                                                    false);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Notify status failed: %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "BLE status: %s", s_status_value);
}

static void ble_provision_publish_ready(void)
{
    ble_provision_refresh_ready_status();
    ble_provision_publish_status(s_status_value);
}

static void ble_provision_handle_set_wifi(cJSON *root)
{
    cJSON *ssid_item = NULL;
    cJSON *password_item = NULL;
    const char *ssid = NULL;
    const char *password = NULL;
    esp_err_t err = ESP_OK;

    ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    password_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == NULL ||
        !cJSON_IsString(password_item) || password_item->valuestring == NULL)
    {
        ble_provision_publish_status("ERR|ARGS");
        return;
    }

    ssid = ssid_item->valuestring;
    password = password_item->valuestring;
    if (ssid[0] == '\0' || password[0] == '\0')
    {
        ble_provision_publish_status("ERR|ARGS");
        return;
    }

    ble_provision_publish_status("SAVING");
    err = wifi_service_store_credentials(ssid, password);
    if (err != ESP_OK)
    {
        ble_provision_publish_status("ERR|STORE");
        return;
    }

    s_waiting_wifi_result = 1U;
    ble_provision_publish_status("SAVED");
    host_ctrl_service_request_wifi(1U);
    ble_provision_publish_status("CONNECTING");
}

static void ble_provision_handle_clear_wifi(void)
{
    esp_err_t err = wifi_service_clear_credentials();

    if (err != ESP_OK)
    {
        ble_provision_publish_status("ERR|CLEAR");
        return;
    }

    s_waiting_wifi_result = 0U;
    host_ctrl_service_request_wifi(0U);
    ble_provision_publish_status("CLEARED");
    ble_provision_request_runtime_apply();
}

static void ble_provision_handle_command_line(const char *line)
{
    cJSON *root = NULL;
    cJSON *cmd_item = NULL;
    const char *cmd = NULL;

    if (line == NULL || line[0] == '\0')
    {
        return;
    }

    root = cJSON_Parse(line);
    if (root == NULL)
    {
        ble_provision_publish_status("ERR|JSON");
        return;
    }

    cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd_item) || cmd_item->valuestring == NULL)
    {
        cJSON_Delete(root);
        ble_provision_publish_status("ERR|CMD");
        return;
    }

    cmd = cmd_item->valuestring;
    if (strcmp(cmd, "status") == 0)
    {
        ble_provision_publish_ready();
    }
    else if (strcmp(cmd, "set_wifi") == 0)
    {
        ble_provision_handle_set_wifi(root);
    }
    else if (strcmp(cmd, "clear_wifi") == 0)
    {
        ble_provision_handle_clear_wifi();
    }
    else
    {
        ble_provision_publish_status("ERR|CMD");
    }

    cJSON_Delete(root);
}

static void ble_provision_feed_rx_data(const uint8_t *value, size_t len)
{
    size_t i = 0U;

    if (value == NULL || len == 0U)
    {
        return;
    }

    for (i = 0U; i < len; ++i)
    {
        char ch = (char)value[i];

        if (ch == '\r')
        {
            continue;
        }

        if (ch == '\n')
        {
            if (s_rx_length != 0U)
            {
                s_rx_buffer[s_rx_length] = '\0';
                ble_provision_handle_command_line(s_rx_buffer);
                s_rx_length = 0U;
            }
            continue;
        }

        if ((s_rx_length + 1U) >= sizeof(s_rx_buffer))
        {
            s_rx_length = 0U;
            ble_provision_publish_status("ERR|LONG");
            continue;
        }

        s_rx_buffer[s_rx_length++] = ch;
    }
}

static void ble_provision_gap_callback(esp_gap_ble_cb_event_t event,
                                       esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_config_state &= (uint8_t)(~BLE_PROV_ADV_CONFIG_FLAG);
        (void)ble_provision_start_advertising();
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        s_adv_config_state &= (uint8_t)(~BLE_PROV_SCAN_RSP_CONFIG_FLAG);
        (void)ble_provision_start_advertising();
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param != NULL && param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            s_advertising = 0U;
            ESP_LOGW(TAG, "Advertising start failed: %u", (unsigned int)param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_advertising = 0U;
        break;

    default:
        break;
    }
}

static void ble_provision_gatts_callback(esp_gatts_cb_event_t event,
                                         esp_gatt_if_t gatts_if,
                                         esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTS_REG_EVT:
        if (param == NULL || param->reg.status != ESP_GATT_OK)
        {
            ESP_LOGW(TAG, "GATTS register failed");
            break;
        }

        s_gatts_if = gatts_if;
        s_adv_config_state = BLE_PROV_ADV_CONFIG_FLAG | BLE_PROV_SCAN_RSP_CONFIG_FLAG;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_gap_set_device_name(BLE_PROV_DEVICE_NAME));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_gap_config_adv_data(&s_adv_data));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_gap_config_adv_data(&s_scan_rsp_data));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_gatts_create_attr_tab(s_gatt_db,
                                                                    gatts_if,
                                                                    BLE_PROV_IDX_NB,
                                                                    0));
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param == NULL ||
            param->add_attr_tab.status != ESP_GATT_OK ||
            param->add_attr_tab.num_handle != BLE_PROV_IDX_NB)
        {
            ESP_LOGW(TAG, "Create attribute table failed");
            break;
        }

        memcpy(s_handle_table, param->add_attr_tab.handles, sizeof(s_handle_table));
        s_attr_table_ready = 1U;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_gatts_start_service(s_handle_table[BLE_PROV_IDX_SVC]));
        (void)esp_ble_gatts_set_attr_value(s_handle_table[BLE_PROV_IDX_CHAR_VAL_TX],
                                           (uint16_t)strlen(s_status_value),
                                           (const uint8_t *)s_status_value);
        break;

    case ESP_GATTS_START_EVT:
        if (param != NULL && param->start.status == ESP_GATT_OK)
        {
            s_service_started = 1U;
            (void)ble_provision_start_advertising();
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        if (param == NULL)
        {
            break;
        }
        s_connected = 1U;
        s_notify_enabled = 0U;
        s_conn_id = param->connect.conn_id;
        s_advertising = 0U;
        ble_provision_request_runtime_apply();
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = 0U;
        s_notify_enabled = 0U;
        s_conn_id = 0U;
        ble_provision_request_runtime_apply();
        if (s_enabled != 0U)
        {
            (void)ble_provision_start_advertising();
        }
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param == NULL)
        {
            break;
        }

        if (param->write.handle == s_handle_table[BLE_PROV_IDX_CHAR_CFG_TX] &&
            param->write.len == 2U)
        {
            uint16_t ccc = (uint16_t)param->write.value[0] |
                           ((uint16_t)param->write.value[1] << 8);

            s_notify_enabled = (ccc != 0U) ? 1U : 0U;
            if (s_notify_enabled != 0U)
            {
                ble_provision_publish_ready();
            }
        }
        else if (param->write.handle == s_handle_table[BLE_PROV_IDX_CHAR_VAL_RX] &&
                 param->write.is_prep == 0)
        {
            ble_provision_feed_rx_data(param->write.value, param->write.len);
        }
        break;

    default:
        break;
    }
}

static esp_err_t ble_provision_enable_stack(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = ESP_OK;

    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE)
    {
        err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED)
    {
        err = esp_bluedroid_init();
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED)
    {
        err = esp_bluedroid_enable();
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (s_stack_registered == 0U)
    {
        err = esp_ble_gap_register_callback(ble_provision_gap_callback);
        if (err != ESP_OK)
        {
            return err;
        }

        err = esp_ble_gatts_register_callback(ble_provision_gatts_callback);
        if (err != ESP_OK)
        {
            return err;
        }

        err = esp_ble_gatts_app_register(BLE_PROV_APP_ID);
        if (err != ESP_OK)
        {
            return err;
        }

        err = esp_ble_gatt_set_local_mtu(BLE_PROV_LOCAL_MTU);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "Set local MTU failed: %s", esp_err_to_name(err));
        }

        s_stack_registered = 1U;
    }

    return ESP_OK;
}

static void ble_provision_reset_session_state(void)
{
    s_connected = 0U;
    s_notify_enabled = 0U;
    s_conn_id = 0U;
    s_rx_length = 0U;
    s_attr_table_ready = 0U;
    s_service_started = 0U;
    s_adv_config_state = 0U;
    s_advertising = 0U;
    s_gatts_if = ESP_GATT_IF_NONE;
    memset(s_handle_table, 0, sizeof(s_handle_table));
}

void ble_provision_service_init(void)
{
    s_enabled = 0U;
    s_waiting_wifi_result = 0U;
    s_stack_registered = 0U;
    ble_provision_reset_session_state();
    ble_provision_refresh_ready_status();
    ESP_LOGI(TAG, "BLE provisioning service initialized");
}

void ble_provision_service_start(void)
{
    if (APP_FEATURE_ENABLE_BLE_PROVISION_STARTUP != 0U)
    {
        ESP_LOGI(TAG, "BLE provisioning startup flag is enabled");
    }
    else
    {
        ESP_LOGI(TAG, "BLE provisioning waits for host_ctrl phase-6 gating");
    }
}

esp_err_t ble_provision_service_set_enabled(uint8_t enabled)
{
    esp_err_t err = ESP_OK;
    esp_bluedroid_status_t bluedroid_status = ESP_BLUEDROID_STATUS_UNINITIALIZED;
    esp_bt_controller_status_t controller_status = ESP_BT_CONTROLLER_STATUS_IDLE;

    if (enabled != 0U)
    {
        if (s_enabled != 0U)
        {
            return ESP_OK;
        }

        err = ble_provision_enable_stack();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Enable BLE provisioning stack failed: %s", esp_err_to_name(err));
            return err;
        }

        s_enabled = 1U;
        ble_provision_publish_ready();
        return ESP_OK;
    }

    if (s_enabled == 0U)
    {
        return ESP_OK;
    }

    s_enabled = 0U;
    s_notify_enabled = 0U;
    if (s_advertising != 0U)
    {
        err = esp_ble_gap_stop_advertising();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "Stop advertising failed: %s", esp_err_to_name(err));
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED)
    {
        err = esp_bluedroid_disable();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Disable bluedroid failed: %s", esp_err_to_name(err));
        }
    }
    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status != ESP_BLUEDROID_STATUS_UNINITIALIZED)
    {
        err = esp_bluedroid_deinit();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Deinit bluedroid failed: %s", esp_err_to_name(err));
        }
    }

    controller_status = esp_bt_controller_get_status();
    if (controller_status == ESP_BT_CONTROLLER_STATUS_ENABLED)
    {
        err = esp_bt_controller_disable();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Disable BT controller failed: %s", esp_err_to_name(err));
        }
    }
    controller_status = esp_bt_controller_get_status();
    if (controller_status == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        err = esp_bt_controller_deinit();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Deinit BT controller failed: %s", esp_err_to_name(err));
        }
    }

    s_stack_registered = 0U;
    ble_provision_reset_session_state();
    return ESP_OK;
}

uint8_t ble_provision_service_should_force_ble(void)
{
    return (uint8_t)((s_connected != 0U || s_waiting_wifi_result != 0U) ? 1U : 0U);
}

void ble_provision_service_on_wifi_connected(void)
{
    s_waiting_wifi_result = 0U;
    ble_provision_publish_status("CONNECTED");
    ble_provision_request_runtime_apply();
}

void ble_provision_service_on_wifi_disconnected(uint16_t reason)
{
    char message[BLE_PROV_STATUS_MAX_LEN + 1U] = {0};

    snprintf(message, sizeof(message), "DISC|%u", (unsigned int)reason);
    ble_provision_publish_status(message);
    ble_provision_request_runtime_apply();
}
