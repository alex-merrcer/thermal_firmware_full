#include "cloud_mqtt_service.h"

#include "MQTT.h"
#include "app_service_bus.h"
#include "esp_log.h"
#include "freertos/task.h"

#define CLOUD_MQTT_SERVICE_STACK_SIZE 6144
#define CLOUD_MQTT_SERVICE_PRIORITY 5

static const char *TAG = "CLOUD_MQTT_SVC";
static TaskHandle_t s_cloud_task_handle = NULL;

static void cloud_mqtt_service_handle_event(const cloud_event_t *event)
{
    if (event == NULL)
    {
        return;
    }

    switch (event->type)
    {
    case CLOUD_EVT_THERMAL_SNAPSHOT:
        if (mqtt_service_submit_thermal_snapshot_x10(event->data.thermal.min_temp_x10,
                                                     event->data.thermal.max_temp_x10,
                                                     event->data.thermal.center_temp_x10) != ESP_OK)
        {
            ESP_LOGW(TAG, "Thermal snapshot submit to MQTT service failed");
        }
        break;

    case CLOUD_EVT_DEVICE_STATUS:
    case CLOUD_EVT_WEATHER_UPDATE:
    case CLOUD_EVT_OTA_STATUS:
    case CLOUD_EVT_DIAGNOSTICS:
    default:
        ESP_LOGD(TAG, "Cloud event type=%d is reserved for later phases", (int)event->type);
        break;
    }
}

static void cloud_mqtt_service_task(void *arg)
{
    QueueHandle_t queue = app_service_bus_cloud_queue();
    cloud_event_t event = {0};

    (void)arg;

    while (true)
    {
        if (queue != NULL &&
            xQueueReceive(queue, &event, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            cloud_mqtt_service_handle_event(&event);
        }

        mqtt_service_step();
    }
}

esp_err_t cloud_mqtt_service_init(void)
{
    esp_err_t err = app_service_bus_init();

    if (err != ESP_OK)
    {
        return err;
    }

    return mqtt_service_init();
}

void cloud_mqtt_service_start(void)
{
    if (s_cloud_task_handle != NULL)
    {
        return;
    }

    if (xTaskCreate(cloud_mqtt_service_task,
                    "cloud_mqtt",
                    CLOUD_MQTT_SERVICE_STACK_SIZE,
                    NULL,
                    CLOUD_MQTT_SERVICE_PRIORITY,
                    &s_cloud_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create cloud MQTT task");
        s_cloud_task_handle = NULL;
    }
}

esp_err_t cloud_mqtt_service_submit_thermal_snapshot_x10(int16_t min_temp_x10,
                                                         int16_t max_temp_x10,
                                                         int16_t center_temp_x10)
{
    QueueHandle_t queue = app_service_bus_cloud_queue();
    cloud_event_t event = {
        .type = CLOUD_EVT_THERMAL_SNAPSHOT,
        .timestamp_ms = (uint32_t)esp_log_timestamp(),
    };

    if (queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    event.data.thermal.min_temp_x10 = min_temp_x10;
    event.data.thermal.max_temp_x10 = max_temp_x10;
    event.data.thermal.center_temp_x10 = center_temp_x10;

    return (xQueueSendToBack(queue, &event, 0) == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}
