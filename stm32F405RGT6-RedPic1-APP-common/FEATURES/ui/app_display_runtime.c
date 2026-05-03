/**
 * @file    app_display_runtime.c
 * @brief   显示运行时模块 —— FreeRTOS QueueSet 驱动的显示命令处理
 * @note    本模块负责管理 LCD 显示资源的并发访问，通过 FreeRTOS QueueSet
 *          机制统一处理同步命令和异步热成像帧呈现请求。
 *
 * @par 命令类型
 *      - SLEEP:           LCD 进入低功耗休眠
 *      - WAKE:            LCD 从休眠中唤醒
 *      - THERMAL_PRESENT: 同步热成像灰度帧送显
 *      - UI_RENDER:       同步 UI 页面渲染回调
 *
 * @par 异步热成像送显
 *      支持异步提交热成像帧，通过 claim/done 回调机制实现：
 *      1. 提交方通过 request_thermal_present_async() 投递帧
 *      2. 显示任务通过 claim 回调获取最新灰度帧指针
 *      3. 送显完成后通过 done 回调通知提交方
 *      4. 新帧替换旧帧时，旧帧收到 CANCELLED 通知
 *
 * @par 同步命令提交
 *      外部任务通过 submit_sync() 发送命令并阻塞等待完成。
 *      若当前任务即为显示任务，则直接执行（避免死锁）。
 *      使用 done_sem 信号量实现跨任务同步。
 *
 * @par 显示锁
 *      提供递归互斥锁（Recursive Mutex），保护 LCD DMA 操作的原子性。
 *      支持在调度器未启动时的安全降级。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

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
#include "watchdog_service.h"

/* =========================================================================
 *  2. 内部宏定义
 * ======================================================================= */

/** 命令队列深度 */
#define APP_DISPLAY_CMD_QUEUE_LEN      8U

/** QueueSet 事件集长度（命令队列 + 热成像信号量） */
#define APP_DISPLAY_EVENT_SET_LEN      (APP_DISPLAY_CMD_QUEUE_LEN + 1U)

/** 同步命令等待超时（ms） */
#define APP_DISPLAY_SYNC_TIMEOUT_MS    1000UL

/* =========================================================================
 *  3. 内部类型定义
 * ======================================================================= */

/**
 * @brief 显示命令 ID 枚举
 */
typedef enum
{
    APP_DISPLAY_CMD_NONE = 0,           /**< 空命令               */
    APP_DISPLAY_CMD_THERMAL_PRESENT,    /**< 热成像帧同步送显     */
    APP_DISPLAY_CMD_SLEEP,              /**< LCD 休眠             */
    APP_DISPLAY_CMD_WAKE,               /**< LCD 唤醒             */
    APP_DISPLAY_CMD_UI_RENDER           /**< UI 页面渲染          */
} app_display_cmd_id_t;

/**
 * @brief 显示命令参数结构体
 */
typedef struct
{
    app_display_cmd_id_t cmd_id;        /**< 命令 ID              */
    uint8_t *gray_frame;                /**< 热成像灰度帧指针     */
    app_display_ui_render_fn_t render_fn; /**< UI 渲染回调函数    */
    uint8_t full_refresh;               /**< 是否整页强制刷新     */
} app_display_runtime_cmd_t;

/**
 * @brief 同步命令响应结构体
 */
typedef struct
{
    uint8_t ok;                         /**< 执行结果（1=成功）   */
} app_display_runtime_rsp_t;

/**
 * @brief 同步请求封装结构体
 * @note  包含命令、响应指针和同步等待标志
 */
typedef struct
{
    app_display_runtime_cmd_t cmd;      /**< 命令参数             */
    app_display_runtime_rsp_t *sync_rsp; /**< 同步响应指针        */
    uint8_t sync_wait;                  /**< 是否需要同步等待     */
} app_display_runtime_req_t;

/**
 * @brief 异步热成像帧待处理结构体
 */
typedef struct
{
    uint8_t pending;                    /**< 是否有待处理帧       */
    uint8_t *gray_frame;                /**< 灰度帧指针           */
    uintptr_t token;                    /**< 帧标识令牌           */
} app_display_runtime_async_thermal_t;

/* =========================================================================
 *  4. 模块级静态变量
 * ======================================================================= */

static SemaphoreHandle_t s_display_mutex       = 0;   /**< 递归互斥锁（保护 LCD 操作）  */
static QueueHandle_t     s_display_cmd_queue   = 0;   /**< 命令队列                     */
static QueueSetHandle_t  s_display_event_set   = 0;   /**< QueueSet 事件集              */
static SemaphoreHandle_t s_display_thermal_sem = 0;   /**< 热成像异步通知信号量         */
static SemaphoreHandle_t s_display_done_sem    = 0;   /**< 同步命令完成信号量           */
static SemaphoreHandle_t s_display_sync_mutex  = 0;   /**< 同步提交互斥锁（防止并发提交） */
static TaskHandle_t      s_display_task_handle = 0;   /**< 显示任务句柄                 */

static uint8_t s_display_awake = 1U;                  /**< LCD 唤醒状态标志             */

static app_display_runtime_async_thermal_t s_pending_thermal; /**< 待处理异步热成像帧  */

static app_display_thermal_done_fn_t   s_thermal_done_fn   = 0; /**< 送显完成回调       */
static app_display_thermal_claim_fn_t  s_thermal_claim_fn  = 0; /**< 帧获取回调         */

/* =========================================================================
 *  5. 内部函数实现 —— 调度器与临界区辅助
 * ======================================================================= */

/**
 * @brief  检查 FreeRTOS 调度器是否正在运行
 * @retval 1 — 运行中；0 — 未启动或已挂起
 */
static uint8_t app_display_runtime_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

/**
 * @brief  安全进入临界区（调度器未启动时跳过）
 */
static void app_display_runtime_enter_critical(void)
{
    if (app_display_runtime_scheduler_running() != 0U)
    {
        taskENTER_CRITICAL();
    }
}

/**
 * @brief  安全退出临界区（调度器未启动时跳过）
 */
static void app_display_runtime_exit_critical(void)
{
    if (app_display_runtime_scheduler_running() != 0U)
    {
        taskEXIT_CRITICAL();
    }
}

/**
 * @brief  检查显示任务所有者及资源是否就绪
 * @retval 1 — 就绪；0 — 未就绪
 */
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

/* =========================================================================
 *  6. 内部函数实现 —— 热成像送显通知
 * ======================================================================= */

/**
 * @brief  通知热成像送显完成（调用 done 回调）
 * @param  token  — 帧标识令牌
 * @param  status — 送显状态（OK / CANCELLED / ERROR）
 */
static void app_display_runtime_notify_thermal_done(uintptr_t token,
                                                    app_display_thermal_done_status_t status)
{
    app_display_thermal_done_fn_t done_fn = 0;

    /* 在临界区内读取回调指针，防止并发注册导致的撕裂 */
    app_display_runtime_enter_critical();
    done_fn = s_thermal_done_fn;
    app_display_runtime_exit_critical();

    if (done_fn != 0)
    {
        done_fn(token, status);
    }
}

/* =========================================================================
 *  7. 内部函数实现 —— 热成像帧送显（持有锁）
 * ======================================================================= */

/**
 * @brief  在持有显示锁的状态下执行热成像帧送显
 * @note   流程：获取 DMA 锁 → 调用插值送显 → 渲染运行时叠加层 → 释放 DMA 锁
 * @param  gray_frame — 灰度帧缓冲区（768 字节）
 * @retval 1 — 送显成功；0 — 帧无效或屏幕休眠
 */
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
        /* 送显成功后渲染底部叠加层（温度信息等） */
        redpic1_thermal_render_runtime_overlay();
    }
    power_manager_release_lock(POWER_LOCK_DISPLAY_DMA);

    return ok;
}

/* =========================================================================
 *  8. 内部函数实现 —— 命令执行核心
 * ======================================================================= */

/**
 * @brief  在持有显示锁的状态下执行命令
 * @param  cmd — 命令参数指针
 * @param  rsp — 响应输出指针（可选）
 */
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

/**
 * @brief  直接执行命令（加锁 → 执行 → 解锁）
 * @note   用于当前任务即为显示任务时的快速路径
 * @param  cmd — 命令参数指针
 * @return 执行结果
 */
static uint8_t app_display_runtime_execute_direct(const app_display_runtime_cmd_t *cmd)
{
    app_display_runtime_rsp_t rsp;

    memset(&rsp, 0, sizeof(rsp));
    app_display_runtime_lock();
    app_display_runtime_execute_locked(cmd, &rsp);
    app_display_runtime_unlock();
    return rsp.ok;
}

/* =========================================================================
 *  9. 内部函数实现 —— 同步命令提交
 * ======================================================================= */

/**
 * @brief  同步提交命令到显示任务并阻塞等待完成
 * @note   快速路径：若当前任务即为显示任务，直接执行（避免死锁）。
 *         正常路径：通过命令队列投递，等待 done_sem 信号量。
 * @param  cmd — 命令参数指针
 * @return 执行结果（1=成功，0=失败或超时）
 */
static uint8_t app_display_runtime_submit_sync(const app_display_runtime_cmd_t *cmd)
{
    app_display_runtime_req_t req;
    app_display_runtime_rsp_t rsp;
    TickType_t wait_ticks = pdMS_TO_TICKS(APP_DISPLAY_SYNC_TIMEOUT_MS);

    if (cmd == 0)
    {
        return 0U;
    }

    /* 快速路径：当前任务即为显示任务，直接执行避免死锁 */
    if (app_display_runtime_owner_ready() == 0U ||
        xTaskGetCurrentTaskHandle() == s_display_task_handle)
    {
        return app_display_runtime_execute_direct(cmd);
    }

    /* 获取同步互斥锁，防止多个任务同时提交 */
    if (xSemaphoreTake(s_display_sync_mutex, wait_ticks) != pdPASS)
    {
        return 0U;
    }

    /* 构建同步请求 */
    memset(&req, 0, sizeof(req));
    memset(&rsp, 0, sizeof(rsp));
    req.cmd = *cmd;
    req.sync_rsp = &rsp;
    req.sync_wait = 1U;

    /* 清除残留信号量，确保等待的是本次命令的完成通知 */
    (void)xSemaphoreTake(s_display_done_sem, 0U);

    /* 投递命令到队列 */
    if (xQueueSendToBack(s_display_cmd_queue, &req, wait_ticks) != pdPASS)
    {
        app_perf_baseline_record_display_queue_fail();
        (void)xSemaphoreGive(s_display_sync_mutex);
        return 0U;
    }

    /* 记录任务通知性能事件 */
    app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_DISPLAY);

    /* 阻塞等待显示任务处理完成 */
    if (xSemaphoreTake(s_display_done_sem, wait_ticks) != pdPASS)
    {
        (void)xSemaphoreGive(s_display_sync_mutex);
        return 0U;
    }

    (void)xSemaphoreGive(s_display_sync_mutex);
    return rsp.ok;
}

/* =========================================================================
 *  10. 内部函数实现 —— 异步热成像处理
 * ======================================================================= */

/**
 * @brief  原子地取走待处理的异步热成像帧
 * @param  pending — 输出：待处理帧信息
 * @retval 1 — 有待处理帧；0 — 无待处理帧
 */
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

/**
 * @brief  处理待处理的异步热成像帧
 * @note   流程：
 *         1. 取走待处理帧
 *         2. 通过 claim 回调获取最新灰度帧指针
 *         3. 执行送显
 *         4. 通过 done 回调通知结果
 * @retval 1 — 已处理（无论成功或取消）；0 — 无待处理帧或屏幕休眠
 */
static uint8_t app_display_runtime_process_pending_thermal(void)
{
    app_display_runtime_async_thermal_t pending;
    uint8_t *gray_frame = 0;
    uint8_t ok = 0U;

    /* 屏幕休眠时跳过处理 */
    if (s_display_awake == 0U)
    {
        return 0U;
    }

    if (app_display_runtime_take_pending_thermal(&pending) == 0U)
    {
        return 0U;
    }

    /* 通过 claim 回调获取最新帧指针（可能已过期） */
    if (s_thermal_claim_fn != 0)
    {
        if (s_thermal_claim_fn(pending.token, &gray_frame) == 0U || gray_frame == 0)
        {
            /* 帧已过期或被取消 */
            app_display_runtime_notify_thermal_done(pending.token,
                                                    APP_DISPLAY_THERMAL_DONE_CANCELLED);
            return 1U;
        }
    }
    else
    {
        /* 无 claim 回调时直接使用缓存的帧指针 */
        gray_frame = pending.gray_frame;
        if (gray_frame == 0)
        {
            app_display_runtime_notify_thermal_done(pending.token,
                                                    APP_DISPLAY_THERMAL_DONE_ERROR);
            return 1U;
        }
    }

    /* 执行送显 */
    app_display_runtime_lock();
    ok = app_display_runtime_present_thermal_locked(gray_frame);
    app_display_runtime_unlock();

    /* 通知送显结果 */
    app_display_runtime_notify_thermal_done(pending.token,
                                            (ok != 0U) ?
                                            APP_DISPLAY_THERMAL_DONE_OK :
                                            APP_DISPLAY_THERMAL_DONE_ERROR);
    return 1U;
}

/* =========================================================================
 *  11. 内部函数实现 —— 同步请求处理
 * ======================================================================= */

/**
 * @brief  处理从命令队列中取出的同步请求
 * @param  req — 同步请求指针
 */
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

    /* 若请求方在等待，回写结果并释放完成信号量 */
    if (req->sync_wait != 0U && req->sync_rsp != 0)
    {
        *(req->sync_rsp) = rsp;
        (void)xSemaphoreGive(s_display_done_sem);
    }
}

/* =========================================================================
 *  12. 公共接口实现 —— 回调注册
 * ======================================================================= */

/**
 * @brief  注册热成像送显完成回调
 * @param  done_fn — 回调函数指针
 */
void app_display_runtime_set_thermal_present_done_callback(app_display_thermal_done_fn_t done_fn)
{
    app_display_runtime_enter_critical();
    s_thermal_done_fn = done_fn;
    app_display_runtime_exit_critical();
}

/**
 * @brief  注册热成像帧获取（claim）回调
 * @param  claim_fn — 回调函数指针
 */
void app_display_runtime_set_thermal_present_claim_callback(app_display_thermal_claim_fn_t claim_fn)
{
    app_display_runtime_enter_critical();
    s_thermal_claim_fn = claim_fn;
    app_display_runtime_exit_critical();
}

/* =========================================================================
 *  13. 公共接口实现 —— 初始化与启动
 * ======================================================================= */

/**
 * @brief  初始化显示运行时模块
 * @note   创建所有 FreeRTOS 同步原语：
 *         - 递归互斥锁（显示操作保护）
 *         - 命令队列（同步命令通道）
 *         - QueueSet（命令队列 + 热成像信号量统一事件源）
 *         - 二值信号量（热成像通知、同步完成通知）
 *         - 互斥锁（同步提交保护）
 * @retval 1 — 初始化成功；0 — 资源创建失败
 */
uint8_t app_display_runtime_init(void)
{
    s_display_task_handle = 0;
    s_display_awake = 1U;
    memset(&s_pending_thermal, 0, sizeof(s_pending_thermal));
    s_thermal_done_fn = 0;
    s_thermal_claim_fn = 0;

    /* 创建同步原语 */
    s_display_mutex      = xSemaphoreCreateRecursiveMutex();
    s_display_cmd_queue  = xQueueCreate(APP_DISPLAY_CMD_QUEUE_LEN, sizeof(app_display_runtime_req_t));
    s_display_event_set  = xQueueCreateSet(APP_DISPLAY_EVENT_SET_LEN);
    s_display_thermal_sem = xSemaphoreCreateBinary();
    s_display_done_sem   = xSemaphoreCreateBinary();
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

    /* 将命令队列和热成像信号量加入 QueueSet */
    if (xQueueAddToSet(s_display_cmd_queue, s_display_event_set) != pdPASS ||
        xQueueAddToSet(s_display_thermal_sem, s_display_event_set) != pdPASS)
    {
        return 0U;
    }

    return 1U;
}

/**
 * @brief  启动显示运行时（绑定当前任务为显示任务）
 * @note   必须在显示任务的上下文中调用。
 * @retval 1 — 启动成功；0 — 前置条件不满足
 */
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

/* =========================================================================
 *  14. 公共接口实现 —— 主任务循环
 * ======================================================================= */

/**
 * @brief  显示运行时主任务入口
 * @note   循环流程：
 *         1. 通过 QueueSet 阻塞等待事件（命令队列或热成像信号量）
 *         2. 处理所有待取的同步命令
 *         3. 若屏幕休眠则取消所有异步热成像请求
 *         4. 处理待处理的异步热成像帧
 * @param  pvParameters — 未使用
 */
void app_display_runtime_task(void *pvParameters)
{
    (void)pvParameters;

    /* H9 修复：启动失败时删除任务，避免 CPU 死锁饥饿其他任务 */
    if (app_display_runtime_start() == 0U)
    {
        vTaskDelete(NULL);
        return;  /* 不可达，但保持防御性编程 */
    }

    /* 主事件循环 */
    while (1)
    {
        QueueSetMemberHandle_t activated = 0;
        app_display_runtime_req_t req;

        /* 阻塞等待 QueueSet 中任意成员有数据 */
        activated = xQueueSelectFromSet(s_display_event_set, portMAX_DELAY);

        /* 热成像信号量激活时清除信号 */
        if (activated == s_display_thermal_sem)
        {
            (void)xSemaphoreTake(s_display_thermal_sem, 0U);
        }

        /* 批量处理所有待取的同步命令 */
        while (xQueueReceive(s_display_cmd_queue, &req, 0U) == pdPASS)
        {
            app_display_runtime_process_sync_req(&req);
        }

        /* 屏幕休眠时取消所有异步热成像请求并跳过 */
        if (s_display_awake == 0U)
        {
            app_display_runtime_cancel_thermal_present_async();
            watchdog_service_mark_progress(WATCHDOG_PROGRESS_DISPLAY);
            continue;
        }

        /* 处理待处理的异步热成像帧 */
        (void)app_display_runtime_process_pending_thermal();

        /* 标记显示任务心跳（无论屏幕是否亮着） */
        watchdog_service_mark_progress(WATCHDOG_PROGRESS_DISPLAY);
    }
}

/* =========================================================================
 *  15. 公共接口实现 —— 显示锁
 * ======================================================================= */

/**
 * @brief  获取显示递归互斥锁
 * @note   支持递归加锁（同一任务可多次获取）。
 *         调度器未启动时跳过加锁。
 */
void app_display_runtime_lock(void)
{
    if (s_display_mutex != 0 && app_display_runtime_scheduler_running() != 0U)
    {
        (void)xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY);
    }
}

/**
 * @brief  释放显示递归互斥锁
 */
void app_display_runtime_unlock(void)
{
    if (s_display_mutex != 0 && app_display_runtime_scheduler_running() != 0U)
    {
        (void)xSemaphoreGiveRecursive(s_display_mutex);
    }
}

/* =========================================================================
 *  16. 公共接口实现 —— 休眠与唤醒
 * ======================================================================= */

/**
 * @brief  请求 LCD 进入休眠
 * @note   先取消异步热成像请求，再同步提交 SLEEP 命令。
 * @retval 1 — 休眠成功；0 — 命令提交失败
 */
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

/**
 * @brief  请求 LCD 从休眠中唤醒
 * @retval 1 — 唤醒成功；0 — 命令提交失败
 */
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

/* =========================================================================
 *  17. 公共接口实现 —— 热成像帧送显
 * ======================================================================= */

/**
 * @brief  同步送显热成像灰度帧
 * @note   阻塞当前任务直到送显完成。
 * @param  gray_frame — 灰度帧缓冲区（768 字节）
 * @retval 1 — 送显成功；0 — 失败
 */
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

/**
 * @brief  异步请求送显热成像帧
 * @note   非阻塞，帧将由显示任务在下一个循环中处理。
 *         若已有待处理帧且 token 不同，旧帧将收到 CANCELLED 通知。
 * @param  gray_frame — 灰度帧缓冲区
 * @param  token      — 帧标识令牌（用于回调匹配）
 * @retval 1 — 请求已提交；0 — 参数无效或屏幕休眠
 */
uint8_t app_display_runtime_request_thermal_present_async(uint8_t *gray_frame, uintptr_t token)
{
    uintptr_t replaced_token = 0U;
    uint8_t replaced = 0U;

    if (gray_frame == 0 || s_display_awake == 0U || s_display_thermal_sem == 0)
    {
        return 0U;
    }

    /* 原子地更新待处理帧，检查是否替换了旧帧 */
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

    /* 旧帧被替换时通知取消 */
    if (replaced != 0U)
    {
        app_display_runtime_notify_thermal_done(replaced_token,
                                                APP_DISPLAY_THERMAL_DONE_CANCELLED);
    }

    /* 释放信号量唤醒显示任务 */
    if (s_display_thermal_sem != 0)
    {
        if (xSemaphoreGive(s_display_thermal_sem) == pdPASS)
        {
            app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_DISPLAY);
        }
    }

    return 1U;
}

/**
 * @brief  取消所有待处理的异步热成像请求
 * @note   清除待处理帧状态，排空信号量，通知取消。
 */
void app_display_runtime_cancel_thermal_present_async(void)
{
    uintptr_t cancelled_token = 0U;
    uint8_t cancelled = 0U;

    /* 原子地清除待处理帧 */
    app_display_runtime_enter_critical();
    if (s_pending_thermal.pending != 0U)
    {
        cancelled_token = s_pending_thermal.token;
        memset(&s_pending_thermal, 0, sizeof(s_pending_thermal));
        cancelled = 1U;
    }
    app_display_runtime_exit_critical();

    /* 排空热成像信号量（防止下次误唤醒） */
    while (s_display_thermal_sem != 0 && xSemaphoreTake(s_display_thermal_sem, 0U) == pdPASS)
    {
    }

    /* 通知取消 */
    if (cancelled != 0U)
    {
        app_display_runtime_notify_thermal_done(cancelled_token,
                                                APP_DISPLAY_THERMAL_DONE_CANCELLED);
    }
}

/* =========================================================================
 *  18. 公共接口实现 —— UI 渲染与状态查询
 * ======================================================================= */

/**
 * @brief  同步请求 UI 页面渲染
 * @param  render_fn    — 渲染回调函数
 * @param  full_refresh — 是否整页强制刷新
 * @retval 1 — 渲染成功；0 — 失败
 */
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

/**
 * @brief  查询 LCD 是否处于唤醒状态
 * @retval 1 — 已唤醒；0 — 休眠中
 */
uint8_t app_display_runtime_is_awake(void)
{
    return s_display_awake;
}
