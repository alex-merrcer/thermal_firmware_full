#include "host_uart_rx.h"

#include "app_service_bus.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "ota_stm32_internal.h"

#define HOST_UART_RX_STACK_SIZE 6144
#define HOST_UART_RX_PRIORITY 4

static const char *TAG = "HOST_UART_RX";
static TaskHandle_t s_host_uart_rx_task = NULL;

static void host_uart_rx_task(void *arg)
{
    QueueHandle_t host_queue = app_service_bus_host_frame_queue();
    QueueHandle_t ota_queue = app_service_bus_ota_frame_queue();
    ota_ctrl_frame_t frame = {0};

    (void)arg;
    ota_ctrl_flush_uart();

    while (true)
    {
        if ((app_service_bus_get_bits() & APP_EVENT_OTA_RUNNING) != 0U)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!ota_ctrl_receive_frame(&frame, 20U))
        {
            continue;
        }

        app_service_bus_set_bits(APP_EVENT_STM32_ONLINE);

        if (frame.msg_type == OTA_CTRL_MSG_HOST_REQ)
        {
            if (host_queue == NULL || xQueueSendToBack(host_queue, &frame, 0) != pdTRUE)
            {
                ESP_LOGW(TAG, "Host frame dropped because host queue is full");
            }
            continue;
        }

        if (frame.msg_type == OTA_CTRL_MSG_REQ)
        {
            if (ota_queue == NULL || xQueueSendToBack(ota_queue, &frame, 0) != pdTRUE)
            {
                ESP_LOGW(TAG, "OTA frame dropped because OTA queue is full");
            }
        }
    }
}

void host_uart_rx_start(void)
{
    if (s_host_uart_rx_task != NULL)
    {
        return;
    }

    if (xTaskCreate(host_uart_rx_task,
                    "host_uart_rx",
                    HOST_UART_RX_STACK_SIZE,
                    NULL,
                    HOST_UART_RX_PRIORITY,
                    &s_host_uart_rx_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create host UART RX task");
        s_host_uart_rx_task = NULL;
    }
}
