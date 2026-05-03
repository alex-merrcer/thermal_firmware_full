/**
 * @file    app_service_bus.c
 * @brief   应用层服务总线模块
 * @note    本模块提供跨任务的服务命令提交与执行框架，支持：
 *          - 同步提交（阻塞等待执行结果）
 *          - 异步提交（投递到队列后立即返回）
 *          - 延迟重试（队列满或同 ID 命令执行中时暂存，后续自动补发）
 *          - 执行器注册（由上层注入实际命令处理函数）
 *
 * @par 线程模型
 *      页面层任务通过 app_service_submit / app_service_submit_async 提交命令，
 *      服务任务通过 app_service_bus_process 消费队列并回调执行器，
 *      UI 响应通过内部消息队列投递给 UI 任务。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "app_service_bus.h"

#include <string.h>

#include "app_perf_baseline.h"
#include "semphr.h"

/* =========================================================================
 *  2. 宏定义
 * ======================================================================= */

/** @defgroup SERVICE_BUS_CONST  服务总线内部常量
 *  @{ */
#define APP_SERVICE_DEFAULT_TIMEOUT_MS  1500UL                                      /**< 同步提交默认超时（毫秒） */
#define APP_SERVICE_CMD_COUNT           ((uint8_t)APP_SERVICE_CMD_OTA_QUERY_LATEST + 1U) /**< 有效命令 ID 总数     */
/** @} */

/* =========================================================================
 *  3. 内部数据类型定义
 * ======================================================================= */

/**
 * @brief 服务命令请求封装
 * @note  投递到服务命令队列的基本单元，包含命令体、同步返回缓冲区指针
 *        以及同步等待标志。
 */
typedef struct
{
    app_service_cmd_t   cmd;            /**< 实际服务命令内容                    */
    app_service_rsp_t  *sync_rsp;       /**< 同步调用时的返回缓冲区，异步为 NULL */
    uint8_t             sync_wait;      /**< 1 表示调用方正在等待完成信号量      */
} app_service_req_t;

/* =========================================================================
 *  4. 模块级静态变量
 * ======================================================================= */

/* ---- 任务句柄 ---- */
static TaskHandle_t         s_service_task_handle   = 0;    /**< 服务任务句柄              */
static TaskHandle_t         s_ui_task_handle        = 0;    /**< UI 任务句柄               */

/* ---- 消息队列 ---- */
static QueueHandle_t        s_service_cmd_queue     = 0;    /**< 服务命令队列              */
static QueueHandle_t        s_ui_msg_queue          = 0;    /**< UI 响应消息队列（外部传入） */

/* ---- 同步原语 ---- */
static SemaphoreHandle_t    s_service_done_sem      = 0;    /**< 同步命令完成信号量        */
static SemaphoreHandle_t    s_service_sync_mutex    = 0;    /**< 同步提交互斥锁            */

/* ---- 同步响应缓冲区（C1 修复：替代栈指针，避免超时后悬挂指针） ---- */
static app_service_rsp_t    s_sync_rsp_buffer;              /**< 同步响应静态缓冲区        */
static volatile uint8_t     s_sync_rsp_valid       = 0U;    /**< 缓冲区有效性标志          */

/* ---- 执行器回调 ---- */
static app_service_execute_fn_t s_execute_fn        = 0;    /**< 命令执行器函数指针        */

/* ---- 延迟重试机制 ---- */
static app_service_cmd_t    s_service_deferred_cmd[APP_SERVICE_CMD_COUNT];     /**< 各命令 ID 的延迟缓存命令 */
static uint8_t              s_service_deferred_valid[APP_SERVICE_CMD_COUNT];   /**< 延迟缓存是否有效         */

/* ---- 命令执行中标志 ---- */
static uint8_t              s_service_pending[APP_SERVICE_CMD_COUNT];          /**< 各命令 ID 是否正在执行中 */

/* =========================================================================
 *  5. 内部函数前向声明
 * ======================================================================= */

static uint8_t  app_service_bus_is_scheduler_running    (void);
static uint8_t  app_service_cmd_id_is_valid             (app_service_cmd_id_t cmd_id);
static uint8_t  app_service_cmd_index                   (app_service_cmd_id_t cmd_id);
static void     app_service_pending_set                 (app_service_cmd_id_t cmd_id,
                                                          uint8_t value);
static uint8_t  app_service_pending_get                 (app_service_cmd_id_t cmd_id);
static void     app_service_store_deferred              (const app_service_cmd_t *cmd);
static uint8_t  app_service_try_enqueue_deferred_for_id (app_service_cmd_id_t cmd_id);
static void     app_service_bus_notify_ui_task          (void);

/* =========================================================================
 *  6. 内部工具函数实现
 * ======================================================================= */

/**
 * @brief  判断 FreeRTOS 调度器是否正在运行
 * @retval 1 — 调度器运行中；0 — 尚未启动或已挂起
 */
static uint8_t app_service_bus_is_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

/**
 * @brief  校验命令 ID 是否合法
 * @param  cmd_id — 待校验的命令 ID
 * @retval 1 — 合法；0 — 超出有效范围
 */
static uint8_t app_service_cmd_id_is_valid(app_service_cmd_id_t cmd_id)
{
    return (cmd_id > APP_SERVICE_CMD_NONE &&
            (uint8_t)cmd_id < APP_SERVICE_CMD_COUNT) ? 1U : 0U;
}

/**
 * @brief  将命令 ID 转换为数组索引
 * @param  cmd_id — 命令 ID
 * @return uint8_t — 对应的数组下标
 */
static uint8_t app_service_cmd_index(app_service_cmd_id_t cmd_id)
{
    return (uint8_t)cmd_id;
}

/**
 * @brief  设置指定命令的"执行中"标志
 * @note   使用临界区保护，确保多任务环境下对 pending 数组的原子访问。
 * @param  cmd_id — 命令 ID
 * @param  value  — 1=标记为执行中，0=清除标记
 */
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

/**
 * @brief  查询指定命令是否正在执行中
 * @param  cmd_id — 命令 ID
 * @retval 1 — 正在执行中；0 — 空闲或 ID 无效
 */
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

/**
 * @brief  将命令暂存到延迟重试缓冲区
 * @note   当命令队列满或同 ID 命令正在执行中时调用，
 *         待条件满足后由 app_service_try_enqueue_deferred_for_id 自动补发。
 * @param  cmd — 待暂存的命令指针
 */
static void app_service_store_deferred(const app_service_cmd_t *cmd)
{
    uint8_t index = 0U;

    if (cmd == 0 || app_service_cmd_id_is_valid(cmd->cmd_id) == 0U)
    {
        return;
    }

    index = app_service_cmd_index(cmd->cmd_id);

    taskENTER_CRITICAL();
    s_service_deferred_cmd[index]   = *cmd;
    s_service_deferred_valid[index] = 1U;
    taskEXIT_CRITICAL();
}

/**
 * @brief  尝试将指定命令 ID 的延迟缓存补发到队列
 * @note   仅当该命令当前未在执行中 且 延迟缓存有效时才补发。
 *         补发成功返回 1，队列满则将命令重新暂存并返回 0。
 * @param  cmd_id — 命令 ID
 * @retval 1 — 补发成功；0 — 无需补发或补发失败
 */
static uint8_t app_service_try_enqueue_deferred_for_id(app_service_cmd_id_t cmd_id)
{
    uint8_t           index        = 0U;
    uint8_t           has_deferred = 0U;
    app_service_cmd_t deferred_cmd;
    app_service_req_t req;

    if (s_service_cmd_queue == 0 || app_service_cmd_id_is_valid(cmd_id) == 0U)
    {
        return 0U;
    }

    index = app_service_cmd_index(cmd_id);

    /* 临界区内检查并取出延迟缓存 */
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

    /* 构造异步请求并投递到队列 */
    memset(&req, 0, sizeof(req));
    req.cmd       = deferred_cmd;
    req.sync_wait = 0U;
    req.sync_rsp  = 0;

    if (xQueueSendToBack(s_service_cmd_queue, &req, 0U) == pdPASS)
    {
        app_service_pending_set(deferred_cmd.cmd_id, 1U);
        return 1U;
    }

    /* 队列满：记录失败并将命令重新暂存，等待下次重试 */
    app_perf_baseline_record_service_queue_fail();
    app_service_store_deferred(&deferred_cmd);
    return 0U;
}

/**
 * @brief  通知 UI 任务有新的服务响应待处理
 */
static void app_service_bus_notify_ui_task(void)
{
    if (s_ui_task_handle != 0)
    {
        app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_UI);
        xTaskNotifyGive(s_ui_task_handle);
    }
}

/* =========================================================================
 *  7. 公共接口实现
 * ======================================================================= */

/**
 * @brief  复位服务总线所有内部状态
 * @note   在系统初始化早期调用，将所有句柄、队列、缓存清零。
 */
void app_service_bus_reset(void)
{
    s_service_task_handle = 0;
    s_ui_task_handle      = 0;
    s_service_cmd_queue   = 0;
    s_ui_msg_queue        = 0;
    s_service_done_sem    = 0;
    s_service_sync_mutex  = 0;
    s_execute_fn          = 0;
    s_sync_rsp_valid      = 0U;
    memset(&s_sync_rsp_buffer, 0, sizeof(s_sync_rsp_buffer));

    memset(s_service_deferred_cmd,   0, sizeof(s_service_deferred_cmd));
    memset(s_service_deferred_valid, 0, sizeof(s_service_deferred_valid));
    memset(s_service_pending,        0, sizeof(s_service_pending));
}

/**
 * @brief  注册命令执行器回调函数
 * @note   执行器负责根据命令 ID 分发到具体的业务处理逻辑。
 *         由 app_rtos_runtime_init 在初始化阶段注册一次。
 * @param  execute_fn — 执行器函数指针
 */
void app_service_bus_register_executor(app_service_execute_fn_t execute_fn)
{
    s_execute_fn = execute_fn;
}

/**
 * @brief  初始化服务总线
 * @note   创建服务命令队列、完成信号量和同步互斥锁。
 *         UI 消息队列由外部创建并传入。
 * @param  ui_msg_queue     — 外部 UI 响息队列句柄
 * @param  service_queue_len — 服务命令队列深度
 * @retval 1 — 初始化成功；0 — 参数无效或资源创建失败
 */
uint8_t app_service_bus_init(QueueHandle_t ui_msg_queue, uint32_t service_queue_len)
{
    if (ui_msg_queue == 0 || service_queue_len == 0U)
    {
        return 0U;
    }

    s_ui_msg_queue      = ui_msg_queue;
    s_service_cmd_queue = xQueueCreate((UBaseType_t)service_queue_len,
                                       sizeof(app_service_req_t));
    s_service_done_sem  = xSemaphoreCreateBinary();
    s_service_sync_mutex = xSemaphoreCreateMutex();

    if (s_service_cmd_queue  == 0 ||
        s_service_done_sem   == 0 ||
        s_service_sync_mutex == 0)
    {
        if (s_service_cmd_queue != 0)  { vQueueDelete(s_service_cmd_queue);  s_service_cmd_queue  = 0; }
        if (s_service_done_sem != 0)   { vSemaphoreDelete(s_service_done_sem);   s_service_done_sem   = 0; }
        if (s_service_sync_mutex != 0) { vSemaphoreDelete(s_service_sync_mutex); s_service_sync_mutex = 0; }
        return 0U;
    }

    return 1U;
}

/**
 * @brief  设置服务任务句柄
 * @note   由 start_task 在创建服务任务后调用。
 * @param  service_task_handle — 服务任务句柄
 */
void app_service_bus_set_service_task_handle(TaskHandle_t service_task_handle)
{
    s_service_task_handle = service_task_handle;
}

/**
 * @brief  设置 UI 任务句柄
 * @note   由 start_task 在创建 UI 任务后调用。
 * @param  ui_task_handle — UI 任务句柄
 */
void app_service_bus_set_ui_task_handle(TaskHandle_t ui_task_handle)
{
    s_ui_task_handle = ui_task_handle;
}

/**
 * @brief  查询服务命令队列中是否有待处理命令
 * @retval 1 — 队列中有待处理命令；0 — 队列为空或未初始化
 */
uint8_t app_service_bus_has_pending_work(void)
{
    return (s_service_cmd_queue != 0 &&
            uxQueueMessagesWaiting(s_service_cmd_queue) != 0U) ? 1U : 0U;
}

/**
 * @brief  通知服务任务有新命令待处理
 * @note   通过 TaskNotify 唤醒服务任务。
 */
void app_service_bus_notify_service_task(void)
{
    if (s_service_task_handle != 0)
    {
        app_perf_baseline_record_task_notify(APP_PERF_NOTIFY_SERVICE);
        xTaskNotifyGive(s_service_task_handle);
    }
}

/**
 * @brief  服务任务调用：消费命令队列并执行
 * @note   非阻塞地取出队列中所有待处理命令，逐条执行：
 *         1. 回调执行器处理命令
 *         2. 若为同步调用，将结果拷贝到调用方缓冲区并释放完成信号量
 *         3. 将响应投递到 UI 消息队列（队列满时丢弃最旧消息）
 *         4. 清除该命令的"执行中"标志，并尝试补发同 ID 的延迟缓存
 */
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
        /* 初始化响应结构体 */
        memset(&service_rsp, 0, sizeof(service_rsp));
        service_rsp.cmd_id = service_req.cmd.cmd_id;

        /* 回调执行器处理命令 */
        if (s_execute_fn != 0)
        {
            (void)s_execute_fn(&service_req.cmd, &service_rsp);
        }

        /* 同步调用：将结果拷贝到调用方缓冲区并释放信号量 */
        if (service_req.sync_wait != 0U && service_req.sync_rsp != 0 &&
            s_sync_rsp_valid != 0U)
        {
            *(service_req.sync_rsp) = service_rsp;
            (void)xSemaphoreGive(s_service_done_sem);
        }

        /* 将响应投递到 UI 消息队列 */
        if (s_ui_msg_queue != 0)
        {
            if (xQueueSendToBack(s_ui_msg_queue, &service_rsp, 0U) != pdPASS)
            {
                /* 队列满：丢弃最旧消息后重试，确保最新响应不丢失 */
                app_service_rsp_t dropped_rsp;

                app_perf_baseline_record_ui_msg_drop();
                (void)xQueueReceive(s_ui_msg_queue, &dropped_rsp, 0U);
                (void)xQueueSendToBack(s_ui_msg_queue, &service_rsp, 0U);
            }
            app_service_bus_notify_ui_task();
        }

        /* 清除执行中标志并尝试补发同 ID 的延迟缓存 */
        app_service_pending_set(service_req.cmd.cmd_id, 0U);
        (void)app_service_try_enqueue_deferred_for_id(service_req.cmd.cmd_id);
    }
}

/**
 * @brief  尝试补发所有命令 ID 的延迟缓存
 * @note   遍历全部有效命令 ID，逐个尝试补发。
 *         由服务任务在每个主循环末尾调用。
 * @retval 1 — 至少有一条延迟命令补发成功；0 — 无延迟命令或全部补发失败
 */
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

/**
 * @brief  同步提交服务命令
 * @note   调用方阻塞等待服务任务执行完成后返回结果。
 *         特殊场景处理：
 *         - 若当前在服务任务上下文中，直接调用执行器避免死锁
 *         - 若调度器未运行，直接调用执行器（初始化阶段兼容）
 *         - 使用互斥锁保证同一时刻只有一条同步命令在等待
 * @param  cmd        — 输入：命令结构体指针
 * @param  rsp        — 输出：响应结构体指针（可为 NULL，内部使用栈变量兜底）
 * @param  timeout_ms — 超时时间（毫秒），为 0 时使用默认值 1500ms
 * @retval 1 — 命令执行成功；0 — 参数无效、超时或队列满
 */
uint8_t app_service_submit(const app_service_cmd_t *cmd,
                           app_service_rsp_t *rsp,
                           uint32_t timeout_ms)
{
    app_service_req_t req;
    TickType_t        wait_ticks = 0U;
    app_service_rsp_t local_rsp;

    if (cmd == 0)
    {
        return 0U;
    }

    /* 超时时间为 0 时使用默认值 */
    if (timeout_ms == 0U)
    {
        timeout_ms = APP_SERVICE_DEFAULT_TIMEOUT_MS;
    }

    /* 特殊情况：在服务任务上下文中直接调用，避免同步等待导致死锁 */
    if ((app_service_bus_is_scheduler_running() != 0U) &&
        (xTaskGetCurrentTaskHandle() == s_service_task_handle))
    {
        if (rsp == 0)
        {
            rsp = &local_rsp;
        }
        return (s_execute_fn != 0) ? s_execute_fn(cmd, rsp) : 0U;
    }

    /* 特殊情况：调度器未运行或同步原语未创建，直接调用执行器 */
    if (app_service_bus_is_scheduler_running() == 0U ||
        s_service_cmd_queue  == 0 ||
        s_service_done_sem   == 0 ||
        s_service_sync_mutex == 0)
    {
        if (rsp == 0)
        {
            rsp = &local_rsp;
        }
        return (s_execute_fn != 0) ? s_execute_fn(cmd, rsp) : 0U;
    }

    /* 构造同步请求 */
    memset(&req, 0, sizeof(req));
    req.cmd       = *cmd;
    req.sync_rsp  = &s_sync_rsp_buffer;
    req.sync_wait = 1U;
    wait_ticks    = pdMS_TO_TICKS(timeout_ms);

    /* 获取同步互斥锁（同一时刻只允许一条同步命令） */
    if (xSemaphoreTake(s_service_sync_mutex, wait_ticks) != pdPASS)
    {
        return 0U;
    }

    /* 清除可能残留的完成信号量（防止上次超时遗留的信号干扰） */
    (void)xSemaphoreTake(s_service_done_sem, 0U);

    /* 标记同步响应缓冲区有效（C1 修复） */
    s_sync_rsp_valid = 1U;

    /* 标记命令为执行中并投递到队列 */
    app_service_pending_set(req.cmd.cmd_id, 1U);
    if (xQueueSendToBack(s_service_cmd_queue, &req, wait_ticks) != pdPASS)
    {
        /* 队列满：记录失败并释放互斥锁 */
        app_perf_baseline_record_service_queue_fail();
        app_service_pending_set(req.cmd.cmd_id, 0U);
        (void)xSemaphoreGive(s_service_sync_mutex);
        return 0U;
    }

    /* 通知服务任务处理 */
    app_service_bus_notify_service_task();

    /* 阻塞等待服务任务执行完成 */
    if (xSemaphoreTake(s_service_done_sem, wait_ticks) != pdPASS)
    {
        /* 超时：废弃同步响应缓冲区，清除执行中标志并释放互斥锁（C1 修复） */
        s_sync_rsp_valid = 0U;
        app_service_pending_set(req.cmd.cmd_id, 0U);
        (void)xSemaphoreGive(s_service_sync_mutex);
        return 0U;
    }

    /* 释放互斥锁，将结果从静态缓冲区拷贝到调用方并返回（C1 修复） */
    (void)xSemaphoreGive(s_service_sync_mutex);
    if (rsp != 0)
    {
        *rsp = s_sync_rsp_buffer;
    }
    return s_sync_rsp_buffer.ok;
}

/**
 * @brief  异步提交服务命令
 * @note   将命令投递到队列后立即返回，不等待执行结果。
 *         特殊场景处理：
 *         - 若调度器未运行，直接调用执行器（初始化阶段兼容）
 *         - 若同 ID 命令正在执行中，将命令暂存到延迟缓冲区
 *         - 若队列满，将命令暂存到延迟缓冲区
 * @param  cmd — 输入：命令结构体指针
 * @retval 1 — 投递成功或已暂存；0 — 参数无效或直接执行失败
 */
uint8_t app_service_submit_async(const app_service_cmd_t *cmd)
{
    app_service_req_t req;
    app_service_rsp_t local_rsp;

    if (cmd == 0 || app_service_cmd_id_is_valid(cmd->cmd_id) == 0U)
    {
        return 0U;
    }

    /* 特殊情况：调度器未运行，直接调用执行器 */
    if (app_service_bus_is_scheduler_running() == 0U || s_service_cmd_queue == 0)
    {
        return (s_execute_fn != 0) ? s_execute_fn(cmd, &local_rsp) : 0U;
    }

    /* 同 ID 命令正在执行中：暂存到延迟缓冲区，待执行完成后自动补发 */
    if (app_service_pending_get(cmd->cmd_id) != 0U)
    {
        app_service_store_deferred(cmd);
        return 1U;
    }

    /* 构造异步请求并投递到队列 */
    memset(&req, 0, sizeof(req));
    req.cmd       = *cmd;
    req.sync_rsp  = 0;
    req.sync_wait = 0U;

    if (xQueueSendToBack(s_service_cmd_queue, &req, 0U) == pdPASS)
    {
        uint8_t index = app_service_cmd_index(cmd->cmd_id);

        /* 清除该命令 ID 的延迟缓存（新命令已入队，旧缓存不再需要） */
        taskENTER_CRITICAL();
        s_service_deferred_valid[index] = 0U;
        taskEXIT_CRITICAL();

        app_service_pending_set(cmd->cmd_id, 1U);
        app_service_bus_notify_service_task();
        return 1U;
    }

    /* 队列满：记录失败并将命令暂存到延迟缓冲区 */
    app_perf_baseline_record_service_queue_fail();
    app_service_store_deferred(cmd);
    return 1U;
}
