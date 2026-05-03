/**
 * @file    thermal_visual.c
 * @brief   热成像可视化处理模块
 * @note    本模块负责将原始温度数据转换为 LCD 灰度帧，包括：
 *          1. 帧间滤波（自适应权重 EMA）
 *          2. 运动检测（高运动帧判定）
 *          3. 显示窗口计算（百分位截断 + EMA 平滑 + 步长限制）
 *          4. 温度→灰度映射（线性缩放 + 钳位）
 *          5. 灰度帧锐化（拉普拉斯锐化）
 *          6. 帧数据有效性校验
 *
 * @par 帧间滤波策略
 *      采用自适应权重 EMA 滤波：
 *      - |ΔT| ≤ 0.20°C: 权重 0.40（强平滑，抑制噪声）
 *      - 0.20 < |ΔT| < 1.00°C: 权重线性插值 0.40~1.00
 *      - |ΔT| ≥ 1.00°C: 权重 1.00（无滤波，快速响应）
 *
 * @par 显示窗口策略
 *      1. 百分位截断：P2 ~ P99.5 + 8% headroom
 *      2. EMA 平滑：正常 α=0.25 / 高运动 α=0.45
 *      3. 步长限制：正常 0.75°C/帧 / 高运动 3.0°C/帧
 *      4. 最小跨度保护：1.5°C
 *
 * @par 灰度锐化
 *      采用 3×3 拉普拉斯锐化核，增益 2/5，
 *      差异阈值 [2, 24]，高亮保护 ≥220。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "thermal_visual.h"

#include <math.h>
#include <string.h>

#include "redpic1_thermal.h"

/* =========================================================================
 *  2. 内部宏定义 —— 基础参数
 * ======================================================================= */

#define THERMAL_VISUAL_SRC_ROWS                 24U         /**< 图像行数              */
#define THERMAL_VISUAL_SRC_COLS                 32U         /**< 图像列数              */
#define THERMAL_VISUAL_PIXEL_COUNT              768U        /**< 总像素数 (24×32)      */

/* =========================================================================
 *  3. 内部宏定义 —— 温度有效性范围
 * ======================================================================= */

#define THERMAL_VISUAL_VALID_TEMP_MIN_C         (-40.0f)    /**< 有效温度下限（°C）    */
#define THERMAL_VISUAL_VALID_TEMP_MAX_C         (300.0f)    /**< 有效温度上限（°C）    */
#define THERMAL_VISUAL_VALID_MIN_SPAN_C         (0.5f)      /**< 帧有效最小跨度（°C）  */

/* =========================================================================
 *  4. 内部宏定义 —— 显示窗口参数
 * ======================================================================= */

#define THERMAL_VISUAL_DISPLAY_WINDOW_MIN_SPAN_C        1.5f    /**< 最小跨度（°C）        */
#define THERMAL_VISUAL_DISPLAY_WINDOW_NORMAL_EMA_ALPHA  REDPIC1_THERMAL_STAGEP7_NORMAL_EMA_ALPHA  /**< 正常 EMA α */
#define THERMAL_VISUAL_DISPLAY_WINDOW_NORMAL_MAX_STEP_C REDPIC1_THERMAL_STAGEP7_NORMAL_MAX_STEP_C /**< 正常步长限制 */
#define THERMAL_VISUAL_DISPLAY_WINDOW_MOTION_EMA_ALPHA  0.45f   /**< 高运动 EMA α          */
#define THERMAL_VISUAL_DISPLAY_WINDOW_MOTION_MAX_STEP_C 3.0f    /**< 高运动步长限制（°C）  */
#define THERMAL_VISUAL_DISPLAY_WINDOW_HALF_SPAN_C       (THERMAL_VISUAL_DISPLAY_WINDOW_MIN_SPAN_C * 0.5f)

/* =========================================================================
 *  5. 内部宏定义 —— 运动检测参数
 * ======================================================================= */

#define THERMAL_VISUAL_HIGH_MOTION_DELTA_C          0.8f    /**< 运动检测阈值（°C）    */
#define THERMAL_VISUAL_HIGH_MOTION_STRONG_DELTA_C   2.2f    /**< 强运动检测阈值（°C）  */
#define THERMAL_VISUAL_HIGH_MOTION_PIXEL_THRESHOLD  48U     /**< 运动像素计数阈值      */

/* =========================================================================
 *  6. 内部宏定义 —— 灰度锐化参数
 * ======================================================================= */

#define THERMAL_ENABLE_GRAY_SHARPEN                 1       /**< 锐化功能开关          */
#define THERMAL_VISUAL_SHARPEN_NUM                  2       /**< 锐化增益分子          */
#define THERMAL_VISUAL_SHARPEN_DEN                  5       /**< 锐化增益分母          */
#define THERMAL_VISUAL_SHARPEN_MIN_DIFF             2       /**< 最小差异阈值          */
#define THERMAL_VISUAL_SHARPEN_MAX_DIFF             24      /**< 最大差异钳位          */
#define THERMAL_VISUAL_SHARPEN_HIGHLIGHT_GUARD      220     /**< 高亮保护阈值          */

/* =========================================================================
 *  7. 内部宏定义 —— 百分位窗口参数
 * ======================================================================= */

#define THERMAL_ENABLE_PERCENTILE_WINDOW            1       /**< 百分位窗口开关        */
#define THERMAL_VISUAL_PERCENTILE_LOW_PERMILLE      20U     /**< 低百分位（千分位）    */
#define THERMAL_VISUAL_PERCENTILE_HIGH_PERMILLE     995U    /**< 高百分位（千分位）    */
#define THERMAL_VISUAL_PERCENTILE_HEADROOM_RATIO    0.08f   /**< headroom 比例         */
#define THERMAL_VISUAL_PERCENTILE_HEADROOM_MIN_C    0.4f    /**< headroom 最小值（°C） */

/* =========================================================================
 *  8. 模块级静态变量
 * ======================================================================= */

/* 显示窗口状态 */
static float    s_display_min_temp     = 0.0f;     /**< 当前显示窗口下限      */
static float    s_display_max_temp     = 0.0f;     /**< 当前显示窗口上限      */
static uint8_t  s_display_window_valid = 0U;        /**< 显示窗口有效性标志    */

/* 滤波历史缓冲区（CCM RAM） */
static CCMRAM float s_previous_filtered_temp_frame[THERMAL_VISUAL_PIXEL_COUNT]; /**< 上一帧滤波结果 */
static CCMRAM float s_current_visual_temp_frame[THERMAL_VISUAL_PIXEL_COUNT];    /**< 当前帧视觉温度 */

/* 百分位排序缓冲区（CCM RAM） */
static CCMRAM float s_percentile_sort_buffer[THERMAL_VISUAL_PIXEL_COUNT];

/* 锐化源缓冲区（CCM RAM） */
static CCMRAM uint8_t s_gray_sharpen_source[THERMAL_VISUAL_PIXEL_COUNT];

static uint8_t s_filter_history_valid = 0U;         /**< 滤波历史有效性标志    */

/* =========================================================================
 *  9. 内部函数前向声明
 * ======================================================================= */

static uint8_t redpic1_thermal_visual_temp_in_range(float temp);

/* =========================================================================
 *  10. 内部函数实现 —— 显示窗口管理
 * ======================================================================= */

/** @brief  重置显示窗口状态 */
static void redpic1_thermal_visual_reset_display_window_state(void)
{
    s_display_min_temp     = 0.0f;
    s_display_max_temp     = 0.0f;
    s_display_window_valid = 0U;
}

/**
 * @brief  限制显示窗口的单步变化量
 * @param  current_value — 当前值
 * @param  target_value  — 目标值
 * @param  max_step_c    — 最大步长（°C）
 * @return 限制后的值
 */
static float redpic1_thermal_visual_limit_display_window_step(float current_value,
                                                              float target_value,
                                                              float max_step_c)
{
    float delta = target_value - current_value;

    if (max_step_c <= 0.0f)
    {
        return target_value;
    }

    if (delta > max_step_c)
    {
        delta = max_step_c;
    }
    else if (delta < -max_step_c)
    {
        delta = -max_step_c;
    }

    return current_value + delta;
}

/**
 * @brief  计算平滑后的显示窗口
 * @note   算法：百分位截断 → EMA 平滑 → 步长限制 → 最小跨度保护。
 * @param  target_min_temp    — 目标窗口下限
 * @param  target_max_temp    — 目标窗口上限
 * @param  high_motion_frame  — 是否为高运动帧
 * @param  out_display_min_temp — 输出：显示窗口下限
 * @param  out_display_max_temp — 输出：显示窗口上限
 */
static void redpic1_thermal_visual_get_display_window(float target_min_temp,
                                                      float target_max_temp,
                                                      uint8_t high_motion_frame,
                                                      float *out_display_min_temp,
                                                      float *out_display_max_temp)
{
    float ema_alpha  = THERMAL_VISUAL_DISPLAY_WINDOW_NORMAL_EMA_ALPHA;
    float max_step_c = THERMAL_VISUAL_DISPLAY_WINDOW_NORMAL_MAX_STEP_C;
    float center_temp = (target_min_temp + target_max_temp) * 0.5f;
    float half_span   = fmaxf((target_max_temp - target_min_temp) * 0.5f,
                              THERMAL_VISUAL_DISPLAY_WINDOW_HALF_SPAN_C);

    /* 确保最小跨度 */
    target_min_temp = center_temp - half_span;
    target_max_temp = center_temp + half_span;

    /* 高运动帧：使用更快的 EMA 和更大的步长 */
    if (high_motion_frame != 0U)
    {
        ema_alpha  = THERMAL_VISUAL_DISPLAY_WINDOW_MOTION_EMA_ALPHA;
        max_step_c = THERMAL_VISUAL_DISPLAY_WINDOW_MOTION_MAX_STEP_C;
    }

    if (s_display_window_valid == 0U)
    {
        /* 首次：直接采用目标值 */
        s_display_min_temp     = target_min_temp;
        s_display_max_temp     = target_max_temp;
        s_display_window_valid = 1U;
    }
    else
    {
        float ema_min_temp   = 0.0f;
        float ema_max_temp   = 0.0f;
        float cur_center     = 0.0f;
        float cur_half_span  = 0.0f;

        /* EMA 平滑 */
        ema_min_temp = s_display_min_temp +
                       ((target_min_temp - s_display_min_temp) * ema_alpha);
        ema_max_temp = s_display_max_temp +
                       ((target_max_temp - s_display_max_temp) * ema_alpha);

        /* 步长限制 */
        s_display_min_temp = redpic1_thermal_visual_limit_display_window_step(
            s_display_min_temp, ema_min_temp, max_step_c);
        s_display_max_temp = redpic1_thermal_visual_limit_display_window_step(
            s_display_max_temp, ema_max_temp, max_step_c);

        /* 最小跨度保护 */
        cur_center    = (s_display_min_temp + s_display_max_temp) * 0.5f;
        cur_half_span = fmaxf((s_display_max_temp - s_display_min_temp) * 0.5f,
                              THERMAL_VISUAL_DISPLAY_WINDOW_HALF_SPAN_C);
        s_display_min_temp = cur_center - cur_half_span;
        s_display_max_temp = cur_center + cur_half_span;

        /* 异常保护：窗口反转时重置 */
        if (s_display_max_temp <= s_display_min_temp)
        {
            s_display_min_temp = target_min_temp;
            s_display_max_temp = target_max_temp;
        }
    }

    *out_display_min_temp = s_display_min_temp;
    *out_display_max_temp = s_display_max_temp;
}

/* =========================================================================
 *  11. 内部函数实现 —— 百分位窗口计算
 * ======================================================================= */

/**
 * @brief  希尔排序（升序）
 * @param  values — 待排序数组
 * @param  count  — 数组长度
 */
static void redpic1_thermal_visual_shell_sort(float *values, uint16_t count)
{
    uint16_t gap = count / 2U;

    while (gap != 0U)
    {
        uint16_t i = 0U;

        for (i = gap; i < count; ++i)
        {
            float temp = values[i];
            uint16_t j = i;

            while (j >= gap && values[j - gap] > temp)
            {
                values[j] = values[j - gap];
                j = (uint16_t)(j - gap);
            }

            values[j] = temp;
        }

        gap = (gap == 1U) ? 0U : (uint16_t)(gap / 2U);
    }
}

/**
 * @brief  计算百分位温度窗口
 * @note   对有效温度排序后取 P2 ~ P99.5，添加 headroom。
 * @param  frame_data         — 帧温度数据
 * @param  raw_min_temp       — 原始最低温度（回退值）
 * @param  raw_max_temp       — 原始最高温度（回退值）
 * @param  out_window_min_temp — 输出：窗口下限
 * @param  out_window_max_temp — 输出：窗口上限
 */
static void redpic1_thermal_visual_get_percentile_window(const float *frame_data,
                                                         float raw_min_temp,
                                                         float raw_max_temp,
                                                         float *out_window_min_temp,
                                                         float *out_window_max_temp)
{
    uint16_t valid_count = 0U;
    uint16_t i = 0U;

    if (out_window_min_temp == 0 || out_window_max_temp == 0)
    {
        return;
    }

    /* 默认回退到原始极值 */
    *out_window_min_temp = raw_min_temp;
    *out_window_max_temp = raw_max_temp;

#if THERMAL_ENABLE_PERCENTILE_WINDOW
    if (frame_data == 0)
    {
        return;
    }

    /* 收集有效温度 */
    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        float temp = frame_data[i];

        if (redpic1_thermal_visual_temp_in_range(temp) == 0U)
        {
            continue;
        }

        s_percentile_sort_buffer[valid_count++] = temp;
    }

    /* 有效像素不足时回退 */
    if (valid_count < 16U)
    {
        return;
    }

    /* 排序并计算百分位 */
    redpic1_thermal_visual_shell_sort(s_percentile_sort_buffer, valid_count);

    {
        uint16_t low_index  = (uint16_t)((((uint32_t)(valid_count - 1U)) *
                                           THERMAL_VISUAL_PERCENTILE_LOW_PERMILLE) / 1000UL);
        uint16_t high_index = (uint16_t)((((uint32_t)(valid_count - 1U)) *
                                           THERMAL_VISUAL_PERCENTILE_HIGH_PERMILLE) / 1000UL);
        float percentile_min = s_percentile_sort_buffer[low_index];
        float percentile_max = s_percentile_sort_buffer[high_index];
        float span      = 0.0f;
        float headroom  = 0.0f;

        if (high_index <= low_index || percentile_max <= percentile_min)
        {
            return;
        }

        /* 计算 headroom */
        span     = percentile_max - percentile_min;
        headroom = span * THERMAL_VISUAL_PERCENTILE_HEADROOM_RATIO;
        if (headroom < THERMAL_VISUAL_PERCENTILE_HEADROOM_MIN_C)
        {
            headroom = THERMAL_VISUAL_PERCENTILE_HEADROOM_MIN_C;
        }

        *out_window_min_temp = percentile_min;
        *out_window_max_temp = percentile_max + headroom;
    }
#else
    (void)frame_data;
#endif
}

/* =========================================================================
 *  12. 内部函数实现 —— 帧间滤波
 * ======================================================================= */

/** @brief  重置滤波状态 */
static void redpic1_thermal_visual_reset_filter_state(void)
{
    s_filter_history_valid = 0U;
}

/**
 * @brief  采用原始帧数据作为滤波历史
 * @note   首帧调用时使用，避免滤波器冷启动问题。
 * @param  raw_frame_data — 原始帧温度数据
 */
static void redpic1_thermal_visual_adopt_raw_history(const float *raw_frame_data)
{
    uint16_t i = 0U;

    if (raw_frame_data == 0)
    {
        return;
    }

    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        s_current_visual_temp_frame[i]    = raw_frame_data[i];
        s_previous_filtered_temp_frame[i] = raw_frame_data[i];
    }

    s_filter_history_valid = 1U;
}

/**
 * @brief  获取滤波后的视觉帧
 * @note   自适应权重 EMA 滤波 + 高运动检测。
 * @param  raw_frame_data      — 原始帧温度数据
 * @param  out_high_motion_frame — 输出：高运动帧标志
 * @return 滤波后的帧数据指针
 */
static const float *redpic1_thermal_visual_get_visual_frame(
    const float *raw_frame_data, uint8_t *out_high_motion_frame)
{
    uint16_t i = 0U;
    uint16_t high_motion_pixel_count = 0U;
    float    max_abs_delta = 0.0f;

    if (out_high_motion_frame != 0)
    {
        *out_high_motion_frame = 0U;
    }

    if (raw_frame_data == 0)
    {
        return 0;
    }

    /* 首帧：直接采用原始数据 */
    if (s_filter_history_valid == 0U)
    {
        redpic1_thermal_visual_adopt_raw_history(raw_frame_data);
        return s_current_visual_temp_frame;
    }

    /* 逐像素自适应滤波 */
    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        float raw_temp     = raw_frame_data[i];
        float prev_temp    = s_previous_filtered_temp_frame[i];
        float delta        = raw_temp - prev_temp;
        float abs_delta    = delta;
        float current_weight = 1.0f;
        float filtered_temp  = 0.0f;

        if (abs_delta < 0.0f)
        {
            abs_delta = -abs_delta;
        }

        /* 追踪最大变化量 */
        if (abs_delta > max_abs_delta)
        {
            max_abs_delta = abs_delta;
        }

        /* 运动像素计数 */
        if (abs_delta >= THERMAL_VISUAL_HIGH_MOTION_DELTA_C)
        {
            high_motion_pixel_count++;
        }

        /* 自适应权重：小变化强平滑，大变化无滤波 */
        if (abs_delta <= 0.20f)
        {
            current_weight = 0.40f;
        }
        else if (abs_delta < 1.00f)
        {
            current_weight = 0.40f + (((abs_delta - 0.20f) / 0.80f) * 0.60f);
        }

        /* EMA 滤波 */
        filtered_temp = prev_temp + ((raw_temp - prev_temp) * current_weight);
        s_current_visual_temp_frame[i]    = filtered_temp;
        s_previous_filtered_temp_frame[i] = filtered_temp;
    }

    /* 高运动帧判定 */
    if (out_high_motion_frame != 0)
    {
        if (high_motion_pixel_count >= THERMAL_VISUAL_HIGH_MOTION_PIXEL_THRESHOLD ||
            max_abs_delta >= THERMAL_VISUAL_HIGH_MOTION_STRONG_DELTA_C)
        {
            *out_high_motion_frame = 1U;
        }
    }

    return s_current_visual_temp_frame;
}

/* =========================================================================
 *  13. 内部函数实现 —— 温度有效性与灰度工具
 * ======================================================================= */

/**
 * @brief  判断温度值是否在有效范围内
 * @param  temp — 温度值（°C）
 * @retval 1 — 有效；0 — 无效（含 NaN）
 */
static uint8_t redpic1_thermal_visual_temp_in_range(float temp)
{
    /* NaN 检测：NaN != NaN 为真 */
    if (temp != temp)
    {
        return 0U;
    }

    if (temp < THERMAL_VISUAL_VALID_TEMP_MIN_C)
    {
        return 0U;
    }
    if (temp > THERMAL_VISUAL_VALID_TEMP_MAX_C)
    {
        return 0U;
    }

    return 1U;
}

/**
 * @brief  将 int32 钳位到 uint8 范围
 * @param  value — 输入值
 * @return 钳位后的 uint8 值
 */
static uint8_t redpic1_thermal_visual_clamp_u8_int(int32_t value)
{
    if (value < 0)
    {
        return 0U;
    }
    if (value > 255)
    {
        return 255U;
    }

    return (uint8_t)value;
}

/**
 * @brief  计算灰度帧中的像素索引（列优先）
 * @param  row — 行号
 * @param  col — 列号
 * @return 线性索引
 */
static uint16_t redpic1_thermal_visual_gray_index(uint16_t row, uint16_t col)
{
    return (uint16_t)(col * THERMAL_VISUAL_SRC_ROWS + row);
}

/* =========================================================================
 *  14. 内部函数实现 —— 灰度帧锐化
 * ======================================================================= */

/**
 * @brief  对灰度帧执行拉普拉斯锐化
 * @note   3×3 锐化核，增益 2/5，差异阈值 [2, 24]，高亮保护 ≥220。
 * @param  gray_frame — 灰度帧数据（768 字节，列优先排列）
 */
static void redpic1_thermal_visual_sharpen_gray_frame(uint8_t *gray_frame)
{
#if THERMAL_ENABLE_GRAY_SHARPEN
    uint16_t row = 0U;
    uint16_t col = 0U;

    if (gray_frame == 0)
    {
        return;
    }

    /* 备份原始灰度数据 */
    memcpy(s_gray_sharpen_source, gray_frame, sizeof(s_gray_sharpen_source));

    /* 遍历内部像素（跳过边界） */
    for (row = 1U; row < (THERMAL_VISUAL_SRC_ROWS - 1U); ++row)
    {
        for (col = 1U; col < (THERMAL_VISUAL_SRC_COLS - 1U); ++col)
        {
            uint16_t idx   = redpic1_thermal_visual_gray_index(row, col);
            int32_t  center = s_gray_sharpen_source[idx];
            int32_t  up     = s_gray_sharpen_source[redpic1_thermal_visual_gray_index((uint16_t)(row - 1U), col)];
            int32_t  down   = s_gray_sharpen_source[redpic1_thermal_visual_gray_index((uint16_t)(row + 1U), col)];
            int32_t  left   = s_gray_sharpen_source[redpic1_thermal_visual_gray_index(row, (uint16_t)(col - 1U))];
            int32_t  right  = s_gray_sharpen_source[redpic1_thermal_visual_gray_index(row, (uint16_t)(col + 1U))];
            int32_t  avg    = (up + down + left + right) / 4;
            int32_t  diff   = center - avg;
            int32_t  abs_diff = (diff < 0) ? -diff : diff;
            int32_t  sharp  = 0;

            /* 差异过小：跳过 */
            if (abs_diff < THERMAL_VISUAL_SHARPEN_MIN_DIFF)
            {
                continue;
            }

            /* 高亮保护：防止过曝区域过度锐化 */
            if (center >= THERMAL_VISUAL_SHARPEN_HIGHLIGHT_GUARD && diff > 0)
            {
                continue;
            }

            /* 差异钳位 */
            if (diff > THERMAL_VISUAL_SHARPEN_MAX_DIFF)
            {
                diff = THERMAL_VISUAL_SHARPEN_MAX_DIFF;
            }
            else if (diff < -THERMAL_VISUAL_SHARPEN_MAX_DIFF)
            {
                diff = -THERMAL_VISUAL_SHARPEN_MAX_DIFF;
            }

            /* 应用锐化 */
            sharp = center + ((diff * THERMAL_VISUAL_SHARPEN_NUM) / THERMAL_VISUAL_SHARPEN_DEN);
            gray_frame[idx] = redpic1_thermal_visual_clamp_u8_int(sharp);
        }
    }
#else
    (void)gray_frame;
#endif
}

/* =========================================================================
 *  15. 公共接口实现 —— 初始化与重置
 * ======================================================================= */

/**
 * @brief  初始化可视化处理模块
 * @param  ops — 回调函数集指针（当前未使用）
 */
void redpic1_thermal_visual_init(const redpic1_thermal_visual_ops_t *ops)
{
    (void)ops;
    redpic1_thermal_visual_reset_history();
}

/**
 * @brief  重置可视化历史状态
 * @note   清除显示窗口和滤波历史。
 */
void redpic1_thermal_visual_reset_history(void)
{
    redpic1_thermal_visual_reset_display_window_state();
    redpic1_thermal_visual_reset_filter_state();
}

/**
 * @brief  失效可视化历史
 * @note   当前为空实现（历史分支已移除）。
 */
void redpic1_thermal_visual_invalidate_history(void)
{
    /* 采集间隔失效逻辑已移除 */
}

/**
 * @brief  检查采集间隔是否超过阈值
 * @param  capture_tick_ms — 采集时间戳
 * @retval 0 — 始终返回 0（当前为空实现）
 */
uint8_t redpic1_thermal_visual_capture_gap_exceeded(uint32_t capture_tick_ms)
{
    (void)capture_tick_ms;
    return 0U;
}

/**
 * @brief  记录采集成功事件
 * @param  capture_tick_ms — 采集时间戳
 */
void redpic1_thermal_visual_note_capture_success(uint32_t capture_tick_ms)
{
    (void)capture_tick_ms;
}

/* =========================================================================
 *  16. 公共接口实现 —— 灰度源帧获取
 * ======================================================================= */

/**
 * @brief  获取灰度生成的源帧（含运动检测）
 * @param  raw_frame_data        — 原始帧温度数据
 * @param  out_high_motion_frame — 输出：高运动帧标志
 * @return 滤波后的帧数据指针；滤波失败时回退到原始数据
 */
const float *redpic1_thermal_visual_get_gray_source_frame(
    const float *raw_frame_data, uint8_t *out_high_motion_frame)
{
    const float *visual_frame = redpic1_thermal_visual_get_visual_frame(
        raw_frame_data, out_high_motion_frame);

    return (visual_frame != 0) ? visual_frame : raw_frame_data;
}

/* =========================================================================
 *  17. 公共接口实现 —— 灰度帧生成
 * ======================================================================= */

/**
 * @brief  准备灰度帧数据
 * @note   流程：
 *         1. 计算原始温度极值
 *         2. 百分位窗口截断
 *         3. EMA 平滑显示窗口
 *         4. 线性映射温度→灰度
 *         5. 拉普拉斯锐化
 * @param  raw_frame_data     — 原始帧温度数据（用于极值计算）
 * @param  display_frame_data — 显示帧温度数据（用于灰度映射）
 * @param  high_motion_frame  — 高运动帧标志
 * @param  gray_frame         — 输出：灰度帧（768 字节）
 * @param  out_min_temp       — 输出：原始最低温度（可选）
 * @param  out_max_temp       — 输出：原始最高温度（可选）
 */
void redpic1_thermal_visual_prepare_gray_frame(const float *raw_frame_data,
                                               const float *display_frame_data,
                                               uint8_t high_motion_frame,
                                               uint8_t *gray_frame,
                                               float *out_min_temp,
                                               float *out_max_temp)
{
    float raw_min_temp          = 300.0f;
    float raw_max_temp          = -40.0f;
    float target_window_min_temp = 0.0f;
    float target_window_max_temp = 0.0f;
    float display_min_temp      = 0.0f;
    float display_max_temp      = 0.0f;
    float scale                 = 0.0f;
    uint16_t i       = 0U;
    uint16_t src_row = 0U;

    if (raw_frame_data == 0 || display_frame_data == 0 || gray_frame == 0)
    {
        return;
    }

    /* 步骤 1：计算原始温度极值 */
    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        float temp = raw_frame_data[i];

        if (temp > raw_max_temp)
        {
            raw_max_temp = temp;
        }
        if (temp < raw_min_temp)
        {
            raw_min_temp = temp;
        }
    }

    /* 温度范围无效：输出全黑帧 */
    if (raw_max_temp <= raw_min_temp)
    {
        for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
        {
            gray_frame[i] = 0U;
        }
        if (out_min_temp != 0)
        {
            *out_min_temp = raw_min_temp;
        }
        if (out_max_temp != 0)
        {
            *out_max_temp = raw_max_temp;
        }
        return;
    }

    /* 步骤 2：百分位窗口截断 */
    redpic1_thermal_visual_get_percentile_window(display_frame_data,
                                                 raw_min_temp,
                                                 raw_max_temp,
                                                 &target_window_min_temp,
                                                 &target_window_max_temp);

    /* 步骤 3：EMA 平滑显示窗口 */
    redpic1_thermal_visual_get_display_window(target_window_min_temp,
                                              target_window_max_temp,
                                              high_motion_frame,
                                              &display_min_temp,
                                              &display_max_temp);

    /* 窗口异常保护 */
    if (display_max_temp <= display_min_temp)
    {
        display_min_temp = raw_min_temp;
        display_max_temp = raw_max_temp;
    }

    /* 步骤 4：线性映射温度→灰度 */
    scale = 255.0f / (display_max_temp - display_min_temp);

    for (src_row = 0U; src_row < THERMAL_VISUAL_SRC_ROWS; ++src_row)
    {
        const float *src = display_frame_data +
                           ((uint32_t)src_row * THERMAL_VISUAL_SRC_COLS);
        uint8_t *dst = gray_frame + src_row;
        uint16_t src_col = 0U;

        for (src_col = 0U; src_col < THERMAL_VISUAL_SRC_COLS; ++src_col)
        {
            int32_t gray_value = (int32_t)(((*src++) - display_min_temp) * scale);

            if (gray_value < 0)
            {
                gray_value = 0;
            }
            else if (gray_value > 255)
            {
                gray_value = 255;
            }

            *dst = (uint8_t)gray_value;
            dst += THERMAL_VISUAL_SRC_ROWS;
        }
    }

    /* 步骤 5：拉普拉斯锐化 */
    redpic1_thermal_visual_sharpen_gray_frame(gray_frame);

    /* 输出极值 */
    if (out_min_temp != 0)
    {
        *out_min_temp = raw_min_temp;
    }
    if (out_max_temp != 0)
    {
        *out_max_temp = raw_max_temp;
    }
}

/* =========================================================================
 *  18. 公共接口实现 —— 温度查询与校验
 * ======================================================================= */

/**
 * @brief  获取图像中心点温度
 * @param  frame_data — 帧温度数据
 * @return 中心点 (12,16) 温度值；指针为空时返回 0.0f
 */
float redpic1_thermal_visual_center_temp(const float *frame_data)
{
    uint16_t center_row = THERMAL_VISUAL_SRC_ROWS / 2U;
    uint16_t center_col = THERMAL_VISUAL_SRC_COLS / 2U;

    if (frame_data == 0)
    {
        return 0.0f;
    }

    return frame_data[(center_row * THERMAL_VISUAL_SRC_COLS) + center_col];
}

/**
 * @brief  校验帧数据中所有像素是否在有效范围内
 * @param  frame_data — 帧温度数据
 * @retval 1 — 全部有效；0 — 存在无效像素
 */
uint8_t redpic1_thermal_visual_frame_data_is_valid(const float *frame_data)
{
    uint16_t i = 0U;

    if (frame_data == 0)
    {
        return 0U;
    }

    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        if (redpic1_thermal_visual_temp_in_range(frame_data[i]) == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

/**
 * @brief  校验帧温度范围的有效性
 * @param  min_temp    — 最低温度
 * @param  max_temp    — 最高温度
 * @param  center_temp — 中心温度
 * @retval 1 — 有效；0 — 无效
 */
uint8_t redpic1_thermal_visual_frame_is_valid(float min_temp,
                                              float max_temp,
                                              float center_temp)
{
    /* 温度范围检查 */
    if (redpic1_thermal_visual_temp_in_range(min_temp) == 0U ||
        redpic1_thermal_visual_temp_in_range(max_temp) == 0U ||
        redpic1_thermal_visual_temp_in_range(center_temp) == 0U)
    {
        return 0U;
    }

    /* 极值关系检查 */
    if (max_temp < min_temp)
    {
        return 0U;
    }

    /* 最小跨度检查 */
    if ((max_temp - min_temp) < THERMAL_VISUAL_VALID_MIN_SPAN_C)
    {
        return 0U;
    }

    return 1U;
}

/**
 * @brief  检查灰度帧是否有足够的对比度
 * @param  gray_frame — 灰度帧数据
 * @retval 1 — 有对比度；0 — 全黑或全白
 */
uint8_t redpic1_thermal_visual_gray_frame_has_contrast(const uint8_t *gray_frame)
{
    uint8_t  gray_min = 255U;
    uint8_t  gray_max = 0U;
    uint16_t i = 0U;

    if (gray_frame == 0)
    {
        return 0U;
    }

    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        if (gray_frame[i] < gray_min)
        {
            gray_min = gray_frame[i];
        }
        if (gray_frame[i] > gray_max)
        {
            gray_max = gray_frame[i];
        }
    }

    return (gray_max > gray_min) ? 1U : 0U;
}
