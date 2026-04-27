#include "HTTP.h"

#include <stdlib.h>
#include <string.h>

#include "MQTT.h"
#include "cJSON.h"
#include "esp_log.h"

char dynamic_url[2048];
const char *TAG = "WIFI_HTTP";
char *http_response_buffer = NULL;
size_t http_response_len = 0;

static esp_err_t http_append_response_data(const char *data, int data_len)
{
    char *new_buffer = NULL;

    if (data == NULL || data_len <= 0)
    {
        return ESP_OK;
    }

    new_buffer = realloc(http_response_buffer, http_response_len + (size_t)data_len + 1U);
    if (new_buffer == NULL)
    {
        ESP_LOGE(TAG, "HTTP response buffer realloc failed");
        return ESP_ERR_NO_MEM;
    }

    http_response_buffer = new_buffer;
    memcpy(http_response_buffer + http_response_len, data, (size_t)data_len);
    http_response_len += (size_t)data_len;
    http_response_buffer[http_response_len] = '\0';
    return ESP_OK;
}

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        if (MQTT_OTA == true)
        {
            ESP_LOGI(MQTT_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        }
        else if (!esp_http_client_is_chunked_response(evt->client))
        {
            return http_append_response_data((const char *)evt->data, evt->data_len);
        }
        break;

    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTP event error");
        break;

    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP connected");
        break;

    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(TAG, "HTTP header sent");
        break;

    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(TAG,
                 "HTTP header key=%s value=%s",
                 evt->header_key != NULL ? evt->header_key : "(null)",
                 evt->header_value != NULL ? evt->header_value : "(null)");
        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP finished");
        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP disconnected");
        break;

    default:
        break;
    }

    return ESP_OK;
}

void weather_parse(const char *response)
{
    cJSON *root = NULL;
    cJSON *results = NULL;
    cJSON *result = NULL;
    cJSON *location = NULL;
    cJSON *name_item = NULL;
    cJSON *now = NULL;
    cJSON *text_item = NULL;
    cJSON *temp_item = NULL;
    cJSON *update_item = NULL;
    const char *city = "N/A";
    const char *weather = "N/A";
    const char *temperature = "N/A";
    char formatted_time[20] = "N/A";

    if (response == NULL)
    {
        ESP_LOGW(TAG, "weather_parse received null response");
        return;
    }

    root = cJSON_Parse(response);
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Weather JSON parse failed: %s", cJSON_GetErrorPtr());
        return;
    }

    results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0)
    {
        ESP_LOGE(TAG, "Weather response missing results array");
        cJSON_Delete(root);
        return;
    }

    result = cJSON_GetArrayItem(results, 0);
    location = cJSON_GetObjectItem(result, "location");
    if (location != NULL)
    {
        name_item = cJSON_GetObjectItem(location, "name");
        if (cJSON_IsString(name_item))
        {
            city = name_item->valuestring;
        }
    }

    now = cJSON_GetObjectItem(result, "now");
    if (now != NULL)
    {
        text_item = cJSON_GetObjectItem(now, "text");
        if (cJSON_IsString(text_item))
        {
            weather = text_item->valuestring;
        }

        temp_item = cJSON_GetObjectItem(now, "temperature");
        if (cJSON_IsString(temp_item))
        {
            temperature = temp_item->valuestring;
        }
    }

    update_item = cJSON_GetObjectItem(result, "last_update");
    if (cJSON_IsString(update_item))
    {
        strncpy(formatted_time, update_item->valuestring, 16U);
        formatted_time[10] = ' ';
        formatted_time[16] = '\0';
    }

    ESP_LOGI(TAG, "Weather city=%s weather=%s temp=%sC update=%s",
             city,
             weather,
             temperature,
             formatted_time);

    cJSON_Delete(root);
}
