/**
 * @file    redpic1_app.c
 * @brief   RedPic1 RTOS 应用运行时主控模块
 * @note    负责组织输入采集、后台服务、UI 刷新、热成像采集、
 *          电源管理及看门狗喂狗等核心任务的创建与调度。
 *          本文件同时对外提供页面层使用的设置访问互斥包装
 *          以及服务命令提交接口（同步/异步）。
 *
 * @par 重构说明
 *      本次重构不改变任务优先级、节拍周期、队列长度和服务执行语义，
 *      仅收口重复模板、统一命名并补齐中文说明，使代码更加符合
 *      企业级嵌入式项目规范。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "redpic1_app.h"

#include <stdio.h>
#include <string.h>

/* --- FreeRTOS 核心头文件 --- */
#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

/* --- 应用层服务模块 --- */
#include "app_display_runtime.h"
#include "app_perf_baseline.h"
#include "battery_monitor.h"
#include "clock_profile_service.h"
#include "delay.h"
#include "esp_sync_service.h"
#include "exti_key.h"
#include "key.h"
#include "lcd.h"
#include "lcd_dma.h"
#include "lcd_init.h"
#include "low_power_runtime.h"
#include "MLX90640_I2C_Driver.h"
#include "ota_ctrl_protocol.h"
#include "ota_service.h"
#include "power_manager.h"
#include "redpic1_thermal.h"
#include "rtc_lp_service.h"
#include "storage_service.h"
#include "sys.h"
#include "ui_manager.h"
#include "uart_rx_ring.h"
#include "usart.h"
#include "watchdog_service.h"
#include "esp_host_service_priv.h"
#include "led.h"

/* =========================================================================
 *  2. 宏定义 — 任务优先级与栈深度
 * ======================================================================= */

/** @defgroup APP_TASK_PRIO  任务优先级定义
 *  @{ */
#define START_TASK_PRIO                1U      /**< 启动任务优先级（最低）    */
#define INPUT_TASK_PRIO                8U      /**< 按键输入采集任务优先级    */
#define DISPLAY_TASK_PRIO              8U      /**< LCD 显示刷新任务优先级    */
#define SERVICE_TASK_PRIO              7U      /**< 后台服务任务优先级        */
#define UI_TASK_PRIO                   6U      /**< UI 逻辑处理任务优先级     */
#define THERMAL_TASK_PRIO              5U      /**< 热成像空闲态任务优先级    */
#define THERMAL_TASK_ACTIVE_PRIO       5U      /**< 热成像激活态任务优先级    */
#define POWER_WDG_TASK_PRIO            9U      /**< 电源与看门狗任务优先级（最高） */
/** @} */

/** @defgroup APP_TASK_STK  任务栈深度（单位：字，4 字节/字）
 *  @{ */
#define START_STK_SIZE                 512U    /**< 启动任务栈深度            */
#define INPUT_STK_SIZE                 384U    /**< 按键输入任务栈深度        */
#define DISPLAY_STK_SIZE               1280U   /**< 显示任务栈深度            */
#define SERVICE_STK_SIZE               1024U   /**< 服务任务栈深度            */
#define UI_STK_SIZE                    1280U   /**< UI 任务栈深度             */
#define THERMAL_STK_SIZE               1024U   /**< 热成像任务栈深度          */
#define POWER_WDG_STK_SIZE             640U    /**< 电源看门狗任务栈深度      */
/** @} */

/* =========================================================================
 *  3. 宏定义 — 消息队列长度
 * ======================================================================= */

/** @defgroup APP_QUEUE_LEN  消息队列深度
 *  @{ */
#define Q_KEY_EVENT_LEN                16U     /**< 按键事件队列可缓存条目数  */
#define Q_SERVICE_CMD_LEN              12U     /**< 服务命令队列可缓存条目数  */
#define Q_UI_MSG_LEN                   12U     /**< UI 响应消息队列深度       */
/** @} */

/* =========================================================================
 *  4. 宏定义 — 各任务主循环节拍（毫秒）
 * ======================================================================= */

/** @defgroup APP_LOOP_MS  任务循环周期
 *  @{ */
#define APP_INPUT_LOOP_MS              20U     /**< 输入任务等待节拍          */
#define APP_SERVICE_LOOP_MS            25U     /**< 服务任务常规等待节拍      */
#define APP_SERVICE_LOOP_MS_THERMAL    100U    /**< 服务任务热成像态等待节拍  */
#define APP_UI_LOOP_MS                 33U     /**< UI 任务常规等待节拍       */
#define APP_UI_LOOP_MS_THERMAL         100U    /**< UI 任务热成像态等待节拍   */
#define APP_THERMAL_IDLE_MS            30U     /**< 热成像空闲态等待节拍      */
#define APP_POWER_LOOP_MS              25U     /**< 电源看门狗任务等待节拍    */
#define APP_KEY2_LONG_MS               600UL   /**< KEY2 长按判定阈值（ms）   */
/** @} */

/* =========================================================================
 *  5. 宏定义 — ESP 同步流程参数
 * ======================================================================= */

/** @defgroup APP_ESP_SYNC  ESP 开机/唤醒同步参数
 *  @{ */
#define APP_ESP_BOOT_SYNC_DELAY_MS     1200UL  /**< 开机后首次同步延时        */
#define APP_ESP_BOOT_SYNC_RETRY_MS     1000UL  /**< 开机同步重试间隔          */
#define APP_ESP_BOOT_SYNC_MAX_TRIES    3U      /**< 开机同步最大重试次数      */
#define APP_ESP_RESUME_SYNC_DELAY_MS   20UL    /**< 唤醒后同步延时            */
#define APP_ESP_RESUME_SYNC_RETRY_MS   150UL   /**< 唤醒同步重试间隔          */
#define APP_ESP_RESUME_SYNC_MAX_TRIES  6U      /**< 唤醒同步最大重试次数      */
#define APP_OTA_QUERY_SYNC_DRAIN_MS    1800UL  /**< OTA 查询前排空同步超时    */
#define APP_OTA_QUERY_SYNC_RETRY_MS    20UL    /**< OTA 查询排空间隔          */
/** @} */

/** @defgroup APP_RSP_VALUE  服务响应特殊值
 *  @{ */
#define APP_RSP_VALUE_OTA_SYNC_PENDING 1UL     /**< OTA 查询返回：同步未完成  */
/** @} */

/* =========================================================================
 *  6. 宏定义 — 事件组位域
 * ======================================================================= */

/** @defgroup APP_EG_BIT  运行时事件组 — 功能状态位
 *  @{ */
#define APP_EG_BIT_THERMAL_ACTIVE      (1UL << 0)  /**< 热成像页面处于激活态  */
#define APP_EG_BIT_SCREEN_OFF          (1UL << 1)  /**< 屏幕已关闭（低功耗）  */
#define APP_EG_BIT_OTA_BUSY            (1UL << 2)  /**< OTA 通道正忙          */
#define APP_EG_BIT_WIFI_BUSY           (1UL << 3)  /**< WiFi 通道正忙         */
/** @} */

/** @defgroup APP_EG_HB  运行时事件组 — 任务心跳位
 *  @note  电源看门狗任务在每个喂狗窗口末尾清除所有心跳位，
 *         以此判断各子任务是否在窗口期内产生过活动。
 *  @{ */
#define APP_EG_HB_INPUT                (1UL << 8)  /**< 输入任务心跳          */
#define APP_EG_HB_UI                   (1UL << 9)  /**< UI 任务心跳           */
#define APP_EG_HB_SERVICE              (1UL << 10) /**< 服务任务心跳          */
#define APP_EG_HB_POWER                (1UL << 11) /**< 电源任务心跳          */
#define APP_EG_HB_ALL                  (APP_EG_HB_INPUT  | \
                                        APP_EG_HB_UI     | \
                                        APP_EG_HB_SERVICE | \
                                        APP_EG_HB_POWER)    /**< 全部心跳掩码  */
/** @} */

/* =========================================================================
 *  7. 内部数据类型定义
 * ======================================================================= */

/**
 * @brief 服务命令请求封装
 * @note  包含命令体本身、同步返回缓冲区指针以及等待标志，
 *        由页面层通过 app_service_bus 提交到服务命令队列。
 */
typedef struct
{
    app_service_cmd_t   cmd;            /**< 实际服务命令内容                */
    app_service_rsp_t  *sync_rsp;       /**< 同步调用时的返回缓冲区，异步为 NULL */
    uint8_t             sync_wait;      /**< 1 表示调用方正在等待完成信号量  */
} app_service_req_t;

/**
 * @brief 输入任务内部状态
 * @note  用于区分 KEY2 的短按与长按：按键按下后进入挂起态，
 *        若超时阈值内释放则判定为短按，否则判定为长按。
 */
typedef struct
{
    uint8_t  key2_pending;              /**< KEY2 是否处于"等待区分短按/长按"挂起态 */
    uint32_t key2_press_start_ms;       /**< KEY2 进入挂起状态时的系统毫秒节拍     */
} app_input_state_t;

/**
 * @brief 服务任务内部状态
 * @note  跟踪 ESP 开机/唤醒后的配置同步流程进展，
 *        以及服务任务上一次观察到的电源状态。
 */
typedef struct
{
    uint8_t         esp_boot_sync_pending;      /**< 同步流程是否仍需继续          */
    uint8_t         esp_boot_sync_tries;        /**< 已执行的同步重试次数          */
    uint32_t        esp_boot_sync_next_ms;      /**< 下一次允许发起同步的时间点    */
    uint32_t        esp_boot_sync_retry_ms;     /**< 同步失败后的重试间隔          */
    uint8_t         esp_boot_sync_max_tries;    /**< 最大允许重试次数              */
    power_state_t   last_power_state;           /**< 服务任务上一次观察到的电源状态 */
} app_service_state_t;

/* =========================================================================
 *  8. 模块级静态变量（任务句柄 / 队列 / 同步原语）
 * ======================================================================= */

/* ---- 任务句柄 ---- */
static TaskHandle_t s_start_task_handle     = 0;    /**< 启动任务句柄          */
static TaskHandle_t s_input_task_handle     = 0;    /**< 输入任务句柄          */
static TaskHandle_t s_display_task_handle   = 0;    /**< 显示任务句柄          */
static TaskHandle_t s_service_task_handle   = 0;    /**< 服务任务句柄          */
static TaskHandle_t s_ui_task_handle        = 0;    /**< UI 任务句柄           */
static TaskHandle_t s_thermal_task_handle   = 0;    /**< 热成像任务句柄        */
static TaskHandle_t s_power_wdg_task_handle = 0;    /**< 电源看门狗任务句柄    */

/* ---- 消息队列 / 同步原语 ---- */
static QueueHandle_t      s_key_event_queue = 0;    /**< 按键事件队列          */
static QueueHandle_t      s_ui_msg_queue    = 0;    /**< UI 响应消息队列       */
static SemaphoreHandle_t  s_settings_mutex  = 0;    /**< 设置读写递归互斥锁    */
static EventGroupHandle_t s_runtime_events  = 0;    /**< 运行时事件组          */

/* ---- 输入任务状态 ---- */
static app_input_state_t  s_input_state;            /**< 输入任务内部状态      */

/* =========================================================================
 *  9. 内部函数前向声明
 * ======================================================================= */

/* ---- RTOS 任务入口 ---- */
static void start_task      (void *pvParameters);
static void input_task      (void *pvParameters);
static void service_task    (void *pvParameters);
static void ui_task         (void *pvParameters);
static void thermal_task    (void *pvParameters);
static void power_wdg_task  (void *pvParameters);

/* ---- 初始化 / 配置 ---- */
static void     app_apply_persisted_settings    (void);
static void     app_force_manual_wifi_boot      (void);

/* ---- 服务命令执行 ---- */
static uint8_t  app_service_execute_command     (const app_service_cmd_t *cmd,
                                                  app_service_rsp_t *rsp);
static void     app_service_poll_background     (uint32_t now_ms);
static uint8_t  app_service_drain_esp_sync      (uint32_t timeout_ms);

/* ---- UI 辅助 ---- */
static void     app_ui_push_key                 (uint8_t key_value);
static void     app_ui_notify_task              (void);
static void     app_ui_update_thermal_page_state(ui_page_id_t *active_page,
                                                  ui_page_id_t *previous_active_page);

/* ---- 热成像 / 电源辅助 ---- */
static void             app_thermal_notify_task             (void);
static void             app_power_notify_task_from_isr      (BaseType_t *higher_priority_task_woken);
static power_state_t    app_power_step_and_handle           (power_state_t previous_state);
static void             app_runtime_set_screen_off_event    (power_state_t state);

/* ---- 工具函数 ---- */
static uint8_t      app_is_scheduler_running    (void);
static uint8_t      app_runtime_thermal_active  (void);
static TickType_t   app_service_wait_ticks      (void);
static TickType_t   app_ui_wait_ticks           (ui_page_id_t active_page);
static void         app_thermal_set_priority    (uint8_t thermal_active);
static void         app_create_runtime_task     (TaskFunction_t task_entry,
                                                  const char *task_name,
                                                  uint16_t stack_size,
                                                  UBaseType_t task_priority,
                                                  TaskHandle_t *task_handle);
static void         app_kick_initial_tasks      (void);
static void         app_panic_loop              (void);

/* =========================================================================
 *  10. 工具函数实现
 * ======================================================================= */

/**
 * @brief  不可恢复错误死循环
 * @note   统一承接初始化 / 创建过程中的致命失败，
 *         保持原有死循环行为不变，便于外部调试器捕获。
 */
static void app_panic_loop(void)
{
    for (;;)
    {
        /* 等待外部调试器介入或看门狗复位 */
    }
}

/**
 * @brief  判断 FreeRTOS 调度器是否正在运行
 * @retval 1 — 调度器运行中；0 — 尚未启动或已挂起
 */
static uint8_t app_is_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

/**
 * @brief  查询热成像页面是否处于激活态
 * @retval 1 — 激活；0 — 未激活或事件组尚未创建
 */
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

/**
 * @brief  根据当前运行态计算服务任务等待节拍数
 * @note   非热成像态使用短节拍；热成像态下若无待处理事务则拉长节拍以降低功耗。
 * @return TickType_t — 传递给 ulTaskNotifyTake 的超时节拍
 */
static TickType_t app_service_wait_ticks(void)
{
    EventBits_t event_bits = 0U;

    /* 非热成像态：始终使用短节拍 */
    if (app_runtime_thermal_active() == 0U)
    {
        return pdMS_TO_TICKS(APP_SERVICE_LOOP_MS);
    }

    /* 热成像态：若 OTA/WiFi 正忙、ESP 同步挂起或服务总线有待处理命令，
     * 仍使用短节拍以保证响应时效；否则拉长节拍降低 CPU 占用。 */
    event_bits = (s_runtime_events != 0) ? xEventGroupGetBits(s_runtime_events) : 0U;

    if ((event_bits & (APP_EG_BIT_OTA_BUSY | APP_EG_BIT_WIFI_BUSY)) != 0U ||
        esp_sync_service_is_pending() != 0U ||
        app_service_bus_has_pending_work() != 0U)
    {
        return pdMS_TO_TICKS(APP_SERVICE_LOOP_MS);
    }

    return pdMS_TO_TICKS(APP_SERVICE_LOOP_MS_THERMAL);
}

/**
 * @brief  根据当前活跃页面计算 UI 任务等待节拍数
 * @param  active_page — 当前活跃的 UI 页面 ID
 * @return TickType_t — 传递给 ulTaskNotifyTake 的超时节拍
 */
static TickType_t app_ui_wait_ticks(ui_page_id_t active_page)
{
    /* 非热成像页面：使用标准 UI 节拍 */
    if (active_page != UI_PAGE_THERMAL)
    {
        return pdMS_TO_TICKS(APP_UI_LOOP_MS);
    }

    /* 热成像页面：若队列中有待处理消息则保持快节拍 */
    if ((s_key_event_queue != 0 && uxQueueMessagesWaiting(s_key_event_queue) != 0U) ||
        (s_ui_msg_queue != 0 && uxQueueMessagesWaiting(s_ui_msg_queue) != 0U))
    {
        return pdMS_TO_TICKS(APP_UI_LOOP_MS);
    }

    return pdMS_TO_TICKS(APP_UI_LOOP_MS_THERMAL);
}

/**
 * @brief  统一创建运行时任务
 * @note   封装 xTaskCreate 并在失败时进入 panic 死循环，
 *         避免每个创建点重复展开同样的失败处理模板。
 * @param  task_entry   — 任务入口函数
 * @param  task_name    — 任务名称字符串（用于调试）
 * @param  stack_size   — 栈深度（单位：字）
 * @param  task_priority — 任务优先级
 * @param  task_handle  — 输出：任务句柄指针
 */
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

/**
 * @brief  动态调整热成像任务优先级
 * @note   激活态提升优先级以保证采集实时性，空闲态降低以让出 CPU。
 * @param  thermal_active — 1=激活态，0=空闲态
 */
static void app_thermal_set_priority(uint8_t thermal_active)
{
    if (s_thermal_task_handle == 0)
    {
        return;
    }

    vTaskPrioritySet(s_thermal_task_handle,
                     (thermal_active != 0U)
                         ? (UBaseType_t)THERMAL_TASK_ACTIVE_PRIO
                         : (UBaseType_t)THERMAL_TASK_PRIO);
}

/* =========================================================================
 *  11. LCD 显示互斥接口（供页面层调用）
 * ======================================================================= */

/**
 * @brief  获取 LCD 显示锁
 * @note   内部委托给 app_display_runtime 模块的锁实现，
 *         确保 DMA 刷新与 UI 绘制不产生竞争。
 */
void app_rtos_lcd_lock(void)
{
    app_display_runtime_lock();
}

/**
 * @brief  释放 LCD 显示锁
 */
void app_rtos_lcd_unlock(void)
{
    app_display_runtime_unlock();
}

/* =========================================================================
 *  12. 设备设置互斥接口（供页面层调用）
 * ======================================================================= */

/**
 * @brief  获取设备设置递归互斥锁
 * @note   使用递归互斥锁，允许同一线程嵌套加锁。
 *         若调度器未运行或锁尚未创建，则静默跳过。
 */
void app_rtos_settings_lock(void)
{
    if (s_settings_mutex != 0 && app_is_scheduler_running() != 0U)
    {
        (void)xSemaphoreTakeRecursive(s_settings_mutex, portMAX_DELAY);
    }
}

/**
 * @brief  释放设备设置递归互斥锁
 */
void app_rtos_settings_unlock(void)
{
    if (s_settings_mutex != 0 && app_is_scheduler_running() != 0U)
    {
        (void)xSemaphoreGiveRecursive(s_settings_mutex);
    }
}

/**
 * @brief  线程安全地读取当前设备设置快照
 * @param  out_settings — 输出：设置结构体指针，不可为 NULL
 */
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

/**
 * @brief  线程安全地更新设备设置
 * @param  settings — 新设置结构体指针，不可为 NULL
 * @retval 1 — 更新成功；0 — 参数无效或写入失败
 */
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

/* =========================================================================
 *  13. 初始化辅助函数
 * ======================================================================= */

/**
 * @brief  将持久化设置应用到电源管理器
 * @note   在系统初始化阶段调用一次，恢复上次保存的电源策略与屏幕超时。
 */
static void app_apply_persisted_settings(void)
{
    device_settings_t settings;

    app_rtos_settings_copy(&settings);
    power_manager_set_policy(settings.power_policy);
    power_manager_set_screen_off_timeout_ms(settings.screen_off_timeout_ms);
}

/**
 * @brief  强制关闭 WiFi 自动启动
 * @note   若持久化设置中 WiFi 已启用，则在首次开机时将其关闭，
 *         等待用户手动开启，避免意外联网消耗电量。
 */
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

/* =========================================================================
 *  14. 任务间通知辅助函数
 * ======================================================================= */

/**
 * @brief  通知 UI 任务有新事件待处理
 * @note   同时记录性能基准中的通知事件。
 */
static void app_ui_notify_task(void)
{
    if (s_ui_task_handle != 0)
    {
        app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_UI);
        xTaskNotifyGive(s_ui_task_handle);
    }
}

/**
 * @brief  通知热成像任务有新事件待处理
 */
static void app_thermal_notify_task(void)
{
    if (s_thermal_task_handle != 0)
    {
        xTaskNotifyGive(s_thermal_task_handle);
    }
}

/**
 * @brief  从中断上下文通知电源看门狗任务
 * @param  higher_priority_task_woken — 输出：是否需要在 ISR 退出时触发上下文切换
 */
static void app_power_notify_task_from_isr(BaseType_t *higher_priority_task_woken)
{
    if (s_power_wdg_task_handle != 0)
    {
        vTaskNotifyGiveFromISR(s_power_wdg_task_handle, higher_priority_task_woken);
    }
}

/* =========================================================================
 *  15. 电源状态处理
 * ======================================================================= */

/**
 * @brief  根据电源状态设置/清除屏幕关闭事件位
 * @param  state — 当前电源状态
 */
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

/**
 * @brief  执行一次电源管理步进并处理状态转换
 * @note   在状态切换时负责 LCD 休眠/唤醒、UI 全量刷新及通知服务任务。
 * @param  previous_state — 上一次的电源状态
 * @return power_state_t  — 转换后的当前电源状态
 */
static power_state_t app_power_step_and_handle(power_state_t previous_state)
{
    power_state_t current_state = previous_state;

    power_manager_step();
    current_state = power_manager_get_state();

    /* 状态发生变化时执行相应动作 */
    if (current_state != previous_state)
    {
        /* 进入屏幕关闭态：LCD 休眠 */
        if (current_state == POWER_STATE_SCREEN_OFF_IDLE)
        {
            (void)app_display_runtime_sleep();
        }
        /* 从屏幕关闭态唤醒：LCD 恢复 + UI 全量刷新 */
        else if (previous_state == POWER_STATE_SCREEN_OFF_IDLE)
        {
            (void)app_display_runtime_wake();
            ui_manager_force_full_refresh();
            app_ui_notify_task();
        }

        /* 通知服务任务电源状态已变化 */
        app_service_bus_notify_service_task();
    }

    app_runtime_set_screen_off_event(current_state);
    return current_state;
}

/* =========================================================================
 *  16. 服务命令执行引擎
 * ======================================================================= */

/**
 * @brief  根据命令 ID 返回需要置位的忙标志位
 * @param  cmd_id — 服务命令 ID
 * @return EventBits_t — 对应的事件组忙标志位，无需忙标志则返回 0
 */
static EventBits_t app_service_cmd_busy_bits(app_service_cmd_id_t cmd_id)
{
    switch (cmd_id)
    {
    /* WiFi / BLE / MQTT 等 ESP 相关命令统一使用 WIFI_BUSY 标志 */
    case APP_SERVICE_CMD_SET_WIFI:
    case APP_SERVICE_CMD_SET_BLE:
    case APP_SERVICE_CMD_SET_MQTT:
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

    /* OTA 查询使用独立的 OTA_BUSY 标志 */
    case APP_SERVICE_CMD_OTA_QUERY_LATEST:
        return APP_EG_BIT_OTA_BUSY;

    default:
        return 0U;
    }
}

/**
 * @brief  执行单条服务命令并填充响应结构体
 * @note   本函数由服务总线回调调用，在服务任务上下文中运行。
 *         执行前设置忙标志，执行后清除，以便看门狗判断通道活跃状态。
 * @param  cmd — 输入：命令结构体指针
 * @param  rsp — 输出：响应结构体指针
 * @retval 1 — 命令执行成功；0 — 执行失败或参数无效
 */
static uint8_t app_service_execute_command(const app_service_cmd_t *cmd,
                                           app_service_rsp_t *rsp)
{
    char        latest_version[APP_SERVICE_TEXT_LEN];
    uint16_t    reject_reason = 0U;
    EventBits_t busy_bits     = 0U;
    uint8_t     ok            = 0U;

    if (cmd == 0 || rsp == 0)
    {
        return 0U;
    }

    /* 初始化响应结构体 */
    memset(rsp, 0, sizeof(*rsp));
    rsp->cmd_id = cmd->cmd_id;

    /* 根据命令类型设置忙标志位 */
    busy_bits = app_service_cmd_busy_bits(cmd->cmd_id);
    if (busy_bits != 0U && s_runtime_events != 0)
    {
        (void)xEventGroupSetBits(s_runtime_events, busy_bits);
    }

    /* 按命令 ID 分发执行 */
    switch (cmd->cmd_id)
    {
    /* --- ESP 状态刷新 --- */
    case APP_SERVICE_CMD_ESP_REFRESH_STATUS:
        ok = esp_host_refresh_status();
        break;

    /* --- WiFi 开关控制 --- */
    case APP_SERVICE_CMD_SET_WIFI:
        ok = esp_host_set_wifi_now(cmd->arg0, cmd->value);
        break;

    /* --- BLE 开关控制 --- */
    case APP_SERVICE_CMD_SET_BLE:
        ok = esp_host_set_ble_now(cmd->arg0);
        break;

    /* --- MQTT 开关控制 --- */
    case APP_SERVICE_CMD_SET_MQTT:
        ok = esp_host_set_mqtt_now(cmd->arg0);
        break;

    /* --- 调试屏幕开关控制 --- */
    case APP_SERVICE_CMD_SET_DEBUG_SCREEN:
        ok = esp_host_set_debug_screen_now(cmd->arg0);
        break;

    /* --- 远程按键开关控制 --- */
    case APP_SERVICE_CMD_SET_REMOTE_KEYS:
        ok = esp_host_set_remote_keys_now(cmd->arg0);
        break;

    /* --- 电源策略设置 --- */
    case APP_SERVICE_CMD_SET_POWER_POLICY:
        ok = esp_host_set_power_policy_now((power_policy_t)cmd->arg0);
        break;

    /* --- 主机状态设置 --- */
    case APP_SERVICE_CMD_SET_HOST_STATE:
        ok = esp_host_set_host_state_now((power_state_t)cmd->arg0);
        break;

    /* --- 强制深度睡眠 --- */
    case APP_SERVICE_CMD_ENTER_FORCED_DEEP_SLEEP:
        ok = esp_host_enter_forced_deep_sleep_now(cmd->value);
        break;

    /* --- 准备停机 --- */
    case APP_SERVICE_CMD_PREPARE_STOP:
        ok = esp_host_prepare_for_stop(cmd->value);
        break;

    /* --- 准备待机 --- */
    case APP_SERVICE_CMD_PREPARE_STANDBY:
        ok = esp_host_prepare_for_standby(cmd->value);
        break;

    /* --- 发送热成像快照数据 --- */
    case APP_SERVICE_CMD_SEND_THERMAL_SNAPSHOT:
    {
        /* 从 cmd->value 中拆分最小温度和最大温度（x10 编码） */
        int16_t min_temp_x10    = (int16_t)(cmd->value & 0xFFFFU);
        int16_t max_temp_x10    = (int16_t)((cmd->value >> 16) & 0xFFFFU);
        /* 从 cmd->arg0/arg1 中拼接中心温度（x10 编码） */
        int16_t center_temp_x10 = (int16_t)((uint16_t)cmd->arg0 |
                                             ((uint16_t)cmd->arg1 << 8));

        ok = esp_host_send_thermal_snapshot_x10(min_temp_x10,
                                                max_temp_x10,
                                                center_temp_x10);
    }
        break;

    /* --- OTA 版本查询 --- */
    case APP_SERVICE_CMD_OTA_QUERY_LATEST:
        memset(latest_version, 0, sizeof(latest_version));

        /* 查询前先排空 ESP 同步队列，避免读到过时状态 */
        if (app_service_drain_esp_sync(APP_OTA_QUERY_SYNC_DRAIN_MS) == 0U)
        {
            rsp->reason = OTA_CTRL_ERR_BUSY;
            rsp->value  = APP_RSP_VALUE_OTA_SYNC_PENDING;
            break;
        }

        /* 获取 OTA 电源锁，防止查询期间进入低功耗态 */
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

    /* --- 无效命令 --- */
    case APP_SERVICE_CMD_NONE:
    default:
        ok = 0U;
        break;
    }

    /* 填充执行结果并清除忙标志位 */
    rsp->ok = ok;
    if (busy_bits != 0U && s_runtime_events != 0)
    {
        (void)xEventGroupClearBits(s_runtime_events, busy_bits);
    }

    return ok;
}

/* =========================================================================
 *  17. 按键中断回调
 * ======================================================================= */

/**
 * @brief  外部按键中断回调函数
 * @note   由 EXTI 中断服务程序调用，通过 TaskNotify 唤醒输入任务，
 *         同时唤醒电源看门狗任务以感知用户活动。
 */
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

/* =========================================================================
 *  18. UI 按键事件推送
 * ======================================================================= */

/**
 * @brief  将按键事件推送到 UI 消息队列
 * @note   若队列已满，丢弃最旧的事件并记录丢弃计数，
 *         确保最新的按键事件始终能够入队。
 * @param  key_value — 按键值
 */
static void app_ui_push_key(uint8_t key_value)
{
    app_key_event_t event_item;

    if (s_key_event_queue == 0 || key_value == 0U)
    {
        return;
    }

    event_item.key_value = key_value;
    event_item.tick_ms   = power_manager_get_tick_ms();

    /* 尝试入队；若队列满则丢弃最旧事件后重试 */
    if (xQueueSendToBack(s_key_event_queue, &event_item, 0U) != pdPASS)
    {
        app_key_event_t dropped;

        app_perf_baseline_record_key_queue_drop();
        (void)xQueueReceive(s_key_event_queue, &dropped, 0U);
        (void)xQueueSendToBack(s_key_event_queue, &event_item, 0U);
    }

    /* 通知 UI 任务处理新事件 */
    if (s_ui_task_handle != 0)
    {
        app_ui_notify_task();
    }
}

/* =========================================================================
 *  19. 启动阶段辅助
 * ======================================================================= */

/**
 * @brief  启动阶段统一拉起所有初始任务
 * @note   保持原有首次通知顺序：输入 -> UI -> 服务总线 -> 电源。
 */
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
        app_service_bus_notify_service_task();
    }

    if (s_power_wdg_task_handle != 0)
    {
        xTaskNotifyGive(s_power_wdg_task_handle);
    }
}

/* =========================================================================
 *  20. RTOS 任务实现
 * ======================================================================= */

/* --------------------------------------------------------------------------
 *  20.1  启动任务
 * ----------------------------------------------------------------------- */

/**
 * @brief  启动任务入口
 * @note   仅负责创建各运行时任务，不承载业务逻辑。
 *         所有子任务创建完成后自行删除。
 */
static void start_task(void *pvParameters)
{
    (void)pvParameters;

    /* 创建输入采集任务 */
    app_create_runtime_task(input_task,
                            "input_task",
                            (uint16_t)INPUT_STK_SIZE,
                            (UBaseType_t)INPUT_TASK_PRIO,
                            &s_input_task_handle);

    /* 创建后台服务任务 */
    app_create_runtime_task(service_task,
                            "service_task",
                            (uint16_t)SERVICE_STK_SIZE,
                            (UBaseType_t)SERVICE_TASK_PRIO,
                            &s_service_task_handle);
    app_service_bus_set_service_task_handle(s_service_task_handle);

    /* 创建 LCD 显示刷新任务 */
    app_create_runtime_task(app_display_runtime_task,
                            "display_task",
                            (uint16_t)DISPLAY_STK_SIZE,
                            (UBaseType_t)DISPLAY_TASK_PRIO,
                            &s_display_task_handle);

    /* 创建 UI 逻辑处理任务 */
    app_create_runtime_task(ui_task,
                            "ui_task",
                            (uint16_t)UI_STK_SIZE,
                            (UBaseType_t)UI_TASK_PRIO,
                            &s_ui_task_handle);
    app_service_bus_set_ui_task_handle(s_ui_task_handle);

    /* 创建热成像采集任务 */
    app_create_runtime_task(thermal_task,
                            "thermal_task",
                            (uint16_t)THERMAL_STK_SIZE,
                            (UBaseType_t)THERMAL_TASK_PRIO,
                            &s_thermal_task_handle);

    /* 创建电源与看门狗任务 */
    app_create_runtime_task(power_wdg_task,
                            "power_wdg_task",
                            (uint16_t)POWER_WDG_STK_SIZE,
                            (UBaseType_t)POWER_WDG_TASK_PRIO,
                            &s_power_wdg_task_handle);

    /* 首次唤醒所有任务 */
    app_kick_initial_tasks();

    /* 启动任务使命完成，自行删除 */
    vTaskDelete((TaskHandle_t)0);
}

/* --------------------------------------------------------------------------
 *  20.2  按键输入采集任务
 * ----------------------------------------------------------------------- */

/**
 * @brief  按键输入采集任务
 * @note   轮询底层按键驱动获取键值，KEY2 支持短按/长按区分：
 *         - 按下后进入挂起态，持续监测时长
 *         - 超过 600ms 判定为长按，松手前触发长按事件
 *         - 600ms 内松手则判定为短按
 *         其他按键直接推送到 UI 队列。
 */
static void input_task(void *pvParameters)
{
    uint8_t    key_value  = 0U;
    TickType_t wait_ticks = portMAX_DELAY;

    (void)pvParameters;
    memset(&s_input_state, 0, sizeof(s_input_state));

    while (1)
    {
        /* KEY2 挂起态使用短超时轮询，否则阻塞等待中断通知 */
        wait_ticks = (s_input_state.key2_pending != 0U)? pdMS_TO_TICKS(APP_INPUT_LOOP_MS): portMAX_DELAY;
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);

        /* 消费所有待处理按键事件 */
        while ((key_value = KEY_GetValue()) != 0U)
        {
            if (key_value == KEY2_PRES)
            {
                /* KEY2 按下：进入挂起态，记录按下时刻 */
                s_input_state.key2_pending       = 1U;
                s_input_state.key2_press_start_ms = power_manager_get_tick_ms();
            }
            else
            {
                /* 其他按键：直接推送到 UI 队列 */
                app_ui_push_key(key_value);
            }
        }

        /* KEY2 挂起态处理：区分短按与长按 */
        if (s_input_state.key2_pending != 0U)
        {
            uint32_t now_ms = power_manager_get_tick_ms();

            if (KEY_IsLogicalPressed(KEY2_PRES) != 0U)
            {
                /* 按键仍按住：检查是否超过长按阈值 */
                if ((now_ms - s_input_state.key2_press_start_ms) >= APP_KEY2_LONG_MS)
                {
                    s_input_state.key2_pending = 0U;
                    app_ui_push_key(UI_KEY_KEY2_LONG);
                }
            }
            else
            {
                /* 按键已松开：未超过长按阈值，判定为短按 */
                s_input_state.key2_pending = 0U;
                app_ui_push_key(KEY2_PRES);
            }
        }

        /* 置位输入任务心跳标志 */
        if (s_runtime_events != 0)
        {
            (void)xEventGroupSetBits(s_runtime_events, APP_EG_HB_INPUT);
        }
    }
}

/* --------------------------------------------------------------------------
 *  20.3  后台服务任务
 * ----------------------------------------------------------------------- */

/**
 * @brief  服务任务后台轮询处理
 * @note   依次执行 ESP 同步、OTA 轮询、ESP 主机步进、电池监测。
 * @param  now_ms — 当前系统毫秒节拍
 */
static void app_service_poll_background(uint32_t now_ms)
{
    esp_sync_service_step(now_ms);
    ota_service_poll();
    esp_host_step();
    battery_monitor_step();

    /* 处理 ESP 唤醒后的同步恢复 */
    esp_sync_service_handle_power_state(power_manager_get_state(),
                                        APP_ESP_RESUME_SYNC_DELAY_MS,
                                        APP_ESP_RESUME_SYNC_RETRY_MS,
                                        APP_ESP_RESUME_SYNC_MAX_TRIES);
}

/**
 * @brief  排空 ESP 同步队列（阻塞式）
 * @note   在 OTA 查询前调用，确保 ESP 已完成所有待处理同步操作。
 * @param  timeout_ms — 最大等待超时（毫秒）
 * @retval 1 — 同步队列已排空；0 — 超时未排空
 */
static uint8_t app_service_drain_esp_sync(uint32_t timeout_ms)
{
    uint32_t start_ms = power_manager_get_tick_ms();

    while (esp_sync_service_is_pending() != 0U)
    {
        uint32_t now_ms = power_manager_get_tick_ms();

        esp_sync_service_step(now_ms);

        /* 同步完成 */
        if (esp_sync_service_is_pending() == 0U)
        {
            return 1U;
        }

        /* 超时退出 */
        if ((now_ms - start_ms) >= timeout_ms)
        {
            return 0U;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_OTA_QUERY_SYNC_RETRY_MS));
    }

    return 1U;
}

/**
 * @brief  后台服务任务入口
 * @note   负责 ESP 同步、OTA 轮询、电池监测等后台操作。
 *         通过 TaskNotify 唤醒，无通知时按自适应节拍休眠。
 */
static void service_task(void *pvParameters)
{
    uint32_t   now_ms     = 0U;
    TickType_t wait_ticks = pdMS_TO_TICKS(APP_SERVICE_LOOP_MS);

    (void)pvParameters;

    /* 初始化 ESP 主机服务并调度首次同步 */
    esp_host_init();
    esp_sync_service_reset(power_manager_get_state());
    esp_sync_service_schedule(APP_ESP_BOOT_SYNC_DELAY_MS,
                              APP_ESP_BOOT_SYNC_RETRY_MS,
                              APP_ESP_BOOT_SYNC_MAX_TRIES);

    while (1)
    {
        /* 自适应等待：根据当前运行态选择短/长节拍 */
        wait_ticks = app_service_wait_ticks();
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);

        /* 处理服务总线命令 */
        app_service_bus_process();
        (void)app_service_bus_try_enqueue_deferred_any();

        /* 后台轮询 */
        now_ms = power_manager_get_tick_ms();
        app_service_poll_background(now_ms);

        /* 置位服务任务心跳标志 */
        if (s_runtime_events != 0)
        {
            (void)xEventGroupSetBits(s_runtime_events, APP_EG_HB_SERVICE);
        }
    }
}

/* --------------------------------------------------------------------------
 *  20.4  UI 逻辑处理任务
 * ----------------------------------------------------------------------- */

/**
 * @brief  同步 UI 活跃页面与热成像任务的联动状态
 * @note   每次 UI step 前调用，当页面切入/切出热成像页面时：
 *         - 设置/清除 THERMAL_ACTIVE 事件位
 *         - 调整热成像任务优先级
 *         - 通知热成像任务
 * @param  active_page         — [in/out] 当前活跃页面 ID
 * @param  previous_active_page — [in/out] 上一次活跃页面 ID
 */
static void app_ui_update_thermal_page_state(ui_page_id_t *active_page,
                                             ui_page_id_t *previous_active_page)
{
    if (active_page == 0 || previous_active_page == 0)
    {
        return;
    }

    *previous_active_page = *active_page;
    *active_page = ui_manager_get_active_page();

    /* 更新热成像激活事件位 */
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

    /* 页面切换涉及热成像页面时，调整优先级并通知热成像任务 */
    if (*active_page != *previous_active_page &&
        (*active_page == UI_PAGE_THERMAL || *previous_active_page == UI_PAGE_THERMAL))
    {
        app_thermal_set_priority((*active_page == UI_PAGE_THERMAL) ? 1U : 0U);
        app_thermal_notify_task();
    }
}

/**
 * @brief  UI 逻辑处理任务入口
 * @note   消费按键事件队列和服务响应队列，
 *         驱动 UI 管理器进行页面渲染与状态更新。
 */
static void ui_task(void *pvParameters)
{
    app_key_event_t  key_event;
    app_service_rsp_t ui_msg;
    TickType_t       wait_ticks          = pdMS_TO_TICKS(APP_UI_LOOP_MS);
    ui_page_id_t     active_page         = UI_PAGE_HOME;
    ui_page_id_t     previous_active_page = UI_PAGE_HOME;

    (void)pvParameters;
    ui_manager_init();

    while (1)
    {
        /* 自适应等待 */
        wait_ticks = app_ui_wait_ticks(active_page);
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);

        /* 消费所有待处理按键事件 */
        while (xQueueReceive(s_key_event_queue, &key_event, 0U) == pdPASS)
        {
            ui_manager_handle_key(key_event.key_value);
        }

        /* 消费所有待处理服务响应消息 */
        while (xQueueReceive(s_ui_msg_queue, &ui_msg, 0U) == pdPASS)
        {
            ui_manager_handle_service_response(&ui_msg);
        }

        /* 同步热成像页面联动状态 */
        app_ui_update_thermal_page_state(&active_page, &previous_active_page);

        /* 驱动 UI 管理器步进 */
        ui_manager_step();

        /* 置位 UI 任务心跳标志 */
        if (s_runtime_events != 0)
        {
            (void)xEventGroupSetBits(s_runtime_events, APP_EG_HB_UI);
        }
    }
}

/* --------------------------------------------------------------------------
 *  20.5  热成像采集任务
 * ----------------------------------------------------------------------- */

/**
 * @brief  热成像采集任务入口
 * @note   两种工作模式：
 *         - 激活态（热成像页面可见）：按传感器帧率周期性采集
 *         - 空闲态（非热成像页面）：低功耗等待，仅响应通知唤醒
 */
static void thermal_task(void *pvParameters)
{
    EventBits_t event_bits      = 0U;
    TickType_t  last_wake_ticks = 0U;
    TickType_t  period_ticks    = 0U;
    uint8_t     active_running  = 0U;

    (void)pvParameters;

    while (1)
    {
        event_bits = (s_runtime_events != 0)? xEventGroupGetBits(s_runtime_events): 0U;

        if ((event_bits & APP_EG_BIT_THERMAL_ACTIVE) != 0U)
        {
            /* ---- 激活态：周期性采集 ---- */
            period_ticks = pdMS_TO_TICKS(redpic1_thermal_get_active_period_ms());
            if (period_ticks == 0U)
            {
                period_ticks = 1U;  /* 防止零延时导致 CPU 空转 */
            }

            /* 首次进入激活态：立即采集一次 */
            if (active_running == 0U)
            {
                active_running   = 1U;
                last_wake_ticks  = xTaskGetTickCount();
                redpic1_thermal_step();
                continue;
            }

            /* 周期性采集：使用 vTaskDelayUntil 保证等间隔 */
            vTaskDelayUntil(&last_wake_ticks, period_ticks);
            redpic1_thermal_step();
            watchdog_service_mark_progress(WATCHDOG_PROGRESS_THERMAL);
        }
        else
        {
            /* ---- 空闲态：低功耗等待 ---- */
            active_running = 0U;
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_THERMAL_IDLE_MS));
            watchdog_service_mark_progress(WATCHDOG_PROGRESS_THERMAL);
        }
    }
}

/* --------------------------------------------------------------------------
 *  20.6  电源管理与看门狗喂狗任务
 * ----------------------------------------------------------------------- */

/**
 * @brief  电源管理与看门狗喂狗任务入口
 * @note   本任务具有最高优先级，职责包括：
 *         1. 驱动电源状态机步进（休眠/唤醒转换）
 *         2. 收集各子任务心跳，判断健康状态
 *         3. 按窗口周期喂狗，防止看门狗复位
 *         4. 采集 UART 错误统计与性能基准数据
 */
static void power_wdg_task(void *pvParameters)
{
    EventBits_t   event_bits              = 0U;
    EventBits_t   watchdog_hb_latched     = 0U;
    uint32_t      uart_error_flags        = 0U;
    uint32_t      watchdog_window_start_ms = 0U;
    uint32_t      watchdog_window_ms       = 0U;
    power_state_t owned_state             = POWER_STATE_ACTIVE_UI;

    (void)pvParameters;

    /* 初始化：获取当前电源状态并设置屏幕事件位 */
    owned_state = power_manager_get_state();
    app_runtime_set_screen_off_event(owned_state);

    /* 获取看门狗喂狗窗口周期 */
    watchdog_window_ms = watchdog_service_get_feed_interval_ms();
    if (watchdog_window_ms == 0U)
    {
        watchdog_window_ms = 1000UL;  /* 默认 1 秒 */
    }
    watchdog_window_start_ms = power_manager_get_tick_ms();
    watchdog_service_begin_cycle();

    while (1)
    {
        /* 等待通知或超时 */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_POWER_LOOP_MS));

        /* 第一次电源步进 */
        owned_state = app_power_step_and_handle(owned_state);

        /* 锁存本轮心跳位（OR 累积，窗口末尾统一清除） */
        event_bits = (s_runtime_events != 0)
                         ? xEventGroupGetBits(s_runtime_events)
                         : 0U;
        watchdog_hb_latched |= (event_bits & APP_EG_HB_ALL);

        /* 标记主循环进度 */
        watchdog_service_mark_progress(WATCHDOG_PROGRESS_MAIN_LOOP);

        /* --- UI 进度标记 --- */
        if ((watchdog_hb_latched & APP_EG_HB_UI) != 0U ||
            (event_bits & APP_EG_BIT_SCREEN_OFF) != 0U ||
            (event_bits & (APP_EG_BIT_OTA_BUSY | APP_EG_BIT_WIFI_BUSY)) != 0U)
        {
            watchdog_service_mark_progress(WATCHDOG_PROGRESS_UI);
        }

        /* --- 服务进度标记 --- */
        if ((watchdog_hb_latched & APP_EG_HB_SERVICE) != 0U ||
            (event_bits & (APP_EG_BIT_OTA_BUSY | APP_EG_BIT_WIFI_BUSY)) != 0U)
        {
            watchdog_service_mark_progress(WATCHDOG_PROGRESS_OTA |
                                           WATCHDOG_PROGRESS_ESP_HOST |
                                           WATCHDOG_PROGRESS_BATTERY);
        }
        else if ((event_bits & APP_EG_BIT_SCREEN_OFF) != 0U)
        {
            /* 屏幕关闭空闲态：服务任务心跳可能因 STOP 唤醒时序而滑动，
             * 此处主动标记进度，避免看门狗误判为服务卡死。 */
            watchdog_service_mark_progress(WATCHDOG_PROGRESS_OTA |
                                           WATCHDOG_PROGRESS_ESP_HOST |
                                           WATCHDOG_PROGRESS_BATTERY);
        }

        /* 标记电源进度并上报按键健康状态 */
        watchdog_service_mark_progress(WATCHDOG_PROGRESS_POWER);
        watchdog_service_report_key_health(KEY_EXTI_IsHealthy());

        /* 采集并上报 UART 错误统计 */
        uart_error_flags = uart_rx_ring_take_error_flags();
        app_perf_baseline_record_uart_errors(uart_error_flags);
        watchdog_service_report_uart_errors(uart_error_flags);

        /* 执行看门狗步进（检查是否需要喂狗） */
        watchdog_service_step();

        /* 低功耗运行时步进 */
        low_power_runtime_step();

        /* 第二次电源步进（处理低功耗步进可能引发的状态变化） */
        owned_state = app_power_step_and_handle(owned_state);

        /* 更新性能基准快照 */
        event_bits = (s_runtime_events != 0)
                         ? xEventGroupGetBits(s_runtime_events)
                         : 0U;
        app_perf_baseline_set_watchdog_snapshot(
            watchdog_service_get_missing_progress_mask(),
            watchdog_service_get_last_fault_flags());
        app_perf_baseline_set_runtime_state(
            owned_state,
            clock_profile_get(),
            ((event_bits & APP_EG_BIT_THERMAL_ACTIVE) != 0U) ? 1U : 0U,
            (owned_state == POWER_STATE_SCREEN_OFF_IDLE) ? 1U : 0U);
        app_perf_baseline_refresh_task_stacks(s_input_task_handle,
                                              s_service_task_handle,
                                              s_ui_task_handle,
                                              s_display_task_handle,
                                              s_thermal_task_handle,
                                              s_power_wdg_task_handle);

        /* 清除所有心跳位并置位电源任务自身心跳 */
        if (s_runtime_events != 0)
        {
            (void)xEventGroupClearBits(s_runtime_events, APP_EG_HB_ALL);
            (void)xEventGroupSetBits(s_runtime_events, APP_EG_HB_POWER);
        }

        /* 看门狗窗口到期：开始新周期 */
        if ((power_manager_get_tick_ms() - watchdog_window_start_ms) >= watchdog_window_ms)
        {
            watchdog_window_start_ms = power_manager_get_tick_ms();
            watchdog_hb_latched      = 0U;
            watchdog_service_begin_cycle();
        }
    }
}

/* =========================================================================
 *  21. 系统初始化与启动入口
 * ======================================================================= */

/**
 * @brief  初始化 RTOS 运行时依赖和跨任务共享对象
 * @note   按以下顺序完成初始化：
 *         1. 底层硬件与基础服务（电源、RTC、看门狗、UART 等）
 *         2. 应用层服务（设置、存储、电池、低功耗等）
 *         3. 外设驱动（LCD DMA、按键、MLX90640 I2C 等）
 *         4. RTOS 同步对象（队列、互斥锁、事件组）
 *         5. 校验所有对象创建成功，任一失败则进入 panic
 */
void app_rtos_runtime_init(void)
{
    uint8_t display_runtime_ok = 0U;
    uint8_t mlx_i2c_runtime_ok = 0U;
    uint8_t service_bus_ok     = 0U;

    /* ---- 1. 底层硬件与基础服务初始化 ---- */
    power_manager_init();
    rtc_lp_service_init();
    watchdog_service_init(1000UL);
    uart_init(115200);
    ota_service_init();
    settings_service_init();
    storage_service_init();
    app_force_manual_wifi_boot();
    battery_monitor_init();
    low_power_runtime_init();

    /* ---- 2. 服务总线与同步服务注册 ---- */
    app_service_bus_reset();
    app_service_bus_register_executor(app_service_execute_command);
    esp_sync_service_register_settings_copy(app_rtos_settings_copy);

    /* 检查低功耗早期启动是否成功 */
    if (low_power_runtime_handle_early_boot() != 0U)
    {
        app_panic_loop();
    }

    /* ---- 3. 外设驱动初始化 ---- */
    /* LCD_Init(); */  /* LCD 初始化已由 display_runtime 管理 */
    MYDMA_Config();
    KEY_Init();
    KEY_EXTI_Init();
    clock_profile_service_init();
    app_perf_baseline_init();
    mlx_i2c_runtime_ok = MLX90640_I2CRuntimeInit();

    /* ---- 4. 热成像模块初始化 ---- */
    redpic1_thermal_init();
    redpic1_thermal_suspend();
    power_manager_notify_activity();
    app_apply_persisted_settings();

    /* ---- 5. 创建 RTOS 同步对象 ---- */
    s_key_event_queue = xQueueCreate(Q_KEY_EVENT_LEN, sizeof(app_key_event_t));
    s_ui_msg_queue    = xQueueCreate(Q_UI_MSG_LEN, sizeof(app_service_rsp_t));
    s_settings_mutex  = xSemaphoreCreateRecursiveMutex();
    s_runtime_events  = xEventGroupCreate();

    /* ---- 6. 初始化服务总线与显示运行时 ---- */
    service_bus_ok     = app_service_bus_init(s_ui_msg_queue, Q_SERVICE_CMD_LEN);
    display_runtime_ok = app_display_runtime_init();
    redpic1_thermal_bind_display_runtime();

    /* ---- 7. 校验所有关键对象创建成功 ---- */
    if (s_key_event_queue == 0    ||
        s_ui_msg_queue    == 0    ||
        s_settings_mutex  == 0    ||
        service_bus_ok    == 0U   ||
        mlx_i2c_runtime_ok == 0U ||
        display_runtime_ok == 0U  ||
        s_runtime_events  == 0)
    {
        app_panic_loop();
    }
}

/**
 * @brief  创建启动任务并进入 FreeRTOS 调度器
 * @note   调用后不会返回，除非调度器被异常终止。
 */
void app_rtos_runtime_start(void)
{
    app_create_runtime_task(start_task,
                            "start_task",
                            (uint16_t)START_STK_SIZE,
                            (UBaseType_t)START_TASK_PRIO,
                            &s_start_task_handle);

    vTaskStartScheduler();
}

/**
 * @brief  APP 主入口函数
 * @note   完成底层硬件初始化后进入 FreeRTOS 运行时，
 *         正常情况下此函数不会返回。
 */
void redpic1_app_main(void)
{
    /* 系统时钟与中断初始化 */
    delay_init(168);
    SystemInit();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    __enable_irq();

    /* 初始化 RTOS 运行时环境 */
    app_rtos_runtime_init();

    /* 启动 FreeRTOS 调度器（不会返回） */
    app_rtos_runtime_start();

    /* 不可达代码：防御性死循环 */
    for (;;)
    {
    }
}
