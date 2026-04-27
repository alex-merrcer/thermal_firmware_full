#include "app_display_runtime.h"
#include "redpic1_thermal.h"
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "app_perf_baseline.h"
#include "lcd_dma.h"
#include "lcd_init.h"
#include "power_manager.h"

#define APP_DISPLAY_CMD_QUEUE_LEN      8U
#define APP_DISPLAY_EVENT_SET_LEN      (APP_DISPLAY_CMD_QUEUE_LEN + 1U)
#define APP_DISPLAY_SYNC_TIMEOUT_MS    1000UL

typedef enum
{
    APP_DISPLAY_CMD_NONE = 0,
    APP_DISPLAY_CMD_THERMAL_PRESENT,
    APP_DISPLAY_CMD_SLEEP,
    APP_DISPLAY_CMD_WAKE,
    APP_DISPLAY_CMD_UI_RENDER
} app_display_cmd_id_t;

typedef struct
{
    app_display_cmd_id_t cmd_id;
    uint8_t *gray_frame;
    app_display_ui_render_fn_t render_fn;
    uint8_t full_refresh;
} app_display_runtime_cmd_t;

typedef struct
{
    uint8_t ok;
} app_display_runtime_rsp_t;

typedef struct
{
    app_display_runtime_cmd_t cmd;
    app_display_runtime_rsp_t *sync_rsp;
    uint8_t sync_wait;
} app_display_runtime_req_t;

typedef struct
{
    uint8_t pending;
    uint8_t *gray_frame;
    uintptr_t token;
} app_display_runtime_async_thermal_t;

static SemaphoreHandle_t s_display_mutex = 0;
static QueueHandle_t s_display_cmd_queue = 0;
static QueueSetHandle_t s_display_event_set = 0;
static SemaphoreHandle_t s_display_thermal_sem = 0;
static SemaphoreHandle_t s_display_done_sem = 0;
static SemaphoreHandle_t s_display_sync_mutex = 0;
static TaskHandle_t s_display_task_handle = 0;
static uint8_t s_display_awake = 1U;
static app_display_runtime_async_thermal_t s_pending_thermal;
static app_display_thermal_done_fn_t s_thermal_done_fn = 0;
static app_display_thermal_claim_fn_t s_thermal_claim_fn = 0;

static uint8_t app_display_runtime_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

static void app_display_runtime_enter_critical(void)
{
    if (app_display_runtime_scheduler_running() != 0U)
    {
        taskENTER_CRITICAL();
    }
}

static void app_display_runtime_exit_critical(void)
{
    if (app_display_runtime_scheduler_running() != 0U)
    {
        taskEXIT_CRITICAL();
    }
}

static uint8_t app_display_runtime_owner_ready(void)
{
    return (app_display_runtime_scheduler_running() != 0U &&
            s_display_task_handle != 0 &&
            s_display_cmd_queue != 0 &&
            s_display_event_set != 0 &&
            s_display_thermal_sem != 0 &&
            s_display_done_sem != 0 &&
            s_display_sync_mutex != 0) ? 1U : 0U;
}

static void app_display_runtime_notify_thermal_done(uintptr_t token,
                                                    app_display_thermal_done_status_t status)
{
    app_display_thermal_done_fn_t done_fn = 0;

    app_display_runtime_enter_critical();
    done_fn = s_thermal_done_fn;
    app_display_runtime_exit_critical();

    if (done_fn != 0)
    {
        done_fn(token, status);
    }
}

static uint8_t app_display_runtime_present_thermal_locked(uint8_t *gray_frame)
{
    uint8_t ok = 0U;

    if (gray_frame == 0 || s_display_awake == 0U)
    {
        return 0U;
    }

    power_manager_acquire_lock(POWER_LOCK_DISPLAY_DMA);
    ok = LCD_Disp_Thermal_Interpolated_DMA(gray_frame);
    if (ok != 0U)
    {
        redpic1_thermal_render_runtime_overlay();
    }
    power_manager_release_lock(POWER_LOCK_DISPLAY_DMA);

    return ok;
}

static void app_display_runtime_execute_locked(const app_display_runtime_cmd_t *cmd,
                                               app_display_runtime_rsp_t *rsp)
{
    if (rsp != 0)
    {
        rsp->ok = 0U;
    }

    if (cmd == 0)
    {
        return;
    }

    switch (cmd->cmd_id)
    {
    case APP_DISPLAY_CMD_SLEEP:
        if (s_display_awake != 0U)
        {
            lcd_power_sleep();
            s_display_awake = 0U;
        }
        if (rsp != 0)
        {
            rsp->ok = 1U;
        }
        break;

    case APP_DISPLAY_CMD_WAKE:
        if (s_display_awake == 0U)
        {
            lcd_power_wake();
            s_display_awake = 1U;
        }
        if (rsp != 0)
        {
            rsp->ok = 1U;
        }
        break;

    case APP_DISPLAY_CMD_THERMAL_PRESENT:
        if (rsp != 0)
        {
            rsp->ok = app_display_runtime_present_thermal_locked(cmd->gray_frame);
        }
        else
        {
            (void)app_display_runtime_present_thermal_locked(cmd->gray_frame);
        }
        break;

    case APP_DISPLAY_CMD_UI_RENDER:
        if (cmd->render_fn != 0 && s_display_awake != 0U)
        {
            cmd->render_fn(cmd->full_refresh);
            if (rsp != 0)
            {
                rsp->ok = 1U;
            }
        }
        break;

    case APP_DISPLAY_CMD_NONE:
    default:
        break;
    }
}

static uint8_t app_display_runtime_execute_direct(const app_display_runtime_cmd_t *cmd)
{
    app_display_runtime_rsp_t rsp;

    memset(&rsp, 0, sizeof(rsp));
    app_display_runtime_lock();
    app_display_runtime_execute_locked(cmd, &rsp);
    app_display_runtime_unlock();
    return rsp.ok;
}

static uint8_t app_display_runtime_submit_sync(const app_display_runtime_cmd_t *cmd)
{
    app_display_runtime_req_t req;
    app_display_runtime_rsp_t rsp;
    TickType_t wait_ticks = pdMS_TO_TICKS(APP_DISPLAY_SYNC_TIMEOUT_MS);

    if (cmd == 0)
    {
        return 0U;
    }

    if (app_display_runtime_owner_ready() == 0U ||
        xTaskGetCurrentTaskHandle() == s_display_task_handle)
    {
        return app_display_runtime_execute_direct(cmd);
    }

    if (xSemaphoreTake(s_display_sync_mutex, wait_ticks) != pdPASS)
    {
        return 0U;
    }

    memset(&req, 0, sizeof(req));
    memset(&rsp, 0, sizeof(rsp));
    req.cmd = *cmd;
    req.sync_rsp = &rsp;
    req.sync_wait = 1U;

    (void)xSemaphoreTake(s_display_done_sem, 0U);
    if (xQueueSendToBack(s_display_cmd_queue, &req, wait_ticks) != pdPASS)
    {
        app_perf_baseline_record_display_queue_fail();
        (void)xSemaphoreGive(s_display_sync_mutex);
        return 0U;
    }

    app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_DISPLAY);
    if (xSemaphoreTake(s_display_done_sem, wait_ticks) != pdPASS)
    {
        (void)xSemaphoreGive(s_display_sync_mutex);
        return 0U;
    }

    (void)xSemaphoreGive(s_display_sync_mutex);
    return rsp.ok;
}

static uint8_t app_display_runtime_take_pending_thermal(app_display_runtime_async_thermal_t *pending)
{
    uint8_t has_pending = 0U;

    if (pending == 0)
    {
        return 0U;
    }

    memset(pending, 0, sizeof(*pending));
    app_display_runtime_enter_critical();
    if (s_pending_thermal.pending != 0U)
    {
        *pending = s_pending_thermal;
        memset(&s_pending_thermal, 0, sizeof(s_pending_thermal));
        has_pending = 1U;
    }
    app_display_runtime_exit_critical();

    return has_pending;
}

static uint8_t app_display_runtime_process_pending_thermal(void)
{
    app_display_runtime_async_thermal_t pending;
    uint8_t *gray_frame = 0;
    uint8_t ok = 0U;

    if (s_display_awake == 0U)
    {
        return 0U;
    }

    if (app_display_runtime_take_pending_thermal(&pending) == 0U)
    {
        return 0U;
    }

    if (s_thermal_claim_fn != 0)
    {
        if (s_thermal_claim_fn(pending.token, &gray_frame) == 0U || gray_frame == 0)
        {
            app_display_runtime_notify_thermal_done(pending.token,
                                                    APP_DISPLAY_THERMAL_DONE_CANCELLED);
            return 1U;
        }
    }
    else
    {
        gray_frame = pending.gray_frame;
        if (gray_frame == 0)
        {
            app_display_runtime_notify_thermal_done(pending.token,
                                                    APP_DISPLAY_THERMAL_DONE_ERROR);
            return 1U;
        }
    }

    app_display_runtime_lock();
    ok = app_display_runtime_present_thermal_locked(gray_frame);
    app_display_runtime_unlock();

    app_display_runtime_notify_thermal_done(pending.token,
                                            (ok != 0U) ?
                                            APP_DISPLAY_THERMAL_DONE_OK :
                                            APP_DISPLAY_THERMAL_DONE_ERROR);
    return 1U;
}

static void app_display_runtime_process_sync_req(const app_display_runtime_req_t *req)
{
    app_display_runtime_rsp_t rsp;

    if (req == 0)
    {
        return;
    }

    memset(&rsp, 0, sizeof(rsp));
    app_display_runtime_lock();
    app_display_runtime_execute_locked(&req->cmd, &rsp);
    app_display_runtime_unlock();

    if (req->sync_wait != 0U && req->sync_rsp != 0)
    {
        *(req->sync_rsp) = rsp;
        (void)xSemaphoreGive(s_display_done_sem);
    }
}

void app_display_runtime_set_thermal_present_done_callback(app_display_thermal_done_fn_t done_fn)
{
    app_display_runtime_enter_critical();
    s_thermal_done_fn = done_fn;
    app_display_runtime_exit_critical();
}

void app_display_runtime_set_thermal_present_claim_callback(app_display_thermal_claim_fn_t claim_fn)
{
    app_display_runtime_enter_critical();
    s_thermal_claim_fn = claim_fn;
    app_display_runtime_exit_critical();
}

uint8_t app_display_runtime_init(void)
{
    s_display_task_handle = 0;
    s_display_awake = 1U;
    memset(&s_pending_thermal, 0, sizeof(s_pending_thermal));
    s_thermal_done_fn = 0;
    s_thermal_claim_fn = 0;
    s_display_mutex = xSemaphoreCreateRecursiveMutex();
    s_display_cmd_queue = xQueueCreate(APP_DISPLAY_CMD_QUEUE_LEN, sizeof(app_display_runtime_req_t));
    s_display_event_set = xQueueCreateSet(APP_DISPLAY_EVENT_SET_LEN);
    s_display_thermal_sem = xSemaphoreCreateBinary();
    s_display_done_sem = xSemaphoreCreateBinary();
    s_display_sync_mutex = xSemaphoreCreateMutex();

    if (s_display_mutex == 0 ||
        s_display_cmd_queue == 0 ||
        s_display_event_set == 0 ||
        s_display_thermal_sem == 0 ||
        s_display_done_sem == 0 ||
        s_display_sync_mutex == 0)
    {
        return 0U;
    }

    if (xQueueAddToSet(s_display_cmd_queue, s_display_event_set) != pdPASS ||
        xQueueAddToSet(s_display_thermal_sem, s_display_event_set) != pdPASS)
    {
        return 0U;
    }

    return 1U;
}

uint8_t app_display_runtime_start(void)
{
    if (app_display_runtime_scheduler_running() == 0U ||
        s_display_cmd_queue == 0 ||
        s_display_event_set == 0 ||
        s_display_thermal_sem == 0 ||
        s_display_done_sem == 0 ||
        s_display_sync_mutex == 0)
    {
        return 0U;
    }

    s_display_task_handle = xTaskGetCurrentTaskHandle();
    return 1U;
}

void app_display_runtime_task(void *pvParameters)
{
    (void)pvParameters;
    if (app_display_runtime_start() == 0U)
    {
        while (1)
        {
        }
    }

    while (1)
    {
        QueueSetMemberHandle_t activated = 0;
        app_display_runtime_req_t req;

        activated = xQueueSelectFromSet(s_display_event_set, portMAX_DELAY);
        if (activated == s_display_thermal_sem)
        {
            (void)xSemaphoreTake(s_display_thermal_sem, 0U);
        }

        while (xQueueReceive(s_display_cmd_queue, &req, 0U) == pdPASS)
        {
            app_display_runtime_process_sync_req(&req);
        }

        if (s_display_awake == 0U)
        {
            app_display_runtime_cancel_thermal_present_async();
            continue;
        }

        (void)app_display_runtime_process_pending_thermal();
    }
}

void app_display_runtime_lock(void)
{
    if (s_display_mutex != 0 && app_display_runtime_scheduler_running() != 0U)
    {
        (void)xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY);
    }
}

void app_display_runtime_unlock(void)
{
    if (s_display_mutex != 0 && app_display_runtime_scheduler_running() != 0U)
    {
        (void)xSemaphoreGiveRecursive(s_display_mutex);
    }
}

uint8_t app_display_runtime_sleep(void)
{
    app_display_runtime_cmd_t cmd;

    if (s_display_awake == 0U)
    {
        return 1U;
    }

    app_display_runtime_cancel_thermal_present_async();

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_DISPLAY_CMD_SLEEP;
    return app_display_runtime_submit_sync(&cmd);
}

uint8_t app_display_runtime_wake(void)
{
    app_display_runtime_cmd_t cmd;

    if (s_display_awake != 0U)
    {
        return 1U;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_DISPLAY_CMD_WAKE;
    return app_display_runtime_submit_sync(&cmd);
}

uint8_t app_display_runtime_present_thermal_frame(uint8_t *gray_frame)
{
    app_display_runtime_cmd_t cmd;

    if (gray_frame == 0)
    {
        return 0U;
    }

    if (s_display_awake == 0U)
    {
        return 0U;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_DISPLAY_CMD_THERMAL_PRESENT;
    cmd.gray_frame = gray_frame;
    return app_display_runtime_submit_sync(&cmd);
}

uint8_t app_display_runtime_request_thermal_present_async(uint8_t *gray_frame, uintptr_t token)
{
    uintptr_t replaced_token = 0U;
    uint8_t replaced = 0U;

    if (gray_frame == 0 || s_display_awake == 0U || s_display_thermal_sem == 0)
    {
        return 0U;
    }

    app_display_runtime_enter_critical();
    if (s_pending_thermal.pending != 0U &&
        s_pending_thermal.token != token)
    {
        replaced_token = s_pending_thermal.token;
        replaced = 1U;
    }
    s_pending_thermal.pending = 1U;
    s_pending_thermal.gray_frame = gray_frame;
    s_pending_thermal.token = token;
    app_display_runtime_exit_critical();

    if (replaced != 0U)
    {
        app_display_runtime_notify_thermal_done(replaced_token,
                                                APP_DISPLAY_THERMAL_DONE_CANCELLED);
    }

    if (s_display_thermal_sem != 0)
    {
        if (xSemaphoreGive(s_display_thermal_sem) == pdPASS)
        {
            app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_DISPLAY);
        }
    }

    return 1U;
}

void app_display_runtime_cancel_thermal_present_async(void)
{
    uintptr_t cancelled_token = 0U;
    uint8_t cancelled = 0U;

    app_display_runtime_enter_critical();
    if (s_pending_thermal.pending != 0U)
    {
        cancelled_token = s_pending_thermal.token;
        memset(&s_pending_thermal, 0, sizeof(s_pending_thermal));
        cancelled = 1U;
    }
    app_display_runtime_exit_critical();

    while (s_display_thermal_sem != 0 && xSemaphoreTake(s_display_thermal_sem, 0U) == pdPASS)
    {
    }

    if (cancelled != 0U)
    {
        app_display_runtime_notify_thermal_done(cancelled_token,
                                                APP_DISPLAY_THERMAL_DONE_CANCELLED);
    }
}

uint8_t app_display_runtime_request_ui_render(app_display_ui_render_fn_t render_fn,
                                              uint8_t full_refresh)
{
    app_display_runtime_cmd_t cmd;

    if (render_fn == 0)
    {
        return 0U;
    }

    if (s_display_awake == 0U)
    {
        return 0U;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_DISPLAY_CMD_UI_RENDER;
    cmd.render_fn = render_fn;
    cmd.full_refresh = full_refresh;
    return app_display_runtime_submit_sync(&cmd);
}

uint8_t app_display_runtime_is_awake(void)
{
    return s_display_awake;
}
