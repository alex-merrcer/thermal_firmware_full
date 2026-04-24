#ifndef REDPIC1_APP_H
#define REDPIC1_APP_H

#include <stdint.h>

#include "settings_service.h"

/*
 * redpic1_app.h
 * RTOS 应用运行时公共头文件。
 *
 * 该模块负责对外暴露：
 * 1. 应用运行时初始化与启动入口
 * 2. 页面/服务任务之间共用的服务命令契约
 * 3. LCD 与设置服务的线程安全包装接口
 */

/* UI 输入任务投递到 UI 管理层的按键事件。 */
typedef struct
{
    /* 已完成消抖与长按归一化后的按键值。 */
    uint8_t key_value;
    /* 事件入队时的系统毫秒节拍。 */
    uint32_t tick_ms;
} app_key_event_t;

/* 应用服务任务支持的命令类型。枚举值同时作为内部索引使用，顺序不得随意调整。 */
typedef enum
{
    APP_SERVICE_CMD_NONE = 0,
    APP_SERVICE_CMD_ESP_REFRESH_STATUS,
    APP_SERVICE_CMD_SET_WIFI,
    APP_SERVICE_CMD_SET_DEBUG_SCREEN,
    APP_SERVICE_CMD_SET_REMOTE_KEYS,
    APP_SERVICE_CMD_SET_POWER_POLICY,
    APP_SERVICE_CMD_SET_HOST_STATE,
    APP_SERVICE_CMD_ENTER_FORCED_DEEP_SLEEP,
    APP_SERVICE_CMD_PREPARE_STOP,
    APP_SERVICE_CMD_PREPARE_STANDBY,
    APP_SERVICE_CMD_OTA_QUERY_LATEST
} app_service_cmd_id_t;

/* 投递给服务任务的命令载荷。
 * arg0/arg1 用于轻量参数，value 用于超时、目标值等 32 位参数。 */
typedef struct
{
    app_service_cmd_id_t cmd_id;
    uint8_t arg0;
    uint8_t arg1;
    uint32_t value;
} app_service_cmd_t;

/* 服务响应文本缓冲区长度。 */
#define APP_SERVICE_TEXT_LEN 24U

/* 服务任务返回给调用方或 UI 层的统一响应。 */
typedef struct
{
    /* 对应的命令类型，用于 UI 层按命令分发。 */
    app_service_cmd_id_t cmd_id;
    /* 1 表示命令执行成功，0 表示失败。 */
    uint8_t ok;
    /* 预留字段，保持结构体对齐。 */
    uint8_t reserved;
    /* 失败原因或业务侧拒绝码。 */
    uint16_t reason;
    /* 扩展返回值，按命令语义解释。 */
    uint32_t value;
    /* 可选文本返回，例如 OTA 查询得到的版本号。 */
    char text[APP_SERVICE_TEXT_LEN];
} app_service_rsp_t;

/* 初始化 RTOS 运行时依赖、驱动与任务间同步对象。
 * 该接口只负责构建运行环境，不启动调度器。 */
void app_rtos_runtime_init(void);

/* 创建启动任务并启动 FreeRTOS 调度器。 */
void app_rtos_runtime_start(void);

/* 同步提交服务命令。
 * 若当前不在调度器上下文，函数会退化为直接执行路径。 */
uint8_t app_service_submit(const app_service_cmd_t *cmd,
                           app_service_rsp_t *rsp,
                           uint32_t timeout_ms);

/* 异步提交服务命令。
 * 当同类命令仍在执行时，最新请求会被覆盖保存为 deferred 命令。 */
uint8_t app_service_submit_async(const app_service_cmd_t *cmd);

/* LCD 显示临界区包装，底层实际由显示运行时模块提供互斥。 */
void app_rtos_lcd_lock(void);
void app_rtos_lcd_unlock(void);

/* 设置服务互斥包装。
 * 所有运行时读写持久化设置的路径都应通过这组接口统一进入。 */
void app_rtos_settings_lock(void);
void app_rtos_settings_unlock(void);
void app_rtos_settings_copy(device_settings_t *out_settings);
uint8_t app_rtos_settings_update(const device_settings_t *settings);

/* 应用主入口。
 * 负责完成底层时钟/中断初始化后进入 RTOS 运行时。 */
void redpic1_app_main(void);

#endif
