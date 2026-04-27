#include "app_service_bus.h"

#include <string.h>

#include "app_perf_baseline.h"
#include "semphr.h"

#define APP_SERVICE_DEFAULT_TIMEOUT_MS 1500UL
#define APP_SERVICE_CMD_COUNT          ((uint8_t)APP_SERVICE_CMD_OTA_QUERY_LATEST + 1U)

typedef struct
{
    app_service_cmd_t cmd;
    app_service_rsp_t *sync_rsp;
    uint8_t sync_wait;
} app_service_req_t;

static TaskHandle_t s_service_task_handle = 0;
static TaskHandle_t s_ui_task_handle = 0;
static QueueHandle_t s_service_cmd_queue = 0;
static QueueHandle_t s_ui_msg_queue = 0;
static SemaphoreHandle_t s_service_done_sem = 0;
static SemaphoreHandle_t s_service_sync_mutex = 0;
static app_service_execute_fn_t s_execute_fn = 0;
static app_service_cmd_t s_service_deferred_cmd[APP_SERVICE_CMD_COUNT];
static uint8_t s_service_deferred_valid[APP_SERVICE_CMD_COUNT];
static uint8_t s_service_pending[APP_SERVICE_CMD_COUNT];

static uint8_t app_service_bus_is_scheduler_running(void);
static uint8_t app_service_cmd_id_is_valid(app_service_cmd_id_t cmd_id);
static uint8_t app_service_cmd_index(app_service_cmd_id_t cmd_id);
static void app_service_pending_set(app_service_cmd_id_t cmd_id, uint8_t value);
static uint8_t app_service_pending_get(app_service_cmd_id_t cmd_id);
static void app_service_store_deferred(const app_service_cmd_t *cmd);
static uint8_t app_service_try_enqueue_deferred_for_id(app_service_cmd_id_t cmd_id);
static void app_service_bus_notify_ui_task(void);

static uint8_t app_service_bus_is_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

static uint8_t app_service_cmd_id_is_valid(app_service_cmd_id_t cmd_id)
{
    return (cmd_id > APP_SERVICE_CMD_NONE &&
            (uint8_t)cmd_id < APP_SERVICE_CMD_COUNT) ? 1U : 0U;
}

static uint8_t app_service_cmd_index(app_service_cmd_id_t cmd_id)
{
    return (uint8_t)cmd_id;
}

static void app_service_pending_set(app_service_cmd_id_t cmd_id, uint8_t value)
{
    uint8_t index = 0U;

    if (app_service_cmd_id_is_valid(cmd_id) == 0U)
    {
        return;
    }

    index = app_service_cmd_index(cmd_id);
    taskENTER_CRITICAL();
    s_service_pending[index] = (value != 0U) ? 1U : 0U;
    taskEXIT_CRITICAL();
}

static uint8_t app_service_pending_get(app_service_cmd_id_t cmd_id)
{
    uint8_t index = 0U;
    uint8_t value = 0U;

    if (app_service_cmd_id_is_valid(cmd_id) == 0U)
    {
        return 0U;
    }

    index = app_service_cmd_index(cmd_id);
    taskENTER_CRITICAL();
    value = s_service_pending[index];
    taskEXIT_CRITICAL();
    return value;
}

static void app_service_store_deferred(const app_service_cmd_t *cmd)
{
    uint8_t index = 0U;

    if (cmd == 0 || app_service_cmd_id_is_valid(cmd->cmd_id) == 0U)
    {
        return;
    }

    index = app_service_cmd_index(cmd->cmd_id);
    taskENTER_CRITICAL();
    s_service_deferred_cmd[index] = *cmd;
    s_service_deferred_valid[index] = 1U;
    taskEXIT_CRITICAL();
}

static uint8_t app_service_try_enqueue_deferred_for_id(app_service_cmd_id_t cmd_id)
{
    uint8_t index = 0U;
    uint8_t has_deferred = 0U;
    app_service_cmd_t deferred_cmd;
    app_service_req_t req;

    if (s_service_cmd_queue == 0 || app_service_cmd_id_is_valid(cmd_id) == 0U)
    {
        return 0U;
    }

    index = app_service_cmd_index(cmd_id);
    taskENTER_CRITICAL();
    if (s_service_pending[index] == 0U && s_service_deferred_valid[index] != 0U)
    {
        deferred_cmd = s_service_deferred_cmd[index];
        s_service_deferred_valid[index] = 0U;
        has_deferred = 1U;
    }
    taskEXIT_CRITICAL();

    if (has_deferred == 0U)
    {
        return 0U;
    }

    memset(&req, 0, sizeof(req));
    req.cmd = deferred_cmd;
    req.sync_wait = 0U;
    req.sync_rsp = 0;

    if (xQueueSendToBack(s_service_cmd_queue, &req, 0U) == pdPASS)
    {
        app_service_pending_set(deferred_cmd.cmd_id, 1U);
        return 1U;
    }

    app_perf_baseline_record_service_queue_fail();
    app_service_store_deferred(&deferred_cmd);
    return 0U;
}

static void app_service_bus_notify_ui_task(void)
{
    if (s_ui_task_handle != 0)
    {
        app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_UI);
        xTaskNotifyGive(s_ui_task_handle);
    }
}

void app_service_bus_reset(void)
{
    s_service_task_handle = 0;
    s_ui_task_handle = 0;
    s_service_cmd_queue = 0;
    s_ui_msg_queue = 0;
    s_service_done_sem = 0;
    s_service_sync_mutex = 0;
    s_execute_fn = 0;
    memset(s_service_deferred_cmd, 0, sizeof(s_service_deferred_cmd));
    memset(s_service_deferred_valid, 0, sizeof(s_service_deferred_valid));
    memset(s_service_pending, 0, sizeof(s_service_pending));
}

void app_service_bus_register_executor(app_service_execute_fn_t execute_fn)
{
    s_execute_fn = execute_fn;
}

uint8_t app_service_bus_init(QueueHandle_t ui_msg_queue, uint32_t service_queue_len)
{
    if (ui_msg_queue == 0 || service_queue_len == 0U)
    {
        return 0U;
    }

    s_ui_msg_queue = ui_msg_queue;
    s_service_cmd_queue = xQueueCreate((UBaseType_t)service_queue_len, sizeof(app_service_req_t));
    s_service_done_sem = xSemaphoreCreateBinary();
    s_service_sync_mutex = xSemaphoreCreateMutex();

    if (s_service_cmd_queue == 0 ||
        s_service_done_sem == 0 ||
        s_service_sync_mutex == 0)
    {
        return 0U;
    }

    return 1U;
}

void app_service_bus_set_service_task_handle(TaskHandle_t service_task_handle)
{
    s_service_task_handle = service_task_handle;
}

void app_service_bus_set_ui_task_handle(TaskHandle_t ui_task_handle)
{
    s_ui_task_handle = ui_task_handle;
}

uint8_t app_service_bus_has_pending_work(void)
{
    return (s_service_cmd_queue != 0 &&
            uxQueueMessagesWaiting(s_service_cmd_queue) != 0U) ? 1U : 0U;
}

void app_service_bus_notify_service_task(void)
{
    if (s_service_task_handle != 0)
    {
        app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_SERVICE);
        xTaskNotifyGive(s_service_task_handle);
    }
}

void app_service_bus_process(void)
{
    app_service_req_t service_req;
    app_service_rsp_t service_rsp;

    if (s_service_cmd_queue == 0)
    {
        return;
    }

    while (xQueueReceive(s_service_cmd_queue, &service_req, 0U) == pdPASS)
    {
        memset(&service_rsp, 0, sizeof(service_rsp));
        service_rsp.cmd_id = service_req.cmd.cmd_id;
        if (s_execute_fn != 0)
        {
            (void)s_execute_fn(&service_req.cmd, &service_rsp);
        }

        if (service_req.sync_wait != 0U && service_req.sync_rsp != 0)
        {
            *(service_req.sync_rsp) = service_rsp;
            (void)xSemaphoreGive(s_service_done_sem);
        }

        if (s_ui_msg_queue != 0)
        {
            if (xQueueSendToBack(s_ui_msg_queue, &service_rsp, 0U) != pdPASS)
            {
                app_service_rsp_t dropped_rsp;

                app_perf_baseline_record_ui_msg_drop();
                (void)xQueueReceive(s_ui_msg_queue, &dropped_rsp, 0U);
                (void)xQueueSendToBack(s_ui_msg_queue, &service_rsp, 0U);
            }
            app_service_bus_notify_ui_task();
        }

        app_service_pending_set(service_req.cmd.cmd_id, 0U);
        (void)app_service_try_enqueue_deferred_for_id(service_req.cmd.cmd_id);
    }
}

uint8_t app_service_bus_try_enqueue_deferred_any(void)
{
    uint8_t index = 0U;

    for (index = 1U; index < APP_SERVICE_CMD_COUNT; ++index)
    {
        if (app_service_try_enqueue_deferred_for_id((app_service_cmd_id_t)index) != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}

uint8_t app_service_submit(const app_service_cmd_t *cmd,
                           app_service_rsp_t *rsp,
                           uint32_t timeout_ms)
{
    app_service_req_t req;
    TickType_t wait_ticks = 0U;
    app_service_rsp_t local_rsp;

    if (cmd == 0)
    {
        return 0U;
    }

    if (timeout_ms == 0U)
    {
        timeout_ms = APP_SERVICE_DEFAULT_TIMEOUT_MS;
    }

    if ((app_service_bus_is_scheduler_running() != 0U) &&
        (xTaskGetCurrentTaskHandle() == s_service_task_handle))
    {
        if (rsp == 0)
        {
            rsp = &local_rsp;
        }
        return (s_execute_fn != 0) ? s_execute_fn(cmd, rsp) : 0U;
    }

    if (app_service_bus_is_scheduler_running() == 0U ||
        s_service_cmd_queue == 0 ||
        s_service_done_sem == 0 ||
        s_service_sync_mutex == 0)
    {
        if (rsp == 0)
        {
            rsp = &local_rsp;
        }
        return (s_execute_fn != 0) ? s_execute_fn(cmd, rsp) : 0U;
    }

    memset(&req, 0, sizeof(req));
    req.cmd = *cmd;
    req.sync_rsp = (rsp != 0) ? rsp : &local_rsp;
    req.sync_wait = 1U;
    wait_ticks = pdMS_TO_TICKS(timeout_ms);

    if (xSemaphoreTake(s_service_sync_mutex, wait_ticks) != pdPASS)
    {
        return 0U;
    }

    (void)xSemaphoreTake(s_service_done_sem, 0U);
    app_service_pending_set(req.cmd.cmd_id, 1U);
    if (xQueueSendToBack(s_service_cmd_queue, &req, wait_ticks) != pdPASS)
    {
        app_perf_baseline_record_service_queue_fail();
        app_service_pending_set(req.cmd.cmd_id, 0U);
        (void)xSemaphoreGive(s_service_sync_mutex);
        return 0U;
    }
    app_service_bus_notify_service_task();

    if (xSemaphoreTake(s_service_done_sem, wait_ticks) != pdPASS)
    {
        app_service_pending_set(req.cmd.cmd_id, 0U);
        (void)xSemaphoreGive(s_service_sync_mutex);
        return 0U;
    }

    (void)xSemaphoreGive(s_service_sync_mutex);
    return req.sync_rsp->ok;
}

uint8_t app_service_submit_async(const app_service_cmd_t *cmd)
{
    app_service_req_t req;
    app_service_rsp_t local_rsp;

    if (cmd == 0 || app_service_cmd_id_is_valid(cmd->cmd_id) == 0U)
    {
        return 0U;
    }

    if (app_service_bus_is_scheduler_running() == 0U || s_service_cmd_queue == 0)
    {
        return (s_execute_fn != 0) ? s_execute_fn(cmd, &local_rsp) : 0U;
    }

    if (app_service_pending_get(cmd->cmd_id) != 0U)
    {
        app_service_store_deferred(cmd);
        return 1U;
    }

    memset(&req, 0, sizeof(req));
    req.cmd = *cmd;
    req.sync_rsp = 0;
    req.sync_wait = 0U;

    if (xQueueSendToBack(s_service_cmd_queue, &req, 0U) == pdPASS)
    {
        uint8_t index = app_service_cmd_index(cmd->cmd_id);

        taskENTER_CRITICAL();
        s_service_deferred_valid[index] = 0U;
        taskEXIT_CRITICAL();
        app_service_pending_set(cmd->cmd_id, 1U);
        app_service_bus_notify_service_task();
        return 1U;
    }

    app_perf_baseline_record_service_queue_fail();
    app_service_store_deferred(cmd);
    return 1U;
}
