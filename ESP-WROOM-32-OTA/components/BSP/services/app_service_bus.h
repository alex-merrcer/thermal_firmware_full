#ifndef APP_SERVICE_BUS_H
#define APP_SERVICE_BUS_H

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#define APP_EVENT_WIFI_CONNECTED BIT0
#define APP_EVENT_MQTT_CONNECTED BIT1
#define APP_EVENT_BLE_ACTIVE BIT2
#define APP_EVENT_OTA_RUNNING BIT3
#define APP_EVENT_LOW_POWER BIT4
#define APP_EVENT_STM32_ONLINE BIT5

typedef enum
{
    CLOUD_EVT_THERMAL_SNAPSHOT = 0,
    CLOUD_EVT_DEVICE_STATUS,
    CLOUD_EVT_WEATHER_UPDATE,
    CLOUD_EVT_OTA_STATUS,
    CLOUD_EVT_DIAGNOSTICS
} cloud_event_type_t;

typedef struct
{
    cloud_event_type_t type;
    uint32_t timestamp_ms;
    union
    {
        struct
        {
            int16_t min_temp_x10;
            int16_t max_temp_x10;
            int16_t center_temp_x10;
        } thermal;

        struct
        {
            int32_t value0;
            int32_t value1;
            int32_t value2;
        } generic;
    } data;
} cloud_event_t;

typedef enum
{
    HOST_EVT_THERMAL_SNAPSHOT = 0,
    HOST_EVT_HOST_STATE,
    HOST_EVT_WIFI_REQ,
    HOST_EVT_BLE_REQ,
    HOST_EVT_POWER_REQ,
    HOST_EVT_OTA_REQ
} host_event_type_t;

typedef struct
{
    host_event_type_t type;
    uint32_t timestamp_ms;
    union
    {
        struct
        {
            int16_t min_temp_x10;
            int16_t max_temp_x10;
            int16_t center_temp_x10;
        } thermal;

        struct
        {
            uint8_t value0;
            uint8_t value1;
            uint8_t value2;
            uint8_t value3;
        } request;
    } data;
} host_event_t;

esp_err_t app_service_bus_init(void);
EventGroupHandle_t app_service_bus_event_group(void);
QueueHandle_t app_service_bus_cloud_queue(void);
QueueHandle_t app_service_bus_host_frame_queue(void);
QueueHandle_t app_service_bus_ota_frame_queue(void);
void app_service_bus_set_bits(EventBits_t bits);
void app_service_bus_clear_bits(EventBits_t bits);
EventBits_t app_service_bus_get_bits(void);

#endif
