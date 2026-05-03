/**
 * @file    app_service_bus.h
 * @brief   应用层服务总线公共接口
 * @note    本模块提供跨任务的服务命令提交与执行框架。
 *          页面层（UI 任务）通过 app_service_submit / app_service_submit_async
 *          提交命令，服务任务通过 app_service_bus_process 消费队列并回调
 *          注册的执行器函数，执行结果通过 UI 消息队列回传给 UI 任务。
 *
 * @par 使用流程
 *      1. 系统初始化阶段调用 app_service_bus_reset() 清零状态
 *      2. 调用 app_service_bus_register_executor() 注册命令执行器
 *      3. 调用 app_service_bus_init() 创建内部队列与同步原语
 *      4. 创建任务后通过 set_service_task_handle / set_ui_task_handle 绑定句柄
 *      5. 运行时由页面层调用 submit / submit_async 提交命令
 *      6. 服务任务主循环中调用 app_service_bus_process() 消费并执行命令
 *      7. 服务任务主循环末尾调用 app_service_bus_try_enqueue_deferred_any()
 *         补发因队列满或同 ID 冲突而暂存的延迟命令
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef APP_SERVICE_BUS_H
#define APP_SERVICE_BUS_H

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

/* =========================================================================
 *  2. 服务命令 ID 枚举
 * ======================================================================= */

/**
 * @brief 服务命令 ID 枚举
 * @note  每个枚举值对应一种服务命令类型，由页面层提交、服务任务执行。
 *        枚举值从 1 开始递增排列，0 (APP_SERVICE_CMD_NONE) 保留为无效值。
 */
typedef enum
{
    APP_SERVICE_CMD_NONE = 0,               /**< 无效命令（保留）                  */

    /* --- ESP 主机控制类命令 --- */
    APP_SERVICE_CMD_ESP_REFRESH_STATUS,     /**< 刷新 ESP 主机状态                */
    APP_SERVICE_CMD_SET_WIFI,               /**< 设置 WiFi 开关及参数             */
    APP_SERVICE_CMD_SET_BLE,                /**< 设置 BLE 开关                    */
    APP_SERVICE_CMD_SET_MQTT,               /**< 设置 MQTT 开关                   */
    APP_SERVICE_CMD_SET_DEBUG_SCREEN,       /**< 设置调试屏幕开关                 */
    APP_SERVICE_CMD_SET_REMOTE_KEYS,        /**< 设置远程按键开关                 */
    APP_SERVICE_CMD_SET_POWER_POLICY,       /**< 设置电源策略                     */
    APP_SERVICE_CMD_SET_HOST_STATE,         /**< 设置主机电源状态                 */
    APP_SERVICE_CMD_ENTER_FORCED_DEEP_SLEEP,/**< 进入强制深度睡眠                 */
    APP_SERVICE_CMD_PREPARE_STOP,           /**< 准备停机                         */
    APP_SERVICE_CMD_PREPARE_STANDBY,        /**< 准备待机                         */
    APP_SERVICE_CMD_SEND_THERMAL_SNAPSHOT,  /**< 发送热成像快照数据               */

    /* --- OTA 类命令 --- */
    APP_SERVICE_CMD_OTA_QUERY_LATEST        /**< 查询最新 OTA 固件版本            */
} app_service_cmd_id_t;

/* =========================================================================
 *  3. 服务命令结构体
 * ======================================================================= */

/**
 * @brief 服务命令结构体
 * @note  由页面层构造并提交到服务总线，服务任务通过执行器解析并处理。
 *        各字段含义由具体命令 ID 定义，arg0/arg1/value 为通用载荷。
 */
typedef struct
{
    app_service_cmd_id_t cmd_id;    /**< 命令 ID，标识命令类型              */
    uint8_t              arg0;      /**< 通用参数 0（由具体命令定义含义）   */
    uint8_t              arg1;      /**< 通用参数 1（由具体命令定义含义）   */
    uint32_t             value;     /**< 通用参数值（由具体命令定义含义）   */
} app_service_cmd_t;

/* =========================================================================
 *  4. 服务响应结构体
 * ======================================================================= */

/** @defgroup SERVICE_RSP  服务响应相关常量
 *  @{ */
#define APP_SERVICE_TEXT_LEN    24U     /**< 响应文本字段长度（字节）         */
/** @} */

/**
 * @brief 服务响应结构体
 * @note  由执行器填充，通过 UI 消息队列回传给 UI 任务，
 *        或通过完成信号量回传给同步调用方。
 */
typedef struct
{
    app_service_cmd_id_t cmd_id;                /**< 对应的命令 ID             */
    uint8_t              ok;                    /**< 执行结果：1=成功，0=失败  */
    uint8_t              reserved;              /**< 保留字段（对齐填充）      */
    uint16_t             reason;                /**< 失败原因码                */
    uint32_t             value;                 /**< 返回值（由具体命令定义）  */
    char                 text[APP_SERVICE_TEXT_LEN]; /**< 返回文本（如版本号） */
} app_service_rsp_t;

/* =========================================================================
 *  5. 执行器函数类型定义
 * ======================================================================= */

/**
 * @brief 服务命令执行器函数类型
 * @note  由上层实现并注册到服务总线，服务任务在消费命令队列时回调此函数。
 * @param  cmd — 输入：命令结构体指针
 * @param  rsp — 输出：响应结构体指针（由调用方预分配）
 * @retval 1 — 执行成功；0 — 执行失败
 */
typedef uint8_t (*app_service_execute_fn_t)(const app_service_cmd_t *cmd,
                                            app_service_rsp_t *rsp);

/* =========================================================================
 *  6. 服务总线初始化与配置接口
 * ======================================================================= */

/**
 * @brief  复位服务总线所有内部状态
 * @note   在系统初始化早期调用，将所有句柄、队列、缓存清零。
 */
void app_service_bus_reset(void);

/**
 * @brief  注册命令执行器回调函数
 * @note   执行器负责根据命令 ID 分发到具体的业务处理逻辑。
 *         由 app_rtos_runtime_init 在初始化阶段注册一次。
 * @param  execute_fn — 执行器函数指针
 */
void app_service_bus_register_executor(app_service_execute_fn_t execute_fn);

/**
 * @brief  初始化服务总线
 * @note   创建服务命令队列、完成信号量和同步互斥锁。
 *         UI 消息队列由外部创建并传入。
 * @param  ui_msg_queue      — 外部 UI 响应消息队列句柄
 * @param  service_queue_len — 服务命令队列深度
 * @retval 1 — 初始化成功；0 — 参数无效或资源创建失败
 */
uint8_t app_service_bus_init(QueueHandle_t ui_msg_queue, uint32_t service_queue_len);

/* =========================================================================
 *  7. 任务句柄绑定接口
 * ======================================================================= */

/**
 * @brief  设置服务任务句柄
 * @note   由 start_task 在创建服务任务后调用，用于 TaskNotify 唤醒。
 * @param  service_task_handle — 服务任务句柄
 */
void app_service_bus_set_service_task_handle(TaskHandle_t service_task_handle);

/**
 * @brief  设置 UI 任务句柄
 * @note   由 start_task 在创建 UI 任务后调用，用于 TaskNotify 唤醒。
 * @param  ui_task_handle — UI 任务句柄
 */
void app_service_bus_set_ui_task_handle(TaskHandle_t ui_task_handle);

/* =========================================================================
 *  8. 服务总线运行时接口
 * ======================================================================= */

/**
 * @brief  查询服务命令队列中是否有待处理命令
 * @retval 1 — 队列中有待处理命令；0 — 队列为空或未初始化
 */
uint8_t app_service_bus_has_pending_work(void);

/**
 * @brief  通知服务任务有新命令待处理
 * @note   通过 TaskNotify 唤醒服务任务。
 */
void app_service_bus_notify_service_task(void);

/**
 * @brief  服务任务调用：消费命令队列并执行
 * @note   非阻塞地取出队列中所有待处理命令，逐条执行。
 *         执行完成后将响应投递到 UI 消息队列。
 */
void app_service_bus_process(void);

/**
 * @brief  尝试补发所有命令 ID 的延迟缓存
 * @note   遍历全部有效命令 ID，逐个尝试补发因队列满或同 ID 冲突而暂存的命令。
 *         由服务任务在每个主循环末尾调用。
 * @retval 1 — 至少有一条延迟命令补发成功；0 — 无延迟命令或全部补发失败
 */
uint8_t app_service_bus_try_enqueue_deferred_any(void);

/* =========================================================================
 *  9. 页面层命令提交接口
 * ======================================================================= */

/**
 * @brief  同步提交服务命令
 * @note   调用方阻塞等待服务任务执行完成后返回结果。
 *         特殊场景处理：
 *         - 若当前在服务任务上下文中，直接调用执行器避免死锁
 *         - 若调度器未运行，直接调用执行器（初始化阶段兼容）
 *         - 使用互斥锁保证同一时刻只有一条同步命令在等待
 * @param  cmd        — 输入：命令结构体指针
 * @param  rsp        — 输出：响应结构体指针（可为 NULL）
 * @param  timeout_ms — 超时时间（毫秒），为 0 时使用默认值
 * @retval 1 — 命令执行成功；0 — 参数无效、超时或队列满
 */
uint8_t app_service_submit(const app_service_cmd_t *cmd,
                           app_service_rsp_t *rsp,
                           uint32_t timeout_ms);

/**
 * @brief  异步提交服务命令
 * @note   将命令投递到队列后立即返回，不等待执行结果。
 *         若同 ID 命令正在执行中或队列满，命令将暂存到延迟缓冲区，
 *         待条件满足后自动补发。
 * @param  cmd — 输入：命令结构体指针
 * @retval 1 — 投递成功或已暂存；0 — 参数无效或直接执行失败
 */
uint8_t app_service_submit_async(const app_service_cmd_t *cmd);

#endif /* APP_SERVICE_BUS_H */
