#include "MQTT.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WIFI.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led.h"
#include "mbedtls/md.h"

extern const uint8_t root_crt_start[] asm("_binary_root_crt_start");

typedef struct
{
    int16_t min_temp_x10;
    int16_t max_temp_x10;
    int16_t center_temp_x10;
    uint8_t valid;
} mqtt_thermal_snapshot_t;

esp_mqtt_client_handle_t client = NULL;
bool mqtt_connected = false;
double temp = 0.0;
double humi = 0.0;
double gas = 0.0;
int g_publish_flag = 0;
bool MQTT_OTA = false;

static uint8_t s_service_inited = 0U;
static uint8_t s_client_created = 0U;
static uint8_t s_client_started = 0U;
static uint8_t s_logged_missing_config = 0U;
static uint32_t s_publish_seq = 1U;
static mqtt_thermal_snapshot_t s_pending_snapshot = {0};

static char s_mqtt_host[128];
static char s_mqtt_client_id[192];
static char s_mqtt_username[96];
static char s_mqtt_password[65];
static char s_topic_property_post[160];
static char s_topic_property_post_reply[160];
static char s_topic_property_set[160];

static uint8_t mqtt_service_is_configured(void)
{
    if (ALIYUN_MQTT_ENABLE == 0U)
    {
        return 0U;
    }

    if (ALIYUN_IOT_PRODUCT_KEY[0] == '\0' ||
        ALIYUN_IOT_DEVICE_NAME[0] == '\0' ||
        ALIYUN_IOT_DEVICE_SECRET[0] == '\0')
    {
        return 0U;
    }

    if (ALIYUN_IOT_MQTT_HOST[0] == '\0' && ALIYUN_IOT_REGION_ID[0] == '\0')
    {
        return 0U;
    }

    return 1U;
}

static void mqtt_service_log_missing_config_once(void)
{
    if (s_logged_missing_config != 0U)
    {
        return;
    }

    s_logged_missing_config = 1U;
    ESP_LOGW(MQTT_TAG,
             "Alibaba Cloud MQTT is not configured yet. Fill ProductKey/DeviceName/DeviceSecret in MQTT.h.");
}

static uint16_t mqtt_service_get_port(void)
{
    return (ALIYUN_MQTT_USE_TLS != 0U) ? ALIYUN_MQTT_PORT_TLS : ALIYUN_MQTT_PORT_TCP;
}

static const char *mqtt_service_transport_name(void)
{
    return (ALIYUN_MQTT_USE_TLS != 0U) ? "MQTTS" : "MQTT";
}

static uint8_t mqtt_service_secure_mode(void)
{
    return (ALIYUN_MQTT_USE_TLS != 0U) ? 2U : 3U;
}

static void mqtt_service_build_host(void)
{
    if (ALIYUN_IOT_MQTT_HOST[0] != '\0')
    {
        snprintf(s_mqtt_host, sizeof(s_mqtt_host), "%s", ALIYUN_IOT_MQTT_HOST);
        return;
    }

    snprintf(s_mqtt_host,
             sizeof(s_mqtt_host),
             "%s.iot-as-mqtt.%s.aliyuncs.com",
             ALIYUN_IOT_PRODUCT_KEY,
             ALIYUN_IOT_REGION_ID);
}

static void mqtt_service_build_topics(void)
{
    snprintf(s_topic_property_post,
             sizeof(s_topic_property_post),
             "/sys/%s/%s/thing/event/property/post",
             ALIYUN_IOT_PRODUCT_KEY,
             ALIYUN_IOT_DEVICE_NAME);
    snprintf(s_topic_property_post_reply,
             sizeof(s_topic_property_post_reply),
             "/sys/%s/%s/thing/event/property/post_reply",
             ALIYUN_IOT_PRODUCT_KEY,
             ALIYUN_IOT_DEVICE_NAME);
    snprintf(s_topic_property_set,
             sizeof(s_topic_property_set),
             "/sys/%s/%s/thing/service/property/set",
             ALIYUN_IOT_PRODUCT_KEY,
             ALIYUN_IOT_DEVICE_NAME);
}

static void mqtt_service_hex_encode(const uint8_t *input, size_t input_len, char *output, size_t output_len)
{
    static const char hex_table[] = "0123456789abcdef";
    size_t i = 0U;

    if (output_len == 0U)
    {
        return;
    }

    for (i = 0U; i < input_len && ((i * 2U) + 1U) < output_len; ++i)
    {
        output[i * 2U] = hex_table[(input[i] >> 4) & 0x0FU];
        output[(i * 2U) + 1U] = hex_table[input[i] & 0x0FU];
    }

    output[(i * 2U) < output_len ? (i * 2U) : (output_len - 1U)] = '\0';
}

static esp_err_t mqtt_service_build_auth(void)
{
    const mbedtls_md_info_t *md_info = NULL;
    mbedtls_md_context_t md_ctx;
    uint8_t digest[32];
    char timestamp_ms[24];
    char brief_client_id[96];
    char sign_content[256];
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    int ret = 0;
    int text_len = 0;

    memset(digest, 0, sizeof(digest));
    memset(&md_ctx, 0, sizeof(md_ctx));

    text_len = snprintf(timestamp_ms, sizeof(timestamp_ms), "%" PRIu64, now_ms);
    if (text_len < 0 || (size_t)text_len >= sizeof(timestamp_ms))
    {
        ESP_LOGE(MQTT_TAG, "timestamp buffer is too small");
        return ESP_FAIL;
    }

    text_len = snprintf(brief_client_id,
                        sizeof(brief_client_id),
                        "%s.%s",
                        ALIYUN_IOT_PRODUCT_KEY,
                        ALIYUN_IOT_DEVICE_NAME);
    if (text_len < 0 || (size_t)text_len >= sizeof(brief_client_id))
    {
        ESP_LOGE(MQTT_TAG, "brief client id buffer is too small");
        return ESP_FAIL;
    }

    text_len = snprintf(s_mqtt_client_id,
                        sizeof(s_mqtt_client_id),
                        "%s|securemode=%u,signmethod=hmacsha256,timestamp=%s|",
                        brief_client_id,
                        (unsigned int)mqtt_service_secure_mode(),
                        timestamp_ms);
    if (text_len < 0 || (size_t)text_len >= sizeof(s_mqtt_client_id))
    {
        ESP_LOGE(MQTT_TAG, "mqtt client id buffer is too small");
        return ESP_FAIL;
    }

    text_len = snprintf(s_mqtt_username,
                        sizeof(s_mqtt_username),
                        "%s&%s",
                        ALIYUN_IOT_DEVICE_NAME,
                        ALIYUN_IOT_PRODUCT_KEY);
    if (text_len < 0 || (size_t)text_len >= sizeof(s_mqtt_username))
    {
        ESP_LOGE(MQTT_TAG, "mqtt username buffer is too small");
        return ESP_FAIL;
    }

    text_len = snprintf(sign_content,
                        sizeof(sign_content),
                        "clientId%sdeviceName%sproductKey%stimestamp%s",
                        brief_client_id,
                        ALIYUN_IOT_DEVICE_NAME,
                        ALIYUN_IOT_PRODUCT_KEY,
                        timestamp_ms);
    if (text_len < 0 || (size_t)text_len >= sizeof(sign_content))
    {
        ESP_LOGE(MQTT_TAG, "mqtt sign content buffer is too small");
        return ESP_FAIL;
    }

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL)
    {
        ESP_LOGE(MQTT_TAG, "SHA256 provider unavailable for Alibaba MQTT auth");
        return ESP_FAIL;
    }

    ret = mbedtls_md_setup(&md_ctx, md_info, 1);
    if (ret != 0)
    {
        ESP_LOGE(MQTT_TAG, "mbedtls_md_setup failed: %d", ret);
        mbedtls_md_free(&md_ctx);
        return ESP_FAIL;
    }

    ret = mbedtls_md_hmac_starts(&md_ctx,
                                 (const unsigned char *)ALIYUN_IOT_DEVICE_SECRET,
                                 strlen(ALIYUN_IOT_DEVICE_SECRET));
    if (ret == 0)
    {
        ret = mbedtls_md_hmac_update(&md_ctx,
                                     (const unsigned char *)sign_content,
                                     strlen(sign_content));
    }
    if (ret == 0)
    {
        ret = mbedtls_md_hmac_finish(&md_ctx, digest);
    }
    mbedtls_md_free(&md_ctx);

    if (ret != 0)
    {
        ESP_LOGE(MQTT_TAG, "Alibaba MQTT password HMAC failed: %d", ret);
        return ESP_FAIL;
    }

    mqtt_service_hex_encode(digest, sizeof(digest), s_mqtt_password, sizeof(s_mqtt_password));
    return ESP_OK;
}

static void mqtt_service_log_data_event(esp_mqtt_event_handle_t event)
{
    char topic[128];
    char data[256];
    int topic_len = 0;
    int data_len = 0;

    if (event == NULL)
    {
        return;
    }

    topic_len = (event->topic_len < (int)(sizeof(topic) - 1U)) ? event->topic_len : (int)(sizeof(topic) - 1U);
    data_len = (event->data_len < (int)(sizeof(data) - 1U)) ? event->data_len : (int)(sizeof(data) - 1U);

    if (topic_len > 0)
    {
        memcpy(topic, event->topic, (size_t)topic_len);
    }
    if (data_len > 0)
    {
        memcpy(data, event->data, (size_t)data_len);
    }
    topic[topic_len] = '\0';
    data[data_len] = '\0';

    ESP_LOGI(MQTT_TAG, "MQTT RX topic=%s payload=%s", topic, data);
}

static void mqtt_service_handle_property_set(esp_mqtt_event_handle_t event)
{
    cJSON *root = NULL;
    cJSON *params = NULL;
    cJSON *led_status = NULL;
    char *payload = NULL;

    if (event == NULL || event->data_len <= 0)
    {
        return;
    }

    payload = (char *)malloc((size_t)event->data_len + 1U);
    if (payload == NULL)
    {
        ESP_LOGE(MQTT_TAG, "malloc failed while parsing property set payload");
        return;
    }

    memcpy(payload, event->data, (size_t)event->data_len);
    payload[event->data_len] = '\0';

    root = cJSON_Parse(payload);
    if (root == NULL)
    {
        ESP_LOGW(MQTT_TAG, "Property set JSON parse failed");
        free(payload);
        return;
    }

    params = cJSON_GetObjectItem(root, "params");
    led_status = (params != NULL) ? cJSON_GetObjectItem(params, "LED") : NULL;
    if (cJSON_IsNumber(led_status))
    {
        gpio_set_level(LED_GPIO_PIN, (led_status->valueint != 0) ? 0 : 1);
        ESP_LOGI(MQTT_TAG, "LED property applied: %d", led_status->valueint);
    }

    cJSON_Delete(root);
    free(payload);
}

static esp_err_t mqtt_service_publish_json(const char *topic, const char *payload)
{
    int msg_id = 0;

    if (client == NULL || mqtt_connected == false || topic == NULL || payload == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    msg_id = esp_mqtt_client_publish(client, topic, payload, 0, ALIYUN_MQTT_QOS, 0);
    if (msg_id < 0)
    {
        g_publish_flag = 0;
        ESP_LOGE(MQTT_TAG, "MQTT publish failed, topic=%s", topic);
        return ESP_FAIL;
    }

    g_publish_flag = 1;
    ESP_LOGI(MQTT_TAG, "MQTT publish queued, msg_id=%d topic=%s", msg_id, topic);
    return ESP_OK;
}

static void mqtt_service_add_property(cJSON *params,
                                      const char *identifier,
                                      float value)
{
    cJSON *property = NULL;

    if (params == NULL || identifier == NULL)
    {
        return;
    }

    property = cJSON_CreateObject();
    if (property == NULL)
    {
        return;
    }

    cJSON_AddNumberToObject(property, "value", value);
    cJSON_AddItemToObject(params, identifier, property);
}

static esp_err_t mqtt_service_publish_pending_snapshot(void)
{
    cJSON *root = NULL;
    cJSON *params = NULL;
    cJSON *sys = NULL;
    char *json_str = NULL;
    char id_buffer[16];
    float min_temp = 0.0f;
    float max_temp = 0.0f;
    float center_temp = 0.0f;
    esp_err_t err = ESP_OK;

    if (s_pending_snapshot.valid == 0U)
    {
        return ESP_OK;
    }

    if (mqtt_connected == false || client == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    min_temp = ((float)s_pending_snapshot.min_temp_x10) / 10.0f;
    max_temp = ((float)s_pending_snapshot.max_temp_x10) / 10.0f;
    center_temp = ((float)s_pending_snapshot.center_temp_x10) / 10.0f;

    root = cJSON_CreateObject();
    params = cJSON_CreateObject();
    sys = cJSON_CreateObject();
    if (root == NULL || params == NULL || sys == NULL)
    {
        ESP_LOGE(MQTT_TAG, "cJSON allocation failed while building thermal snapshot");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    snprintf(id_buffer, sizeof(id_buffer), "%" PRIu32, s_publish_seq++);
    cJSON_AddStringToObject(root, "id", id_buffer);
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddNumberToObject(sys, "ack", 1);
    cJSON_AddItemToObject(root, "sys", sys);
    cJSON_AddItemToObject(root, "params", params);
    cJSON_AddStringToObject(root, "method", "thing.event.property.post");

    mqtt_service_add_property(params, ALIYUN_THERMAL_PROP_MIN_TEMP, min_temp);
    mqtt_service_add_property(params, ALIYUN_THERMAL_PROP_MAX_TEMP, max_temp);
    mqtt_service_add_property(params, ALIYUN_THERMAL_PROP_CENTER_TEMP, center_temp);

    json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        ESP_LOGE(MQTT_TAG, "cJSON_PrintUnformatted failed while building thermal snapshot");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ESP_LOGI(MQTT_TAG,
             "THERMAL cloud post min=%.1fC max=%.1fC center=%.1fC",
             min_temp,
             max_temp,
             center_temp);
    err = mqtt_service_publish_json(s_topic_property_post, json_str);
    if (err == ESP_OK)
    {
        s_pending_snapshot.valid = 0U;
    }

cleanup:
    if (json_str != NULL)
    {
        free(json_str);
    }
    if (root != NULL)
    {
        cJSON_Delete(root);
    }
    else
    {
        if (params != NULL)
        {
            cJSON_Delete(params);
        }
        if (sys != NULL)
        {
            cJSON_Delete(sys);
        }
    }
    return err;
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    int msg_id = 0;

    (void)args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;
        ESP_LOGI(MQTT_TAG,
                 "%s connected host=%s port=%u",
                 mqtt_service_transport_name(),
                 s_mqtt_host,
                 (unsigned int)mqtt_service_get_port());
        msg_id = esp_mqtt_client_subscribe(client, s_topic_property_set, ALIYUN_MQTT_QOS);
        ESP_LOGI(MQTT_TAG, "Subscribe property/set, msg_id=%d", msg_id);
        msg_id = esp_mqtt_client_subscribe(client, s_topic_property_post_reply, ALIYUN_MQTT_QOS);
        ESP_LOGI(MQTT_TAG, "Subscribe property/post_reply, msg_id=%d", msg_id);
        (void)mqtt_service_publish_pending_snapshot();
        break;

    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        ESP_LOGW(MQTT_TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(MQTT_TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(MQTT_TAG, "MQTT published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        mqtt_service_log_data_event(event);
        if (event->topic_len == (int)strlen(s_topic_property_set) &&
            strncmp(event->topic, s_topic_property_set, (size_t)event->topic_len) == 0)
        {
            mqtt_service_handle_property_set(event);
        }
        break;

    case MQTT_EVENT_ERROR:
        mqtt_connected = false;
        ESP_LOGE(MQTT_TAG, "MQTT error event");
        if (event != NULL &&
            event->error_handle != NULL &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(MQTT_TAG,
                     "Transport errno=%d",
                     event->error_handle->esp_transport_sock_errno);
        }
        break;

    default:
        ESP_LOGD(MQTT_TAG, "MQTT event id=%" PRId32, event_id);
        break;
    }
}

static esp_err_t mqtt_service_create_client_if_needed(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};

    if (s_client_created != 0U)
    {
        return ESP_OK;
    }

    mqtt_service_build_host();
    mqtt_service_build_topics();
    if (mqtt_service_build_auth() != ESP_OK)
    {
        return ESP_FAIL;
    }

    mqtt_cfg.broker.address.hostname = s_mqtt_host;
    mqtt_cfg.broker.address.port = mqtt_service_get_port();
    mqtt_cfg.broker.address.transport = (ALIYUN_MQTT_USE_TLS != 0U) ? MQTT_TRANSPORT_OVER_SSL
                                                                    : MQTT_TRANSPORT_OVER_TCP;
    mqtt_cfg.credentials.client_id = s_mqtt_client_id;
    mqtt_cfg.credentials.username = s_mqtt_username;
    mqtt_cfg.credentials.authentication.password = s_mqtt_password;
    mqtt_cfg.session.keepalive = ALIYUN_MQTT_KEEPALIVE_SEC;
    if (ALIYUN_MQTT_USE_TLS != 0U)
    {
        mqtt_cfg.broker.verification.certificate = (const char *)root_crt_start;
    }

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL)
    {
        ESP_LOGE(MQTT_TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    s_client_created = 1U;
    ESP_LOGI(MQTT_TAG,
             "MQTT client ready host=%s port=%u tls=%u",
             s_mqtt_host,
             (unsigned int)mqtt_service_get_port(),
             (unsigned int)((ALIYUN_MQTT_USE_TLS != 0U) ? 1U : 0U));
    return ESP_OK;
}

static void mqtt_service_stop_if_needed(void)
{
    if (client == NULL || s_client_started == 0U)
    {
        return;
    }

    if (esp_mqtt_client_stop(client) == ESP_OK)
    {
        ESP_LOGI(MQTT_TAG, "MQTT client stopped because WiFi is disabled");
    }
    else
    {
        ESP_LOGW(MQTT_TAG, "esp_mqtt_client_stop failed");
    }
    mqtt_connected = false;
    s_client_started = 0U;
}

static esp_err_t mqtt_service_start_if_needed(void)
{
    esp_err_t err = ESP_OK;

    if (s_client_started != 0U)
    {
        return ESP_OK;
    }

    err = esp_mqtt_client_start(client);
    if (err != ESP_OK)
    {
        ESP_LOGE(MQTT_TAG, "esp_mqtt_client_start failed: 0x%04X", (unsigned int)err);
        return err;
    }

    s_client_started = 1U;
    ESP_LOGI(MQTT_TAG, "MQTT client start requested");
    return ESP_OK;
}

esp_err_t mqtt_service_init(void)
{
    if (s_service_inited != 0U)
    {
        return ESP_OK;
    }

    s_service_inited = 1U;
    s_client_created = 0U;
    s_client_started = 0U;
    s_logged_missing_config = 0U;
    s_publish_seq = 1U;
    s_pending_snapshot.valid = 0U;
    mqtt_connected = false;
    g_publish_flag = 0;
    client = NULL;

    if (mqtt_service_is_configured() == 0U)
    {
        mqtt_service_log_missing_config_once();
        return ESP_OK;
    }

    ESP_LOGI(MQTT_TAG,
             "Alibaba MQTT service init, endpoint=%s:%u",
             (ALIYUN_IOT_MQTT_HOST[0] != '\0') ? ALIYUN_IOT_MQTT_HOST : "(derived from productKey+region)",
             (unsigned int)mqtt_service_get_port());
    return ESP_OK;
}

void mqtt_service_step(void)
{
    if (s_service_inited == 0U)
    {
        return;
    }

    if (mqtt_service_is_configured() == 0U)
    {
        return;
    }

    if (wifi_service_is_enabled() == 0U)
    {
        mqtt_service_stop_if_needed();
        return;
    }

    if (mqtt_service_create_client_if_needed() != ESP_OK)
    {
        return;
    }

    if (wifi_service_is_connected() == 0U)
    {
        return;
    }

    if (mqtt_service_start_if_needed() != ESP_OK)
    {
        return;
    }

    if (mqtt_connected != false && s_pending_snapshot.valid != 0U)
    {
        (void)mqtt_service_publish_pending_snapshot();
    }
}

esp_err_t mqtt_service_submit_thermal_snapshot_x10(int16_t min_temp_x10,
                                                   int16_t max_temp_x10,
                                                   int16_t center_temp_x10)
{
    esp_err_t err = ESP_OK;

    if (s_service_inited == 0U)
    {
        err = mqtt_service_init();
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (mqtt_service_is_configured() == 0U)
    {
        mqtt_service_log_missing_config_once();
        return ESP_ERR_INVALID_STATE;
    }

    s_pending_snapshot.min_temp_x10 = min_temp_x10;
    s_pending_snapshot.max_temp_x10 = max_temp_x10;
    s_pending_snapshot.center_temp_x10 = center_temp_x10;
    s_pending_snapshot.valid = 1U;

    if (wifi_service_is_enabled() == 0U)
    {
        if (wifi_service_has_credentials() == 0U)
        {
            ESP_LOGE(MQTT_TAG, "WiFi credentials are empty, cannot publish thermal snapshot");
            return ESP_ERR_INVALID_STATE;
        }

        err = wifi_service_set_enabled(1U);
        if (err != ESP_OK)
        {
            ESP_LOGE(MQTT_TAG, "wifi_service_set_enabled(1) failed: 0x%04X", (unsigned int)err);
            return err;
        }
    }

    mqtt_service_step();
    return ESP_OK;
}

void mqtt_app_init(void)
{
    (void)mqtt_service_init();
}

void mqtt_app_init_1(void)
{
    (void)mqtt_service_init();
}

void publish_sensor_data(uint16_t ir, uint16_t als, uint16_t ps)
{
    cJSON *root = NULL;
    cJSON *params = NULL;
    cJSON *sys = NULL;
    char *json_str = NULL;
    char id_buffer[16];
    if (mqtt_connected == false || client == NULL)
    {
        ESP_LOGW(MQTT_TAG, "publish_sensor_data skipped because MQTT is not connected");
        return;
    }

    root = cJSON_CreateObject();
    params = cJSON_CreateObject();
    sys = cJSON_CreateObject();
    if (root == NULL || params == NULL || sys == NULL)
    {
        ESP_LOGE(MQTT_TAG, "cJSON allocation failed in publish_sensor_data");
        goto cleanup;
    }

    snprintf(id_buffer, sizeof(id_buffer), "%" PRIu32, s_publish_seq++);
    cJSON_AddStringToObject(root, "id", id_buffer);
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddNumberToObject(sys, "ack", 1);
    cJSON_AddItemToObject(root, "sys", sys);
    cJSON_AddItemToObject(root, "params", params);
    cJSON_AddStringToObject(root, "method", "thing.event.property.post");

    mqtt_service_add_property(params, "IR", (float)ir);
    mqtt_service_add_property(params, "als", (float)als);
    mqtt_service_add_property(params, "ps", (float)ps);

    json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        ESP_LOGE(MQTT_TAG, "cJSON_PrintUnformatted failed in publish_sensor_data");
        goto cleanup;
    }

    (void)mqtt_service_publish_json(s_topic_property_post, json_str);

cleanup:
    if (json_str != NULL)
    {
        free(json_str);
    }
    if (root != NULL)
    {
        cJSON_Delete(root);
    }
    else
    {
        if (params != NULL)
        {
            cJSON_Delete(params);
        }
        if (sys != NULL)
        {
            cJSON_Delete(sys);
        }
    }
}
