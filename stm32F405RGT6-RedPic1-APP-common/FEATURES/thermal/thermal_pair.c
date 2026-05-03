/**
 * @file    thermal_pair.c
 * @brief   MLX90640 子页配对合成模块
 * @note    本模块负责将 MLX90640 的两个子页（subpage 0/1）数据合成为完整帧。
 *
 * @par 子页机制
 *      MLX90640 每次采集只输出半个像素阵列（交错模式）。
 *      像素归属由 (row XOR col) & 1 决定：
 *      - subpage 0: (row+col) 为偶数的像素
 *      - subpage 1: (row+col) 为奇数的像素
 *      需要两个连续子页合成一帧完整 32×24 温度数据。
 *
 * @par 配对策略
 *      1. 缓存每个子页的最新帧数据和时间戳
 *      2. 当对端子页有效时，按像素归属合成完整帧
 *      3. 子页超时判定：
 *         - 正常超时（>80ms）：检查宽限窗口（>160ms 才丢弃）
 *         - 宽限期内仍可配对，记录 pair_grace_ok 性能事件
 *         - 超过宽限期：丢弃旧子页，记录 pair_timeout_detail
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "thermal_pair.h"

#include <string.h>

#include "app_perf_baseline.h"
#include "redpic1_thermal.h"
#include "sys.h"

/* =========================================================================
 *  2. 内部宏定义
 * ======================================================================= */

#define THERMAL_PAIR_PIXEL_COUNT    768U    /**< 总像素数（32×24）    */
#define THERMAL_PAIR_SRC_COLS       32U     /**< 图像列数             */

/* =========================================================================
 *  3. 模块级静态变量
 * ======================================================================= */

/** 子页帧数据缓冲区（CCM RAM，双缓冲） */
static CCMRAM float s_v4_subpage_temp_frame[2][THERMAL_PAIR_PIXEL_COUNT];

static uint32_t s_v4_subpage_tick_ms[2]           = { 0U, 0U };   /**< 子页采集时间戳  */
static uint8_t  s_v4_subpage_valid[2]             = { 0U, 0U };   /**< 子页有效性标志  */
static uint8_t  s_v4_pair_last_arrived_subpage    = 0xFFU;         /**< 上次到达的子页  */
static uint32_t s_v4_pair_same_subpage_streak     = 0U;            /**< 连续同子页计数  */

/* =========================================================================
 *  4. 内部函数实现 —— 像素子页归属判定
 * ======================================================================= */

/**
 * @brief  判断像素所属子页编号
 * @note   归属规则：(row XOR col) & 1
 * @param  pixel_index — 像素线性索引（0~767）
 * @return 子页编号（0 或 1）
 */
static uint8_t redpic1_thermal_pair_pixel_subpage(uint16_t pixel_index)
{
    uint16_t row = (uint16_t)(pixel_index / THERMAL_PAIR_SRC_COLS);
    uint16_t col = (uint16_t)(pixel_index % THERMAL_PAIR_SRC_COLS);

    return (uint8_t)((row ^ col) & 0x01U);
}

/* =========================================================================
 *  5. 公共接口实现 —— 重置
 * ======================================================================= */

/**
 * @brief  重置子页配对状态
 * @note   清除所有子页缓存、时间戳和连续同子页计数。
 */
void redpic1_thermal_pair_reset(void)
{
    s_v4_subpage_tick_ms[0]          = 0U;
    s_v4_subpage_tick_ms[1]          = 0U;
    s_v4_subpage_valid[0]            = 0U;
    s_v4_subpage_valid[1]            = 0U;
    s_v4_pair_last_arrived_subpage   = 0xFFU;
    s_v4_pair_same_subpage_streak    = 0U;
}

/* =========================================================================
 *  6. 公共接口实现 —— 子页配对合成
 * ======================================================================= */

/**
 * @brief  尝试将当前子页与对端子页合成为完整帧
 * @note   流程：
 *         1. 缓存当前子页数据
 *         2. 检查对端子页有效性与超时
 *         3. 对端有效时按像素归属合成完整帧
 *         4. 对端无效时记录等待事件并返回 0
 * @param  frame_data          — 输入/输出：帧温度数据（768 floats）
 * @param  subpage             — 当前子页编号（0 或 1）
 * @param  capture_tick_ms     — 采集时间戳（ms）
 * @param  get_temp_elapsed_us — get_temp_ex 耗时（μs）
 * @param  step_elapsed_us     — step 函数耗时（μs）
 * @param  out_capture_tick_ms — 输出：合成帧时间戳（可选）
 * @retval 1 — 合成成功；0 — 等待对端子页
 */
uint8_t redpic1_thermal_pair_try_compose(float *frame_data,
                                         uint8_t subpage,
                                         uint32_t capture_tick_ms,
                                         uint32_t get_temp_elapsed_us,
                                         uint32_t step_elapsed_us,
                                         uint32_t *out_capture_tick_ms)
{
    uint8_t  other_subpage   = (uint8_t)(subpage ^ 0x01U);
    uint16_t pixel_index     = 0U;
    uint32_t gap_ms          = 0U;
    uint8_t  pair_grace_ok   = 0U;

    if (frame_data == 0 || subpage > 1U)
    {
        return 0U;
    }

    /* 追踪连续同子页到达 */
    if (s_v4_pair_last_arrived_subpage == subpage)
    {
        s_v4_pair_same_subpage_streak++;
    }
    else
    {
        s_v4_pair_last_arrived_subpage = subpage;
        s_v4_pair_same_subpage_streak  = 1U;
    }

    /* 缓存当前子页数据 */
    memcpy(s_v4_subpage_temp_frame[subpage],
           frame_data,
           sizeof(s_v4_subpage_temp_frame[subpage]));
    s_v4_subpage_tick_ms[subpage] = capture_tick_ms;
    s_v4_subpage_valid[subpage]   = 1U;

    /* 计算与对端子页的时间间隔 */
    if (s_v4_subpage_valid[other_subpage] != 0U)
    {
        gap_ms = capture_tick_ms - s_v4_subpage_tick_ms[other_subpage];
    }

    /* 对端子页超时判定 */
    if (s_v4_subpage_valid[other_subpage] != 0U &&
        gap_ms > REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_MAX_AGE_MS)
    {
        if (gap_ms <= REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_GRACE_AGE_MS)
        {
            /* 宽限期内：允许配对 */
            pair_grace_ok = 1U;
        }
        else
        {
            /* 超过宽限期：丢弃旧子页 */
            app_perf_baseline_record_thermal_pair_timeout_detail(
                subpage, other_subpage, gap_ms,
                s_v4_pair_same_subpage_streak,
                get_temp_elapsed_us, step_elapsed_us);
            s_v4_subpage_valid[other_subpage] = 0U;
            s_v4_subpage_tick_ms[other_subpage] = 0U;
        }
    }

    /* 对端子页无效：等待 */
    if (s_v4_subpage_valid[other_subpage] == 0U)
    {
        app_perf_baseline_record_thermal_pair_wait_other(
            subpage, other_subpage, gap_ms,
            s_v4_pair_same_subpage_streak);
        return 0U;
    }

    /* 按像素归属合成完整帧 */
    for (pixel_index = 0U; pixel_index < THERMAL_PAIR_PIXEL_COUNT; ++pixel_index)
    {
        uint8_t owner_subpage = redpic1_thermal_pair_pixel_subpage(pixel_index);
        frame_data[pixel_index] = s_v4_subpage_temp_frame[owner_subpage][pixel_index];
    }

    /* 输出合成帧时间戳（取两者中较新者） */
    if (out_capture_tick_ms != 0)
    {
        *out_capture_tick_ms =
            (s_v4_subpage_tick_ms[subpage] >= s_v4_subpage_tick_ms[other_subpage]) ?
            s_v4_subpage_tick_ms[subpage] :
            s_v4_subpage_tick_ms[other_subpage];
    }

    /* 记录配对性能事件 */
    if (pair_grace_ok != 0U)
    {
        app_perf_baseline_record_thermal_pair_grace_ok(
            subpage, other_subpage, gap_ms,
            s_v4_pair_same_subpage_streak);
    }
    else
    {
        app_perf_baseline_record_thermal_pair_compose_ok(
            subpage, other_subpage, gap_ms,
            s_v4_pair_same_subpage_streak);
    }

    return 1U;
}
