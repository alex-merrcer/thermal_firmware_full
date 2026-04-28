#ifndef APP_SERVICE_BUS_H
#define APP_SERVICE_BUS_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

typedef enum
{
    APP_SERVICE_CMD_NONE = 0,
    APP_SERVICE_CMD_ESP_REFRESH_STATUS,
    APP_SERVICE_CMD_SET_WIFI,
    APP_SERVICE_CMD_SET_BLE,
    APP_SERVICE_CMD_SET_MQTT,
    APP_SERVICE_CMD_SET_DEBUG_SCREEN,
    APP_SERVICE_CMD_SET_REMOTE_KEYS,
    APP_SERVICE_CMD_SET_POWER_POLICY,
    APP_SERVICE_CMD_SET_HOST_STATE,
    APP_SERVICE_CMD_ENTER_FORCED_DEEP_SLEEP,
    APP_SERVICE_CMD_PREPARE_STOP,
    APP_SERVICE_CMD_PREPARE_STANDBY,
    APP_SERVICE_CMD_SEND_THERMAL_SNAPSHOT,
    APP_SERVICE_CMD_OTA_QUERY_LATEST
} app_service_cmd_id_t;

typedef struct
{
    app_service_cmd_id_t cmd_id;
    uint8_t arg0;
    uint8_t arg1;
    uint32_t value;
} app_service_cmd_t;

#define APP_SERVICE_TEXT_LEN 24U

typedef struct
{
    app_service_cmd_id_t cmd_id;
    uint8_t ok;
    uint8_t reserved;
    uint16_t reason;
    uint32_t value;
    char text[APP_SERVICE_TEXT_LEN];
} app_service_rsp_t;

typedef uint8_t (*app_service_execute_fn_t)(const app_service_cmd_t *cmd,
                                            app_service_rsp_t *rsp);

void app_service_bus_reset(void);
void app_service_bus_register_executor(app_service_execute_fn_t execute_fn);
uint8_t app_service_bus_init(QueueHandle_t ui_msg_queue, uint32_t service_queue_len);
void app_service_bus_set_service_task_handle(TaskHandle_t service_task_handle);
void app_service_bus_set_ui_task_handle(TaskHandle_t ui_task_handle);
uint8_t app_service_bus_has_pending_work(void);
void app_service_bus_process(void);
uint8_t app_service_bus_try_enqueue_deferred_any(void);
void app_service_bus_notify_service_task(void);

uint8_t app_service_submit(const app_service_cmd_t *cmd,
                           app_service_rsp_t *rsp,
                           uint32_t timeout_ms);
uint8_t app_service_submit_async(const app_service_cmd_t *cmd);

#endif
