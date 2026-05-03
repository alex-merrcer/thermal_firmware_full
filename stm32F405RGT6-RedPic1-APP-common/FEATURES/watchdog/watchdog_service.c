/**
 * @file    watchdog_service.c
 * @brief   看门狗服务层 —— 业务侧看门狗接口封装
 * @note    本模块为上层业务提供简化的看门狗接口，内部委托给
 *          watchdog_supervisor 执行实际的喂狗与健康检查逻辑。
 *
 * @par 默认监控任务
 *      初始化时自动注册以下 7 个监控任务：
 *      - main_loop:  主循环心跳
 *      - key:        按键扫描
 *      - ota:        OTA 升级
 *      - esp_host:   ESP32 主机通信
 *      - battery:    电池监测
 *      - ui:         UI 渲染
 *      - power:      电源管理
 *
 * @par UART 错误上报
 *      将 UART 接收环形缓冲区的错误标志映射为看门狗故障标志：
 *      - UART_RX_RING_FLAG_OVERFLOW → WATCHDOG_FAULT_UART_OVERFLOW
 *      - UART_RX_RING_FLAG_ORE/FE/NE → WATCHDOG_FAULT_UART_ERROR
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "watchdog_service.h"

#include "uart_rx_ring.h"
#include "watchdog_supervisor.h"

/* =========================================================================
 *  2. 内部函数实现 —— 默认任务注册
 * ======================================================================= */

/**
 * @brief  注册默认监控任务列表
 * @note   将 7 个核心业务任务注册到看门狗监管器。
 *         任一任务注册失败时立即返回 0。
 * @retval 1 — 全部注册成功；0 — 注册失败
 */
static uint8_t watchdog_service_register_default_tasks(void)
{
    static const watchdog_supervisor_task_t s_default_tasks[] = {
        { "main_loop", WATCHDOG_PROGRESS_MAIN_LOOP, 1U },  /**< 主循环（必需）   */
        { "key",       WATCHDOG_PROGRESS_KEY,       1U },  /**< 按键扫描（必需） */
        { "ota",       WATCHDOG_PROGRESS_OTA,       1U },  /**< OTA 升级（必需） */
        { "esp_host",  WATCHDOG_PROGRESS_ESP_HOST,  1U },  /**< ESP 主机（必需） */
        { "battery",   WATCHDOG_PROGRESS_BATTERY,   1U },  /**< 电池监测（必需） */
        { "ui",        WATCHDOG_PROGRESS_UI,        1U },  /**< UI 渲染（必需）  */
        { "power",     WATCHDOG_PROGRESS_POWER,     1U },  /**< 电源管理（必需） */
        { "display",   WATCHDOG_PROGRESS_DISPLAY,   1U },  /**< 显示刷新（必需） */
        { "thermal",   WATCHDOG_PROGRESS_THERMAL,   1U }   /**< 热成像（必需）   */
    };
    uint8_t i = 0U;

    for (i = 0U; i < (uint8_t)(sizeof(s_default_tasks) / sizeof(s_default_tasks[0])); ++i)
    {
        if (watchdog_supervisor_register_task(&s_default_tasks[i]) == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

/* =========================================================================
 *  3. 公共接口实现 —— 初始化
 * ======================================================================= */

/**
 * @brief  初始化看门狗服务
 * @note   初始化监管器并注册默认监控任务。
 * @param  feed_interval_ms — 喂狗间隔（ms），0 使用默认值（1000ms）
 */
void watchdog_service_init(uint32_t feed_interval_ms)
{
    watchdog_supervisor_init(feed_interval_ms);
    (void)watchdog_service_register_default_tasks();
}

/* =========================================================================
 *  4. 公共接口实现 —— 周期管理
 * ======================================================================= */

/**
 * @brief  开启一个新的监控窗口
 * @note   清除上一窗口的进度和故障标志，开始新一轮健康检查。
 */
void watchdog_service_begin_cycle(void)
{
    watchdog_supervisor_begin_window();
}

/**
 * @brief  标记指定任务的进度
 * @param  mask — 进度位掩码（WATCHDOG_PROGRESS_xxx）
 */
void watchdog_service_mark_progress(uint32_t mask)
{
    watchdog_supervisor_mark_progress(mask);
}

/* =========================================================================
 *  5. 公共接口实现 —— 故障上报
 * ======================================================================= */

/**
 * @brief  上报按键健康状态
 * @note   自动标记 KEY 进度位；若不健康则上报 KEY_STUCK 故障。
 * @param  healthy — 1=健康；0=卡键
 */
void watchdog_service_report_key_health(uint8_t healthy)
{
    watchdog_supervisor_mark_progress(WATCHDOG_PROGRESS_KEY);
    if (healthy == 0U)
    {
        watchdog_supervisor_report_fault_flags(WATCHDOG_FAULT_KEY_STUCK);
    }
}

/**
 * @brief  上报 UART 接收错误
 * @note   将 uart_rx_ring 的错误标志映射为看门狗故障标志。
 * @param  flags — UART 错误标志位（UART_RX_RING_FLAG_xxx）
 */
void watchdog_service_report_uart_errors(uint32_t flags)
{
    if (flags == 0U)
    {
        return;
    }

    /* 缓冲区溢出 */
    if ((flags & UART_RX_RING_FLAG_OVERFLOW) != 0U)
    {
        watchdog_supervisor_report_fault_flags(WATCHDOG_FAULT_UART_OVERFLOW);
    }

    /* 硬件错误（溢出/帧错误/噪声） */
    if ((flags & (UART_RX_RING_FLAG_ORE | UART_RX_RING_FLAG_FE | UART_RX_RING_FLAG_NE)) != 0U)
    {
        watchdog_supervisor_report_fault_flags(WATCHDOG_FAULT_UART_ERROR);
    }
}

/* =========================================================================
 *  6. 公共接口实现 —— STOP 模式支持
 * ======================================================================= */

/**
 * @brief  通知看门狗即将从 STOP 模式唤醒
 * @note   唤醒后首周期不检查健康状态，避免误报。
 */
void watchdog_service_note_stop_wake(void)
{
    watchdog_supervisor_note_stop_wake();
}

/* =========================================================================
 *  7. 公共接口实现 —— 调度与喂狗
 * ======================================================================= */

/**
 * @brief  执行一次看门狗调度步骤
 * @note   检查当前窗口健康状态，健康时按间隔喂狗。
 *         不健康时捕获复位快照但不喂狗（将触发硬件复位）。
 */
void watchdog_service_step(void)
{
    watchdog_supervisor_step();
}

/**
 * @brief  强制立即喂狗
 * @note   用于特殊场景（如 OTA 升级前）防止意外复位。
 */
void watchdog_service_force_feed(void)
{
    watchdog_supervisor_force_feed();
}

/* =========================================================================
 *  8. 公共接口实现 —— 状态查询
 * ======================================================================= */

/**
 * @brief  查询当前监控窗口是否健康
 * @retval 1 — 健康（所有必需任务已上报进度且无故障）；0 — 不健康
 */
uint8_t watchdog_service_is_healthy(void)
{
    return watchdog_supervisor_is_healthy();
}

/**
 * @brief  查询是否可以安全进入 STOP 低功耗模式
 * @retval 1 — 可以进入；0 — 当前不健康，不应进入
 */
uint8_t watchdog_service_can_enter_stop(void)
{
    return watchdog_supervisor_can_enter_stop();
}

/**
 * @brief  获取缺失的进度位掩码
 * @return 未上报进度的必需任务位掩码
 */
uint32_t watchdog_service_get_missing_progress_mask(void)
{
    return watchdog_supervisor_get_missing_progress_mask();
}

/**
 * @brief  获取最近一次的故障标志
 * @return 故障位掩码（WATCHDOG_FAULT_xxx）
 */
uint32_t watchdog_service_get_last_fault_flags(void)
{
    return watchdog_supervisor_get_last_fault_flags();
}

/**
 * @brief  获取当前喂狗间隔
 * @return 喂狗间隔（ms）
 */
uint32_t watchdog_service_get_feed_interval_ms(void)
{
    return watchdog_supervisor_get_feed_interval_ms();
}

/**
 * @brief  获取看门狗复位快照
 * @note   复位快照记录了导致看门狗复位时的诊断信息。
 * @param  out_snapshot — 输出：复位快照结构体指针
 */
void watchdog_service_get_reset_snapshot(watchdog_reset_snapshot_t *out_snapshot)
{
    watchdog_supervisor_reset_snapshot_t snapshot;

    if (out_snapshot == 0)
    {
        return;
    }

    watchdog_supervisor_get_reset_snapshot(&snapshot);

    /* 从内部快照格式转换为服务层快照格式 */
    out_snapshot->tick_ms = snapshot.tick_ms;
    out_snapshot->missing_progress_mask = snapshot.missing_progress_mask;
    out_snapshot->fault_flags = snapshot.fault_flags;
    out_snapshot->valid = snapshot.valid;
}
