#include "app_service_bus.h"

#include "esp_log.h"
#include "ota_stm32_internal.h"

#define APP_CLOUD_EVENT_QUEUE_LENGTH 16U
#define APP_HOST_FRAME_QUEUE_LENGTH 8U
#define APP_OTA_FRAME_QUEUE_LENGTH 4U

static EventGroupHandle_t s_app_event_group = NULL;
static QueueHandle_t s_cloud_queue = NULL;
static QueueHandle_t s_host_frame_queue = NULL;
static QueueHandle_t s_ota_frame_queue = NULL;

esp_err_t app_service_bus_init(void)
{
    if (s_app_event_group == NULL)
    {
        s_app_event_group = xEventGroupCreate();
        if (s_app_event_group == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_cloud_queue == NULL)
    {
        s_cloud_queue = xQueueCreate(APP_CLOUD_EVENT_QUEUE_LENGTH, sizeof(cloud_event_t));
        if (s_cloud_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_host_frame_queue == NULL)
    {
        s_host_frame_queue = xQueueCreate(APP_HOST_FRAME_QUEUE_LENGTH, sizeof(ota_ctrl_frame_t));
        if (s_host_frame_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_ota_frame_queue == NULL)
    {
        s_ota_frame_queue = xQueueCreate(APP_OTA_FRAME_QUEUE_LENGTH, sizeof(ota_ctrl_frame_t));
        if (s_ota_frame_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

EventGroupHandle_t app_service_bus_event_group(void)
{
    return s_app_event_group;
}

QueueHandle_t app_service_bus_cloud_queue(void)
{
    return s_cloud_queue;
}

QueueHandle_t app_service_bus_host_frame_queue(void)
{
    return s_host_frame_queue;
}

QueueHandle_t app_service_bus_ota_frame_queue(void)
{
    return s_ota_frame_queue;
}

esp_err_t app_service_bus_submit_cloud_event(const cloud_event_t *event)
{
    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_cloud_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return (xQueueSendToBack(s_cloud_queue, event, 0) == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t app_service_bus_submit_thermal_snapshot_x10(int16_t min_temp_x10,
                                                      int16_t max_temp_x10,
                                                      int16_t center_temp_x10)
{
    cloud_event_t event = {
        .type = CLOUD_EVT_THERMAL_SNAPSHOT,
        .timestamp_ms = (uint32_t)esp_log_timestamp(),
    };

    event.data.thermal.min_temp_x10 = min_temp_x10;
    event.data.thermal.max_temp_x10 = max_temp_x10;
    event.data.thermal.center_temp_x10 = center_temp_x10;

    return app_service_bus_submit_cloud_event(&event);
}

esp_err_t app_service_bus_submit_ota_status(uint8_t stage,
                                            uint8_t percent,
                                            uint16_t detail_code,
                                            uint32_t current_value,
                                            uint32_t total_value)
{
    cloud_event_t event = {
        .type = CLOUD_EVT_OTA_STATUS,
        .timestamp_ms = (uint32_t)esp_log_timestamp(),
    };

    event.data.ota_status.stage = stage;
    event.data.ota_status.percent = percent;
    event.data.ota_status.detail_code = detail_code;
    event.data.ota_status.current_value = current_value;
    event.data.ota_status.total_value = total_value;

    return app_service_bus_submit_cloud_event(&event);
}

void app_service_bus_set_bits(EventBits_t bits)
{
    if (s_app_event_group != NULL)
    {
        (void)xEventGroupSetBits(s_app_event_group, bits);
    }
}

void app_service_bus_clear_bits(EventBits_t bits)
{
    if (s_app_event_group != NULL)
    {
        (void)xEventGroupClearBits(s_app_event_group, bits);
    }
}

EventBits_t app_service_bus_get_bits(void)
{
    if (s_app_event_group == NULL)
    {
        return 0U;
    }

    return xEventGroupGetBits(s_app_event_group);
}
