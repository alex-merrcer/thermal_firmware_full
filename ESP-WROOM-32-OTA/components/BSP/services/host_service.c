#include "host_service.h"

#include "app_service_bus.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "host_ctrl_service.h"

#define HOST_SERVICE_STACK_SIZE 8192
#define HOST_SERVICE_PRIORITY 5

static const char *TAG = "HOST_SERVICE";
static TaskHandle_t s_host_task_handle = NULL;

static void host_service_task(void *arg)
{
    QueueHandle_t queue = app_service_bus_host_frame_queue();
    ota_ctrl_frame_t frame = {0};

    (void)arg;
    host_ctrl_service_init();

    while (true)
    {
        if (queue != NULL &&
            xQueueReceive(queue, &frame, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            (void)host_ctrl_service_handle_frame(&frame);
        }

        host_ctrl_service_step();
    }
}

esp_err_t host_service_init(void)
{
    return app_service_bus_init();
}

void host_service_start(void)
{
    if (s_host_task_handle != NULL)
    {
        return;
    }

    if (xTaskCreate(host_service_task,
                    "host_service",
                    HOST_SERVICE_STACK_SIZE,
                    NULL,
                    HOST_SERVICE_PRIORITY,
                    &s_host_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create host service task");
        s_host_task_handle = NULL;
    }
}
