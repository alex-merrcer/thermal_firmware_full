/**
 * @file    thermal_capture.c
 * @brief   热成像采集控制模块
 * @note    本模块负责 MLX90640 传感器的帧采集控制，包括：
 *          - 采集前的退避（Backoff）判定
 *          - I2C 总线故障恢复机制
 *          - 连续传输失败的分级退避策略
 *          - 帧数据读取与性能计时
 *
 * @par 退避策略
 *      - 第 1 次传输失败：退避 2ms
 *      - 第 2 次传输失败：退避 5ms
 *      - 第 3 次及以上：退避 20ms 并标记总线恢复请求
 *      - 非传输类错误：退避 2ms，重置连续失败计数
 *
 * @par 总线恢复流程
 *      当连续传输失败达到阈值时，标记 s_restore_bus_pending。
 *      在下次 prepare_step 中执行：重新初始化 I2C → 重写刷新率 → 失效历史数据。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "thermal_capture.h"

#include <string.h>

#include "app_perf_baseline.h"
#include "power_manager.h"
#include "MLX90640.h"
#include "MLX90640_I2C_Driver.h"
#include "redpic1_thermal.h"

/* =========================================================================
 *  2. 内部宏定义
 * ======================================================================= */

#define THERMAL_CAPTURE_BACKOFF_MS      20UL    /**< 总线恢复退避时间（ms）    */

/* =========================================================================
 *  3. 模块级静态变量
 * ======================================================================= */

static redpic1_thermal_capture_ops_t s_ops;             /**< 回调函数集                */
static uint32_t s_backoff_until_ms = 0U;                /**< 退避截止时间（ms）        */
static uint8_t  s_restore_bus_pending = 0U;             /**< 总线恢复待执行标志        */
static uint8_t  s_consecutive_transport_failures = 0U;  /**< 连续传输失败计数          */

/* =========================================================================
 *  4. 内部函数实现 —— 回调代理
 * ======================================================================= */

/**
 * @brief  获取当前刷新率（通过回调）
 * @return 刷新率枚举值；未注册回调时返回 0
 */
static uint8_t redpic1_thermal_capture_get_refresh_rate(void)
{
    if (s_ops.get_refresh_rate != 0)
    {
        return s_ops.get_refresh_rate();
    }

    return 0U;
}

/**
 * @brief  应用刷新率设置（通过回调）
 * @param  refresh_rate — 目标刷新率
 * @param  force_write  — 是否强制写入（忽略当前值比较）
 */
static void redpic1_thermal_capture_apply_refresh_rate(uint8_t refresh_rate, uint8_t force_write)
{
    if (s_ops.apply_refresh_rate != 0)
    {
        s_ops.apply_refresh_rate(refresh_rate, force_write);
    }
}

/**
 * @brief  失效历史数据（通过回调）
 * @note   在总线恢复或采集错误后调用，清除滤波/窗口历史。
 */
static void redpic1_thermal_capture_invalidate_history(void)
{
    if (s_ops.invalidate_history != 0)
    {
        s_ops.invalidate_history();
    }
}

/* =========================================================================
 *  5. 内部函数实现 —— 时间判定与总线恢复
 * ======================================================================= */

/**
 * @brief  判断当前时间是否已到达截止时间
 * @param  now_ms     — 当前系统时间（ms）
 * @param  deadline_ms — 截止时间（ms）
 * @retval 1 — 已到达；0 — 未到达
 */
static uint8_t redpic1_thermal_capture_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (((int32_t)(now_ms - deadline_ms)) >= 0) ? 1U : 0U;
}

/**
 * @brief  立即执行 I2C 总线恢复
 * @note   重新初始化 I2C → 重写刷新率 → 失效历史数据 → 重置失败计数。
 */
static void redpic1_thermal_capture_restore_bus_now(void)
{
    MLX90640_I2CInit();
    redpic1_thermal_capture_apply_refresh_rate(
        redpic1_thermal_capture_get_refresh_rate(), 1U);
    s_restore_bus_pending = 0U;
    s_consecutive_transport_failures = 0U;
    redpic1_thermal_capture_invalidate_history();
}

/* =========================================================================
 *  6. 公共接口实现 —— 初始化与重置
 * ======================================================================= */

/**
 * @brief  初始化采集控制模块
 * @param  ops — 回调函数集指针（可为 NULL）
 */
void redpic1_thermal_capture_init(const redpic1_thermal_capture_ops_t *ops)
{
    memset(&s_ops, 0, sizeof(s_ops));
    if (ops != 0)
    {
        memcpy(&s_ops, ops, sizeof(s_ops));
    }

    redpic1_thermal_capture_reset();
}

/**
 * @brief  重置采集控制状态
 * @note   清除退避定时器、总线恢复标志和失败计数。
 */
void redpic1_thermal_capture_reset(void)
{
    s_backoff_until_ms = 0U;
    s_restore_bus_pending = 0U;
    s_consecutive_transport_failures = 0U;
}

/* =========================================================================
 *  7. 公共接口实现 —— 退避与恢复
 * ======================================================================= */

/**
 * @brief  记录一次采集失败并设置退避
 * @note   传输类错误采用分级退避策略，非传输类错误统一退避 2ms。
 * @param  transport_related — 是否为传输类错误（I2C 总线故障）
 */
void redpic1_thermal_capture_note_backoff(uint8_t transport_related)
{
    uint32_t now_ms = power_manager_get_tick_ms();

    /* 记录性能基线 */
    app_perf_baseline_record_thermal_capture_failure();
    app_perf_baseline_record_thermal_backoff();

    if (transport_related != 0U)
    {
        /* 传输类错误：分级退避 */
        s_consecutive_transport_failures++;

        if (s_consecutive_transport_failures == 1U)
        {
            /* 第 1 次：退避 2ms */
            s_backoff_until_ms = now_ms + 2U;
        }
        else if (s_consecutive_transport_failures == 2U)
        {
            /* 第 2 次：退避 5ms */
            s_backoff_until_ms = now_ms + 5U;
        }
        else
        {
            /* 第 3 次及以上：标记总线恢复，退避 20ms */
            s_restore_bus_pending = 1U;
            s_backoff_until_ms = now_ms + THERMAL_CAPTURE_BACKOFF_MS;
        }
    }
    else
    {
        /* 非传输类错误：退避 2ms，重置连续失败计数 */
        s_backoff_until_ms = now_ms + 2U;
        s_consecutive_transport_failures = 0U;
    }
}

/**
 * @brief  采集准备步骤（退避判定与总线恢复）
 * @note   在每次采集前调用，检查退避是否到期，必要时执行总线恢复。
 * @retval 1 — 可以继续采集；0 — 仍在退避中
 */
uint8_t redpic1_thermal_capture_prepare_step(void)
{
    uint32_t now_ms = power_manager_get_tick_ms();

    /* 退避未到期：跳过本次采集 */
    if (s_backoff_until_ms != 0U &&
        redpic1_thermal_capture_deadline_reached(now_ms, s_backoff_until_ms) == 0U)
    {
        return 0U;
    }

    /* 退避到期：清除退避定时器 */
    s_backoff_until_ms = 0U;

    /* 有待执行的总线恢复：立即执行 */
    if (s_restore_bus_pending != 0U)
    {
        redpic1_thermal_capture_restore_bus_now();
    }

    return 1U;
}

/* =========================================================================
 *  8. 公共接口实现 —— 帧读取
 * ======================================================================= */

/**
 * @brief  读取一帧热成像数据
 * @note   调用 MLX90640 库的 get_temp_ex 函数，记录耗时并处理错误。
 * @param  frame_data            — 输出：帧温度数据缓冲区（768 floats）
 * @param  out_subpage           — 输出：采集到的子页编号（可选）
 * @param  out_capture_tick_ms   — 输出：采集完成时间戳（可选）
 * @param  out_get_temp_elapsed_us — 输出：get_temp_ex 耗时（μs，可选）
 * @retval 1 — 采集成功；0 — 采集失败
 */
uint8_t redpic1_thermal_capture_read_frame(float *frame_data,
                                           uint8_t *out_subpage,
                                           uint32_t *out_capture_tick_ms,
                                           uint32_t *out_get_temp_elapsed_us)
{
    uint32_t get_temp_start_cycle = 0U;
    uint32_t get_temp_elapsed_us = 0U;
    float ta = 0.0f;
    uint8_t captured_subpage = 0U;
    int temp_status = 0;

    if (frame_data == 0)
    {
        return 0U;
    }

    /* 调用 MLX90640 库读取帧数据 */
    get_temp_start_cycle = app_perf_baseline_cycle_now();
    temp_status = get_temp_ex(frame_data, &ta, &captured_subpage);
    get_temp_elapsed_us = app_perf_baseline_elapsed_us(get_temp_start_cycle);

    /* 记录耗时 */
    app_perf_baseline_record_get_temp_us(get_temp_elapsed_us);

    if (out_get_temp_elapsed_us != 0)
    {
        *out_get_temp_elapsed_us = get_temp_elapsed_us;
    }

    /* 错误处理 */
    if (temp_status < 0)
    {
        if (temp_status == -9)
        {
            /* 软超时：非传输类错误 */
            app_perf_baseline_record_thermal_soft_timeout();
            redpic1_thermal_capture_note_backoff(0U);
        }
        else
        {
            /* 传输类错误：失效历史数据并退避 */
            redpic1_thermal_capture_invalidate_history();
            redpic1_thermal_capture_note_backoff(1U);
        }

        return 0U;
    }

    /* 输出子页编号 */
    if (out_subpage != 0)
    {
        *out_subpage = captured_subpage;
    }

    /* 输出采集时间戳 */
    if (out_capture_tick_ms != 0)
    {
        *out_capture_tick_ms = power_manager_get_tick_ms();
    }

    return 1U;
}

/* =========================================================================
 *  9. 公共接口实现 —— 成功通知与 STOP 恢复
 * ======================================================================= */

/**
 * @brief  通知采集成功
 * @note   重置连续传输失败计数。
 */
void redpic1_thermal_capture_note_success(void)
{
    s_consecutive_transport_failures = 0U;
}

/**
 * @brief  请求在 STOP 唤醒后恢复 I2C 总线
 * @note   若调度器已运行，则延迟到 prepare_step 中恢复；
 *         否则立即执行恢复（中断上下文安全）。
 * @param  scheduler_running — FreeRTOS 调度器是否运行中
 */
void redpic1_thermal_capture_request_restore_after_stop(uint8_t scheduler_running)
{
    if (scheduler_running != 0U)
    {
        /* 调度器运行中：延迟恢复 */
        s_restore_bus_pending = 1U;
        s_backoff_until_ms = 0U;
        return;
    }

    /* 调度器未启动：立即恢复 */
    redpic1_thermal_capture_restore_bus_now();
}
