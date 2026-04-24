#include "redpic1_app.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "app_display_runtime.h"
#include "app_perf_baseline.h"
#include "battery_monitor.h"
#include "clock_profile_service.h"
#include "delay.h"
#include "exti_key.h"
#include "key.h"
#include "lcd.h"
#include "lcd_dma.h"
#include "lcd_init.h"
#include "low_power_runtime.h"
#include "ota_ctrl_protocol.h"
#include "ota_service.h"
#include "power_manager.h"
#include "redpic1_thermal.h"
#include "rtc_lp_service.h"
#include "sys.h"
#include "ui_manager.h"
#include "uart_rx_ring.h"
#include "usart.h"
#include "watchdog_service.h"
#include "esp_host_service_priv.h"
#include "led.h"

/*
 * redpic1_app.c
 * RTOS 应用运行时主控模块。
 *
 * 该文件负责组织输入、服务、UI、热成像、电源看门狗等任务，
 * 同时提供页面层使用的服务命令提交接口和设置访问互斥包装。
 * 本次重构不改变任务优先级、节拍、队列长度和服务执行语义，
 * 仅收口重复模板、统一命名并补齐中文说明。
 */

#define START_TASK_PRIO                1U
#define START_STK_SIZE                 512U
#define INPUT_TASK_PRIO                8U
#define INPUT_STK_SIZE                 384U
#define DISPLAY_TASK_PRIO              8U
#define DISPLAY_STK_SIZE               1280U
#define SERVICE_TASK_PRIO              7U
#define SERVICE_STK_SIZE               1024U
#define UI_TASK_PRIO                   6U
#define UI_STK_SIZE                    1280U
#define THERMAL_TASK_PRIO              5U
#define THERMAL_TASK_ACTIVE_PRIO       5U
#define THERMAL_STK_SIZE               1024U
#define POWER_WDG_TASK_PRIO            9U
#define POWER_WDG_STK_SIZE             640U

#define Q_KEY_EVENT_LEN                16U
#define Q_SERVICE_CMD_LEN              12U
#define Q_UI_MSG_LEN                   12U

#define APP_INPUT_LOOP_MS              20U
#define APP_SERVICE_LOOP_MS            25U
#define APP_SERVICE_LOOP_MS_THERMAL    100U
#define APP_UI_LOOP_MS                 33U
#define APP_UI_LOOP_MS_THERMAL         100U
#define APP_THERMAL_IDLE_MS            30U
#define APP_POWER_LOOP_MS              25U
#define APP_KEY2_LONG_MS               600UL

#define APP_ESP_BOOT_SYNC_DELAY_MS     1200UL
#define APP_ESP_BOOT_SYNC_RETRY_MS     1000UL
#define APP_ESP_BOOT_SYNC_MAX_TRIES    3U
#define APP_ESP_RESUME_SYNC_DELAY_MS   20UL
#define APP_ESP_RESUME_SYNC_RETRY_MS   150UL
#define APP_ESP_RESUME_SYNC_MAX_TRIES  6U

#define APP_SERVICE_DEFAULT_TIMEOUT_MS 1500UL
#define APP_SERVICE_CMD_COUNT          ((uint8_t)APP_SERVICE_CMD_OTA_QUERY_LATEST + 1U)

#define APP_EG_BIT_THERMAL_ACTIVE      (1UL << 0)
#define APP_EG_BIT_SCREEN_OFF          (1UL << 1)
#define APP_EG_BIT_OTA_BUSY            (1UL << 2)
#define APP_EG_BIT_WIFI_BUSY           (1UL << 3)

#define APP_EG_HB_INPUT                (1UL << 8)
#define APP_EG_HB_UI                   (1UL << 9)
#define APP_EG_HB_SERVICE              (1UL << 10)
#define APP_EG_HB_POWER                (1UL << 11)
#define APP_EG_HB_ALL                  (APP_EG_HB_INPUT | APP_EG_HB_UI | APP_EG_HB_SERVICE | APP_EG_HB_POWER)

typedef struct
{
    /* 实际服务命令内容。 */
    app_service_cmd_t cmd;
    /* 同步调用时的返回缓冲区，异步调用为 NULL。 */
    app_service_rsp_t *sync_rsp;
    /* 1 表示调用方正在等待完成信号量。 */
    uint8_t sync_wait;
} app_service_req_t;

typedef struct
{
    /* KEY2 是否处于“等待区分短按/长按”的挂起状态。 */
    uint8_t key2_pending;
    /* KEY2 进入挂起状态时的系统毫秒节拍。 */
    uint32_t key2_press_start_ms;
} app_input_state_t;

typedef struct
{
    /* ESP 开机/唤醒后的配置同步流程是否仍需继续。 */
    uint8_t esp_boot_sync_pending;
    /* 已经执行过的同步重试次数。 */
    uint8_t esp_boot_sync_tries;
    /* 下一次允许发起同步的时间点。 */
    uint32_t esp_boot_sync_next_ms;
    /* 同步失败后的重试间隔。 */
    uint32_t esp_boot_sync_retry_ms;
    /* 最大允许重试次数。 */
    uint8_t esp_boot_sync_max_tries;
    /* 服务任务上一次观察到的电源状态。 */
    power_state_t last_power_state;
} app_service_state_t;

static TaskHandle_t s_start_task_handle = 0;
static TaskHandle_t s_input_task_handle = 0;
static TaskHandle_t s_display_task_handle = 0;
static TaskHandle_t s_service_task_handle = 0;
static TaskHandle_t s_ui_task_handle = 0;
static TaskHandle_t s_thermal_task_handle = 0;
static TaskHandle_t s_power_wdg_task_handle = 0;

static QueueHandle_t s_key_event_queue = 0;
static QueueHandle_t s_service_cmd_queue = 0;
static QueueHandle_t s_ui_msg_queue = 0;
static SemaphoreHandle_t s_service_done_sem = 0;
static SemaphoreHandle_t s_service_sync_mutex = 0;
static SemaphoreHandle_t s_settings_mutex = 0;
static EventGroupHandle_t s_runtime_events = 0;
static app_service_cmd_t s_service_deferred_cmd[APP_SERVICE_CMD_COUNT];
static uint8_t s_service_deferred_valid[APP_SERVICE_CMD_COUNT];
static uint8_t s_service_pending[APP_SERVICE_CMD_COUNT];

static app_input_state_t s_input_state;
static app_service_state_t s_service_state;

static void start_task(void *pvParameters);
static void input_task(void *pvParameters);
static void service_task(void *pvParameters);
static void ui_task(void *pvParameters);
static void thermal_task(void *pvParameters);
static void power_wdg_task(void *pvParameters);

static void app_apply_persisted_settings(void);
static void app_force_manual_wifi_boot(void);
static void app_schedule_esp_sync(uint32_t delay_ms, uint32_t retry_ms, uint8_t max_tries);
static void app_service_step_esp_sync(uint32_t now_ms);
static uint8_t app_service_sync_esp_settings_now(void);
static uint8_t app_service_execute_command(const app_service_cmd_t *cmd, app_service_rsp_t *rsp);
static void app_service_process_queue(void);
static void app_service_poll_background(uint32_t now_ms);
static uint8_t app_service_cmd_id_is_valid(app_service_cmd_id_t cmd_id);
static uint8_t app_service_cmd_index(app_service_cmd_id_t cmd_id);
static void app_service_pending_set(app_service_cmd_id_t cmd_id, uint8_t value);
static uint8_t app_service_pending_get(app_service_cmd_id_t cmd_id);
static void app_service_store_deferred(const app_service_cmd_t *cmd);
static uint8_t app_service_try_enqueue_deferred_for_id(app_service_cmd_id_t cmd_id);
static uint8_t app_service_try_enqueue_deferred_any(void);
static void app_ui_push_key(uint8_t key_value);
static void app_ui_notify_task(void);
static void app_service_notify_task(void);
static void app_thermal_notify_task(void);
static void app_power_notify_task_from_isr(BaseType_t *higher_priority_task_woken);
static power_state_t app_power_step_and_handle(power_state_t previous_state);
static void app_runtime_set_screen_off_event(power_state_t state);
static void app_ui_update_thermal_page_state(ui_page_id_t *active_page,
                                             ui_page_id_t *previous_active_page);
static uint8_t app_is_scheduler_running(void);
static uint8_t app_runtime_thermal_active(void);
static TickType_t app_service_wait_ticks(void);
static TickType_t app_ui_wait_ticks(ui_page_id_t active_page);
static void app_thermal_set_priority(uint8_t thermal_active);
static void app_create_runtime_task(TaskFunction_t task_entry,
                                    const char *task_name,
                                    uint16_t stack_size,
                                    UBaseType_t task_priority,
                                    TaskHandle_t *task_handle);
static void app_kick_initial_tasks(void);
static void app_panic_loop(void);

/* 统一承接不可恢复的初始化/创建失败，保持原有死循环行为不变。 */
static void app_panic_loop(void)
{
    while (1)
    {
    }
}

static uint8_t app_is_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

static uint8_t app_runtime_thermal_active(void)
{
    EventBits_t event_bits = 0U;

    if (s_runtime_events == 0)
    {
        return 0U;
    }

    event_bits = xEventGroupGetBits(s_runtime_events);
    return ((event_bits & APP_EG_BIT_THERMAL_ACTIVE) != 0U) ? 1U : 0U;
}

static TickType_t app_service_wait_ticks(void)
{
    EventBits_t event_bits = 0U;

    if (app_runtime_thermal_active() == 0U)
    {
        return pdMS_TO_TICKS(APP_SERVICE_LOOP_MS);
    }

    event_bits = (s_runtime_events != 0) ? xEventGroupGetBits(s_runtime_events) : 0U;
    if ((event_bits & (APP_EG_BIT_OTA_BUSY | APP_EG_BIT_WIFI_BUSY)) != 0U ||
        s_service_state.esp_boot_sync_pending != 0U ||
        (s_service_cmd_queue != 0 && uxQueueMessagesWaiting(s_service_cmd_queue) != 0U))
    {
        return pdMS_TO_TICKS(APP_SERVICE_LOOP_MS);
    }

    return pdMS_TO_TICKS(APP_SERVICE_LOOP_MS_THERMAL);
}

static TickType_t app_ui_wait_ticks(ui_page_id_t active_page)
{
    if (active_page != UI_PAGE_THERMAL)
    {
        return pdMS_TO_TICKS(APP_UI_LOOP_MS);
    }

    if ((s_key_event_queue != 0 && uxQueueMessagesWaiting(s_key_event_queue) != 0U) ||
        (s_ui_msg_queue != 0 && uxQueueMessagesWaiting(s_ui_msg_queue) != 0U))
    {
        return pdMS_TO_TICKS(APP_UI_LOOP_MS);
    }

    return pdMS_TO_TICKS(APP_UI_LOOP_MS_THERMAL);
}

/* 统一创建运行时任务，避免每个创建点重复展开同样的失败处理模板。 */
static void app_create_runtime_task(TaskFunction_t task_entry,
                                    const char *task_name,
                                    uint16_t stack_size,
                                    UBaseType_t task_priority,
                                    TaskHandle_t *task_handle)
{
    if (xTaskCreate(task_entry,
                    task_name,
                    stack_size,
                    (void *)0,
                    task_priority,
                    task_handle) != pdPASS)
    {
        app_panic_loop();
    }
}

static void app_thermal_set_priority(uint8_t thermal_active)
{
    if (s_thermal_task_handle == 0)
    {
        return;
    }

    vTaskPrioritySet(s_thermal_task_handle,
                     (thermal_active != 0U) ? (UBaseType_t)THERMAL_TASK_ACTIVE_PRIO
                                            : (UBaseType_t)THERMAL_TASK_PRIO);
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

static uint8_t app_service_try_enqueue_deferred_any(void)
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

void app_rtos_lcd_lock(void)
{
    app_display_runtime_lock();
}

void app_rtos_lcd_unlock(void)
{
    app_display_runtime_unlock();
}

void app_rtos_settings_lock(void)
{
    if (s_settings_mutex != 0 && app_is_scheduler_running() != 0U)
    {
        (void)xSemaphoreTakeRecursive(s_settings_mutex, portMAX_DELAY);
    }
}

void app_rtos_settings_unlock(void)
{
    if (s_settings_mutex != 0 && app_is_scheduler_running() != 0U)
    {
        (void)xSemaphoreGiveRecursive(s_settings_mutex);
    }
}

void app_rtos_settings_copy(device_settings_t *out_settings)
{
    if (out_settings == 0)
    {
        return;
    }

    app_rtos_settings_lock();
    *out_settings = *settings_service_get();
    app_rtos_settings_unlock();
}

uint8_t app_rtos_settings_update(const device_settings_t *settings)
{
    uint8_t ok = 0U;

    if (settings == 0)
    {
        return 0U;
    }

    app_rtos_settings_lock();
    ok = settings_service_update(settings);
    app_rtos_settings_unlock();
    return ok;
}

static void app_apply_persisted_settings(void)
{
    device_settings_t settings;

    app_rtos_settings_copy(&settings);
    power_manager_set_policy(settings.power_policy);
    power_manager_set_screen_off_timeout_ms(settings.screen_off_timeout_ms);
}

static void app_force_manual_wifi_boot(void)
{
    device_settings_t settings;

    settings = *settings_service_get();
    if (settings.wifi_enabled == 0U)
    {
        return;
    }

    settings.wifi_enabled = 0U;
    (void)settings_service_update(&settings);
}

static void app_schedule_esp_sync(uint32_t delay_ms, uint32_t retry_ms, uint8_t max_tries)
{
    s_service_state.esp_boot_sync_pending = 1U;
    s_service_state.esp_boot_sync_tries = 0U;
    s_service_state.esp_boot_sync_next_ms = power_manager_get_tick_ms() + delay_ms;
    s_service_state.esp_boot_sync_retry_ms = retry_ms;
    s_service_state.esp_boot_sync_max_tries = max_tries;
}

static uint8_t app_service_sync_esp_settings_now(void)
{
    device_settings_t settings;
    uint8_t debug_screen_enabled = 0U;
    uint8_t remote_keys_enabled = 0U;

    app_rtos_settings_copy(&settings);
    if (settings.debug_mode_enabled != 0U)
    {
        debug_screen_enabled = settings.esp32_debug_screen_enabled;
        remote_keys_enabled = settings.esp32_remote_keys_enabled;
    }

    if (esp_host_refresh_status() == 0U)
    {
        return 0U;
    }
    if (esp_host_set_power_policy_now(settings.power_policy) == 0U)
    {
        return 0U;
    }
    if (esp_host_set_host_state_now(power_manager_get_state()) == 0U)
    {
        return 0U;
    }
    if (esp_host_set_ble_now(settings.ble_enabled) == 0U)
    {
        return 0U;
    }
    if (esp_host_set_debug_screen_now(debug_screen_enabled) == 0U)
    {
        return 0U;
    }
    if (esp_host_set_remote_keys_now(remote_keys_enabled) == 0U)
    {
        return 0U;
    }
    if (esp_host_set_wifi_now(settings.wifi_enabled, (settings.wifi_enabled != 0U) ? 500UL : 0UL) == 0U)
    {
        return 0U;
    }

    return 1U;
}

static void app_service_step_esp_sync(uint32_t now_ms)
{
    if (s_service_state.esp_boot_sync_pending == 0U ||
        now_ms < s_service_state.esp_boot_sync_next_ms)
    {
        return;
    }

    if (app_service_sync_esp_settings_now() != 0U)
    {
        s_service_state.esp_boot_sync_pending = 0U;
        return;
    }

    s_service_state.esp_boot_sync_tries++;
    if (s_service_state.esp_boot_sync_tries >= s_service_state.esp_boot_sync_max_tries)
    {
        s_service_state.esp_boot_sync_pending = 0U;
        return;
    }

    s_service_state.esp_boot_sync_next_ms = now_ms + s_service_state.esp_boot_sync_retry_ms;
}

static void app_ui_notify_task(void)
{
    if (s_ui_task_handle != 0)
    {
        app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_UI);
        xTaskNotifyGive(s_ui_task_handle);
    }
}

static void app_service_notify_task(void)
{
    if (s_service_task_handle != 0)
    {
        app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_SERVICE);
        xTaskNotifyGive(s_service_task_handle);
    }
}

static void app_thermal_notify_task(void)
{
    if (s_thermal_task_handle != 0)
    {
        xTaskNotifyGive(s_thermal_task_handle);
    }
}

static void app_power_notify_task_from_isr(BaseType_t *higher_priority_task_woken)
{
    if (s_power_wdg_task_handle != 0)
    {
        vTaskNotifyGiveFromISR(s_power_wdg_task_handle, higher_priority_task_woken);
    }
}

static void app_runtime_set_screen_off_event(power_state_t state)
{
    if (s_runtime_events == 0)
    {
        return;
    }

    if (state == POWER_STATE_SCREEN_OFF_IDLE)
    {
        (void)xEventGroupSetBits(s_runtime_events, APP_EG_BIT_SCREEN_OFF);
    }
    else
    {
        (void)xEventGroupClearBits(s_runtime_events, APP_EG_BIT_SCREEN_OFF);
    }
}

static power_state_t app_power_step_and_handle(power_state_t previous_state)
{
    power_state_t current_state = previous_state;

    power_manager_step();
    current_state = power_manager_get_state();

    if (current_state != previous_state)
    {
        if (current_state == POWER_STATE_SCREEN_OFF_IDLE)
        {
            (void)app_display_runtime_sleep();
        }
        else if (previous_state == POWER_STATE_SCREEN_OFF_IDLE)
        {
            (void)app_display_runtime_wake();
            ui_manager_force_full_refresh();
            app_ui_notify_task();
        }

        app_service_notify_task();
    }

    app_runtime_set_screen_off_event(current_state);
    return current_state;
}

static EventBits_t app_service_cmd_busy_bits(app_service_cmd_id_t cmd_id)
{
    switch (cmd_id)
    {
    case APP_SERVICE_CMD_SET_WIFI:
    case APP_SERVICE_CMD_SET_DEBUG_SCREEN:
    case APP_SERVICE_CMD_SET_REMOTE_KEYS:
    case APP_SERVICE_CMD_ESP_REFRESH_STATUS:
    case APP_SERVICE_CMD_SET_POWER_POLICY:
    case APP_SERVICE_CMD_SET_HOST_STATE:
    case APP_SERVICE_CMD_ENTER_FORCED_DEEP_SLEEP:
    case APP_SERVICE_CMD_PREPARE_STOP:
    case APP_SERVICE_CMD_PREPARE_STANDBY:
    case APP_SERVICE_CMD_SEND_THERMAL_SNAPSHOT:
        return APP_EG_BIT_WIFI_BUSY;

    case APP_SERVICE_CMD_OTA_QUERY_LATEST:
        return APP_EG_BIT_OTA_BUSY;

    default:
        return 0U;
    }
}

static uint8_t app_service_execute_command(const app_service_cmd_t *cmd, app_service_rsp_t *rsp)
{
    char latest_version[APP_SERVICE_TEXT_LEN];
    uint16_t reject_reason = 0U;
    EventBits_t busy_bits = 0U;
    uint8_t ok = 0U;

    if (cmd == 0 || rsp == 0)
    {
        return 0U;
    }

    memset(rsp, 0, sizeof(*rsp));
    rsp->cmd_id = cmd->cmd_id;
    busy_bits = app_service_cmd_busy_bits(cmd->cmd_id);
    if (busy_bits != 0U && s_runtime_events != 0)
    {
        (void)xEventGroupSetBits(s_runtime_events, busy_bits);
    }

    switch (cmd->cmd_id)
    {
    case APP_SERVICE_CMD_ESP_REFRESH_STATUS:
        ok = esp_host_refresh_status();
        break;

    case APP_SERVICE_CMD_SET_WIFI:
        ok = esp_host_set_wifi_now(cmd->arg0, cmd->value);
        break;

    case APP_SERVICE_CMD_SET_DEBUG_SCREEN:
        ok = esp_host_set_debug_screen_now(cmd->arg0);
        break;

    case APP_SERVICE_CMD_SET_REMOTE_KEYS:
        ok = esp_host_set_remote_keys_now(cmd->arg0);
        break;

    case APP_SERVICE_CMD_SET_POWER_POLICY:
        ok = esp_host_set_power_policy_now((power_policy_t)cmd->arg0);
        break;

    case APP_SERVICE_CMD_SET_HOST_STATE:
        ok = esp_host_set_host_state_now((power_state_t)cmd->arg0);
        break;

    case APP_SERVICE_CMD_ENTER_FORCED_DEEP_SLEEP:
        ok = esp_host_enter_forced_deep_sleep_now(cmd->value);
        break;

    case APP_SERVICE_CMD_PREPARE_STOP:
        ok = esp_host_prepare_for_stop(cmd->value);
        break;

    case APP_SERVICE_CMD_PREPARE_STANDBY:
        ok = esp_host_prepare_for_standby(cmd->value);
        break;

    case APP_SERVICE_CMD_SEND_THERMAL_SNAPSHOT:
    {
        int16_t min_temp_x10 = (int16_t)(cmd->value & 0xFFFFU);
        int16_t max_temp_x10 = (int16_t)((cmd->value >> 16) & 0xFFFFU);
        int16_t center_temp_x10 = (int16_t)((uint16_t)cmd->arg0 |
                                            ((uint16_t)cmd->arg1 << 8));

        ok = esp_host_send_thermal_snapshot_x10(min_temp_x10,
                                                max_temp_x10,
                                                center_temp_x10);
    }
        break;

    case APP_SERVICE_CMD_OTA_QUERY_LATEST:
        memset(latest_version, 0, sizeof(latest_version));
        power_manager_acquire_lock(POWER_LOCK_OTA);
        ok = ota_service_query_latest_version(latest_version,
                                              sizeof(latest_version),
                                              &reject_reason);
        power_manager_release_lock(POWER_LOCK_OTA);
        rsp->reason = reject_reason;
        if (ok != 0U)
        {
            snprintf(rsp->text, sizeof(rsp->text), "%s", latest_version);
        }
        break;

    case APP_SERVICE_CMD_NONE:
    default:
        ok = 0U;
        break;
    }

    rsp->ok = ok;
    if (busy_bits != 0U && s_runtime_events != 0)
    {
        (void)xEventGroupClearBits(s_runtime_events, busy_bits);
    }

    return ok;
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

    /* 服务任务内的同步调用直接退化为本地执行，避免自发自收导致死锁。 */
    if ((app_is_scheduler_running() != 0U) &&
        (xTaskGetCurrentTaskHandle() == s_service_task_handle))
    {
        if (rsp == 0)
        {
            rsp = &local_rsp;
        }
        return app_service_execute_command(cmd, rsp);
    }

    if (app_is_scheduler_running() == 0U ||
        s_service_cmd_queue == 0 ||
        s_service_done_sem == 0 ||
        s_service_sync_mutex == 0)
    {
        if (rsp == 0)
        {
            rsp = &local_rsp;
        }
        return app_service_execute_command(cmd, rsp);
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
    app_service_notify_task();

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

    if (app_is_scheduler_running() == 0U || s_service_cmd_queue == 0)
    {
        return app_service_execute_command(cmd, &local_rsp);
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
        app_service_notify_task();
        return 1U;
    }

    app_perf_baseline_record_service_queue_fail();
    app_service_store_deferred(cmd);
    return 1U;
}

void KEY_EXTI_OnEventQueuedFromISR(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (s_input_task_handle == 0)
    {
        return;
    }

    app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_INPUT);
    vTaskNotifyGiveFromISR(s_input_task_handle, &higher_priority_task_woken);
    app_power_notify_task_from_isr(&higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static void app_ui_push_key(uint8_t key_value)
{
    app_key_event_t event_item;

    if (s_key_event_queue == 0 || key_value == 0U)
    {
        return;
    }

    event_item.key_value = key_value;
    event_item.tick_ms = power_manager_get_tick_ms();

    if (xQueueSendToBack(s_key_event_queue, &event_item, 0U) != pdPASS)
    {
        app_key_event_t dropped;
        app_perf_baseline_record_key_queue_drop();
        (void)xQueueReceive(s_key_event_queue, &dropped, 0U);
        (void)xQueueSendToBack(s_key_event_queue, &event_item, 0U);
    }

    if (s_ui_task_handle != 0)
    {
        app_ui_notify_task();
    }
}

/* 启动阶段统一拉起初始任务，保持原有首次通知顺序不变。 */
static void app_kick_initial_tasks(void)
{
    if (s_input_task_handle != 0)
    {
        app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_INPUT);
        xTaskNotifyGive(s_input_task_handle);
    }
    if (s_ui_task_handle != 0)
    {
        app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_UI);
        xTaskNotifyGive(s_ui_task_handle);
    }
    if (s_service_task_handle != 0)
    {
        app_service_notify_task();
    }
    if (s_power_wdg_task_handle != 0)
    {
        xTaskNotifyGive(s_power_wdg_task_handle);
    }
}

/* 启动任务仅负责创建各运行时任务，不承载业务逻辑。 */
static void start_task(void *pvParameters)
{
    (void)pvParameters;

    app_create_runtime_task(input_task,
                            "input_task",
                            (uint16_t)INPUT_STK_SIZE,
                            (UBaseType_t)INPUT_TASK_PRIO,
                            &s_input_task_handle);

    app_create_runtime_task(service_task,
                            "service_task",
                            (uint16_t)SERVICE_STK_SIZE,
                            (UBaseType_t)SERVICE_TASK_PRIO,
                            &s_service_task_handle);

#if APP_DISPLAY_STAGE3_ENABLE
    app_create_runtime_task(app_display_runtime_task,
                            "display_task",
                            (uint16_t)DISPLAY_STK_SIZE,
                            (UBaseType_t)DISPLAY_TASK_PRIO,
                            &s_display_task_handle);
#endif

    app_create_runtime_task(ui_task,
                            "ui_task",
                            (uint16_t)UI_STK_SIZE,
                            (UBaseType_t)UI_TASK_PRIO,
                            &s_ui_task_handle);

    app_create_runtime_task(thermal_task,
                            "thermal_task",
                            (uint16_t)THERMAL_STK_SIZE,
                            (UBaseType_t)THERMAL_TASK_PRIO,
                            &s_thermal_task_handle);

    app_create_runtime_task(power_wdg_task,
                            "power_wdg_task",
                            (uint16_t)POWER_WDG_STK_SIZE,
                            (UBaseType_t)POWER_WDG_TASK_PRIO,
                            &s_power_wdg_task_handle);

    app_kick_initial_tasks();

    vTaskDelete((TaskHandle_t)0);
}

static void input_task(void *pvParameters)
{
    uint8_t key_value = 0U;
    TickType_t wait_ticks = portMAX_DELAY;

    (void)pvParameters;
    memset(&s_input_state, 0, sizeof(s_input_state));

    while (1)
    {
        wait_ticks = (s_input_state.key2_pending != 0U) ?
                     pdMS_TO_TICKS(APP_INPUT_LOOP_MS) :
                     portMAX_DELAY;
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);

        while ((key_value = KEY_GetValue()) != 0U)
        {
            if (key_value == KEY2_PRES)
            {
                s_input_state.key2_pending = 1U;
                s_input_state.key2_press_start_ms = power_manager_get_tick_ms();
            }
            else
            {
                app_ui_push_key(key_value);
            }
        }

        if (s_input_state.key2_pending != 0U)
        {
            uint32_t now_ms = power_manager_get_tick_ms();

            if (KEY_IsLogicalPressed(KEY2_PRES) != 0U)
            {
                if ((now_ms - s_input_state.key2_press_start_ms) >= APP_KEY2_LONG_MS)
                {
                    s_input_state.key2_pending = 0U;
                    app_ui_push_key(UI_KEY_KEY2_LONG);
                }
            }
            else
            {
                s_input_state.key2_pending = 0U;
                app_ui_push_key(KEY2_PRES);
            }
        }

        if (s_runtime_events != 0)
        {
            (void)xEventGroupSetBits(s_runtime_events, APP_EG_HB_INPUT);
        }
    }
}

/* 服务任务单次消费命令队列，并保持原有“执行后立即投递 UI 响应”的顺序。 */
static void app_service_process_queue(void)
{
    app_service_req_t service_req;
    app_service_rsp_t service_rsp;

    while (xQueueReceive(s_service_cmd_queue, &service_req, 0U) == pdPASS)
    {
        (void)app_service_execute_command(&service_req.cmd, &service_rsp);

        if (service_req.sync_wait != 0U && service_req.sync_rsp != 0)
        {
            *(service_req.sync_rsp) = service_rsp;
            (void)xSemaphoreGive(s_service_done_sem);
        }

        if (xQueueSendToBack(s_ui_msg_queue, &service_rsp, 0U) != pdPASS)
        {
            app_service_rsp_t dropped_rsp;

            app_perf_baseline_record_ui_msg_drop();
            (void)xQueueReceive(s_ui_msg_queue, &dropped_rsp, 0U);
            (void)xQueueSendToBack(s_ui_msg_queue, &service_rsp, 0U);
        }
        app_ui_notify_task();

        app_service_pending_set(service_req.cmd.cmd_id, 0U);
        (void)app_service_try_enqueue_deferred_for_id(service_req.cmd.cmd_id);
    }
}

/* 服务任务后台轮询步骤。
 * 保持原有 ESP/OTA/电池/电源状态同步顺序不变。 */
static void app_service_poll_background(uint32_t now_ms)
{
    app_service_step_esp_sync(now_ms);
    ota_service_poll();
    esp_host_step();
    battery_monitor_step();

    if (power_manager_get_state() != s_service_state.last_power_state)
    {
        power_state_t previous_state = s_service_state.last_power_state;

        s_service_state.last_power_state = power_manager_get_state();
        if (previous_state == POWER_STATE_SCREEN_OFF_IDLE &&
            s_service_state.last_power_state != POWER_STATE_SCREEN_OFF_IDLE)
        {
            app_schedule_esp_sync(APP_ESP_RESUME_SYNC_DELAY_MS,
                                  APP_ESP_RESUME_SYNC_RETRY_MS,
                                  APP_ESP_RESUME_SYNC_MAX_TRIES);
        }
        (void)esp_host_set_host_state_now(s_service_state.last_power_state);
    }
}

/* UI 任务在每次 step 前统一同步活跃页与 thermal 任务联动状态。 */
static void app_ui_update_thermal_page_state(ui_page_id_t *active_page,
                                             ui_page_id_t *previous_active_page)
{
    if (active_page == 0 || previous_active_page == 0)
    {
        return;
    }

    *previous_active_page = *active_page;
    *active_page = ui_manager_get_active_page();

    if (s_runtime_events != 0)
    {
        if (*active_page == UI_PAGE_THERMAL)
        {
            (void)xEventGroupSetBits(s_runtime_events, APP_EG_BIT_THERMAL_ACTIVE);
        }
        else
        {
            (void)xEventGroupClearBits(s_runtime_events, APP_EG_BIT_THERMAL_ACTIVE);
        }
    }

    if (*active_page != *previous_active_page &&
        (*active_page == UI_PAGE_THERMAL || *previous_active_page == UI_PAGE_THERMAL))
    {
        app_thermal_set_priority((*active_page == UI_PAGE_THERMAL) ? 1U : 0U);
        app_thermal_notify_task();
    }
}

static void service_task(void *pvParameters)
{
    uint32_t now_ms = 0U;
    TickType_t wait_ticks = pdMS_TO_TICKS(APP_SERVICE_LOOP_MS);

    (void)pvParameters;
    memset(&s_service_state, 0, sizeof(s_service_state));

    esp_host_init();
    app_schedule_esp_sync(APP_ESP_BOOT_SYNC_DELAY_MS,
                          APP_ESP_BOOT_SYNC_RETRY_MS,
                          APP_ESP_BOOT_SYNC_MAX_TRIES);
    s_service_state.last_power_state = power_manager_get_state();

    while (1)
    {
        wait_ticks = app_service_wait_ticks();
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);

        app_service_process_queue();
        (void)app_service_try_enqueue_deferred_any();

        now_ms = power_manager_get_tick_ms();
        app_service_poll_background(now_ms);

        if (s_runtime_events != 0)
        {
            (void)xEventGroupSetBits(s_runtime_events, APP_EG_HB_SERVICE);
        }
    }
}

static void ui_task(void *pvParameters)
{
    app_key_event_t key_event;
    app_service_rsp_t ui_msg;
    TickType_t wait_ticks = pdMS_TO_TICKS(APP_UI_LOOP_MS);
    ui_page_id_t active_page = UI_PAGE_HOME;
    ui_page_id_t previous_active_page = UI_PAGE_HOME;

    (void)pvParameters;
    ui_manager_init();

    while (1)
    {
        wait_ticks = app_ui_wait_ticks(active_page);
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);

        while (xQueueReceive(s_key_event_queue, &key_event, 0U) == pdPASS)
        {
            ui_manager_handle_key(key_event.key_value);
        }

        while (xQueueReceive(s_ui_msg_queue, &ui_msg, 0U) == pdPASS)
        {
            ui_manager_handle_service_response(&ui_msg);
        }

        app_ui_update_thermal_page_state(&active_page, &previous_active_page);

        ui_manager_step();
        if (s_runtime_events != 0)
        {
            (void)xEventGroupSetBits(s_runtime_events, APP_EG_HB_UI);
        }
    }
}

static void thermal_task(void *pvParameters)
{
    EventBits_t event_bits = 0U;
    TickType_t last_wake_ticks = 0U;
    TickType_t period_ticks = 0U;
    uint8_t active_running = 0U;

    (void)pvParameters;

    while (1)
    {
        event_bits = (s_runtime_events != 0) ? xEventGroupGetBits(s_runtime_events) : 0U;
        if ((event_bits & APP_EG_BIT_THERMAL_ACTIVE) != 0U)
        {
            period_ticks = pdMS_TO_TICKS(redpic1_thermal_get_active_period_ms());
            if (period_ticks == 0U)
            {
                period_ticks = 1U;
            }

            if (active_running == 0U)
            {
                active_running = 1U;
                last_wake_ticks = xTaskGetTickCount();
                redpic1_thermal_step();
                continue;
            }

            vTaskDelayUntil(&last_wake_ticks, period_ticks);
            redpic1_thermal_step();
        }
        else
        {
            active_running = 0U;
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_THERMAL_IDLE_MS));
        }
    }
}

static void power_wdg_task(void *pvParameters)
{
    EventBits_t event_bits = 0U;
    uint32_t uart_error_flags = 0U;
    power_state_t owned_state = POWER_STATE_ACTIVE_UI;

    (void)pvParameters;
    owned_state = power_manager_get_state();
    app_runtime_set_screen_off_event(owned_state);

    while (1)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_POWER_LOOP_MS));
        owned_state = app_power_step_and_handle(owned_state);

        watchdog_service_begin_cycle();
        watchdog_service_mark_progress(WATCHDOG_PROGRESS_MAIN_LOOP);

        event_bits = (s_runtime_events != 0) ? xEventGroupGetBits(s_runtime_events) : 0U;
        if ((event_bits & APP_EG_HB_UI) != 0U ||
            (event_bits & APP_EG_BIT_SCREEN_OFF) != 0U ||
            (event_bits & (APP_EG_BIT_OTA_BUSY | APP_EG_BIT_WIFI_BUSY)) != 0U)
        {
            watchdog_service_mark_progress(WATCHDOG_PROGRESS_UI);
        }
        if ((event_bits & APP_EG_HB_SERVICE) != 0U ||
            (event_bits & (APP_EG_BIT_OTA_BUSY | APP_EG_BIT_WIFI_BUSY)) != 0U)
        {
            watchdog_service_mark_progress(WATCHDOG_PROGRESS_OTA |
                                           WATCHDOG_PROGRESS_ESP_HOST |
                                           WATCHDOG_PROGRESS_BATTERY);
        }
        else if ((event_bits & APP_EG_BIT_SCREEN_OFF) != 0U)
        {
            /* In screen-off idle, service task heartbeats can slip around STOP wake timing.
             * Keep watchdog health aligned with bare-metal behavior: these services are
             * intentionally quiescent and should not block STOP entry. */
            watchdog_service_mark_progress(WATCHDOG_PROGRESS_OTA |
                                           WATCHDOG_PROGRESS_ESP_HOST |
                                           WATCHDOG_PROGRESS_BATTERY);
        }

        watchdog_service_mark_progress(WATCHDOG_PROGRESS_POWER);
        watchdog_service_report_key_health(KEY_EXTI_IsHealthy());
        uart_error_flags = uart_rx_ring_take_error_flags();
        app_perf_baseline_record_uart_errors(uart_error_flags);
        watchdog_service_report_uart_errors(uart_error_flags);
        watchdog_service_step();

        low_power_runtime_step();
        owned_state = app_power_step_and_handle(owned_state);
        event_bits = (s_runtime_events != 0) ? xEventGroupGetBits(s_runtime_events) : 0U;
        app_perf_baseline_set_watchdog_snapshot(watchdog_service_get_missing_progress_mask(),
                                               watchdog_service_get_last_fault_flags());
        app_perf_baseline_set_runtime_state(owned_state,
                                            clock_profile_get(),
                                            ((event_bits & APP_EG_BIT_THERMAL_ACTIVE) != 0U) ? 1U : 0U,
                                            (owned_state == POWER_STATE_SCREEN_OFF_IDLE) ? 1U : 0U);
        app_perf_baseline_refresh_task_stacks(s_input_task_handle,
                                              s_service_task_handle,
                                              s_ui_task_handle,
                                              s_display_task_handle,
                                              s_thermal_task_handle,
                                              s_power_wdg_task_handle);

        if (s_runtime_events != 0)
        {
            (void)xEventGroupClearBits(s_runtime_events, APP_EG_HB_ALL);
            (void)xEventGroupSetBits(s_runtime_events, APP_EG_HB_POWER);
        }
    }
}

/* 初始化 RTOS 运行时依赖和跨任务共享对象。 */
void app_rtos_runtime_init(void)
{
    uint8_t display_runtime_ok = 0U;

    power_manager_init();
    rtc_lp_service_init();
    watchdog_service_init(1000UL);
    uart_init(115200);
    ota_service_init();
    settings_service_init();
    app_force_manual_wifi_boot();
    battery_monitor_init();
    low_power_runtime_init();
    if (low_power_runtime_handle_early_boot() != 0U)
    {
        app_panic_loop();
    }

    //LCD_Init();
    MYDMA_Config();
    KEY_Init();
    KEY_EXTI_Init();
    clock_profile_service_init();
    app_perf_baseline_init();

    redpic1_thermal_init();
    redpic1_thermal_suspend();
    power_manager_notify_activity();
    app_apply_persisted_settings();
    memset(s_service_deferred_cmd, 0, sizeof(s_service_deferred_cmd));
    memset(s_service_deferred_valid, 0, sizeof(s_service_deferred_valid));
    memset(s_service_pending, 0, sizeof(s_service_pending));

    s_key_event_queue = xQueueCreate(Q_KEY_EVENT_LEN, sizeof(app_key_event_t));
    s_service_cmd_queue = xQueueCreate(Q_SERVICE_CMD_LEN, sizeof(app_service_req_t));
    s_ui_msg_queue = xQueueCreate(Q_UI_MSG_LEN, sizeof(app_service_rsp_t));
    s_service_done_sem = xSemaphoreCreateBinary();
    s_service_sync_mutex = xSemaphoreCreateMutex();
    s_settings_mutex = xSemaphoreCreateRecursiveMutex();
    s_runtime_events = xEventGroupCreate();
    display_runtime_ok = app_display_runtime_init();
    redpic1_thermal_bind_display_runtime();

    if (s_key_event_queue == 0 ||
        s_service_cmd_queue == 0 ||
        s_ui_msg_queue == 0 ||
        s_service_done_sem == 0 ||
        s_service_sync_mutex == 0 ||
        s_settings_mutex == 0 ||
        display_runtime_ok == 0U ||
        s_runtime_events == 0)
    {
        app_panic_loop();
    }
}

/* 创建启动任务并进入 FreeRTOS 调度器。 */
void app_rtos_runtime_start(void)
{
    app_create_runtime_task(start_task,
                            "start_task",
                            (uint16_t)START_STK_SIZE,
                            (UBaseType_t)START_TASK_PRIO,
                            &s_start_task_handle);

    vTaskStartScheduler();
}

/* APP 主入口：完成底层启动后进入 RTOS 运行时。 */
void redpic1_app_main(void)
{
    delay_init(168);
    SystemInit();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    __enable_irq();
    app_rtos_runtime_init();
    app_rtos_runtime_start();

    while (1)
    {
    }
}
