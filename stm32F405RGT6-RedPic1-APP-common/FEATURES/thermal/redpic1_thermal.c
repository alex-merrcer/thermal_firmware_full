#include "redpic1_thermal.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app_display_runtime.h"
#include "app_perf_baseline.h"
#include "delay.h"
#include "key.h"
#include "lcd.h"
#include "lcd_utf8.h"
#include "lcd_init.h"
#include "power_manager.h"
#include "MLX90640.h"
#include "lcd_dma.h"
#include "thermal_capture.h"
#include "thermal_cloud.h"
#include "thermal_frame_slot.h"
#include "thermal_pair.h"
#include "thermal_visual.h"

/**
 * @file redpic1_thermal.c
 * @brief 热成像采集、灰度生成与异步显示提交模块。
 * 
 * @details 
 * - 核心架构：采用“三槽位异步 Present 路径”解耦传感器采集与屏幕刷新。
 * - 模块职责：
 *   1. 传感器 I2C 采集与总线退避/恢复机制 (Backoff & Restore)
 *   2. 帧槽位所有权状态机 (FREE -> WRITING -> READY -> INFLIGHT -> FRONT)
 *   3. Display Runtime 异步回调后的帧生命周期管理
 *   4. 热成像页运行时叠加条 (Overlay) 与本地按键控制
 * 
 * @note 当前版本已固化异步送显路径，移除历史阶段回滚分支以提升确定性。
 */

/* ========================================================================= */
/*  宏定义与常量配置 (Constants & Configuration)                             */
/* ========================================================================= */
/* 基础参数 */
#define REDPIC1_THERMAL_ACTIVE_REFRESH_RATE        RefreshRate
#define REDPIC1_THERMAL_IDLE_REFRESH_RATE              FPS1HZ       ///< 休眠/低功耗模式帧率
#define REDPIC1_THERMAL_SRC_ROWS                       24U          ///< MLX90640 原始分辨率行数
#define REDPIC1_THERMAL_SRC_COLS                       32U          ///< MLX90640 原始分辨率列数
#define REDPIC1_THERMAL_PIXEL_COUNT                    768U         ///< 总像素数 (24*32)

/* 显示与 UI 布局参数 */
#define REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT             20U          ///< 底部状态栏高度
#define REDPIC1_THERMAL_VIEWPORT_HEIGHT                (LCD_H - REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT) ///< 成像可视区域高度
#define REDPIC1_THERMAL_OVERLAY_BAR_TEXT_Y_OFFSET      2U           ///< 状态栏文本 Y 轴偏移
#define REDPIC1_THERMAL_OVERLAY_BAR_TEXT_X             4U           ///< 状态栏文本 X 轴起始位置
#define REDPIC1_THERMAL_OVERLAY_CROSS_HALF_SIZE        6U           ///< 中心十字准星半宽
#define REDPIC1_THERMAL_OVERLAY_BAR_REFRESH_MS         250UL        ///< 底部状态栏刷新防抖周期 (ms)
#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_TEXT      (1U << 0)
#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PALETTE   (1U << 1)
#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PAUSE     (1U << 2)
#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE     (1U << 3)
#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_VISIBLE   (1U << 4)

/* ========================================================================= */
/*  静态全局变量 (Static Globals)                                            */
/* ========================================================================= */

/* [显示控制与 UI 区] 运行时状态与叠加层缓存 */
static uint8_t  s_display_paused = 0U;                                         ///< 暂停送显标志 (仅本模块可见)
static uint8_t  s_runEnabled = 1U;                                             ///< 模块总使能开关
static uint8_t  s_refreshRate = REDPIC1_THERMAL_ACTIVE_REFRESH_RATE;           ///< 当前硬件帧率配置
static uint8_t  s_overlayHold = 0U;                                            ///< UI 叠加层持有标志 (持有时禁止新帧提交)
static uint8_t  s_colorMode = 0U;                                              ///< 调色板模式 (0~4)
static uint8_t  s_diag_pattern_ready = 0U;                                     ///< 诊断测试图案就绪标志
static uint8_t  s_runtime_overlay_visible = 1U;                                ///< 底部状态栏可见性开关
static char     s_overlay_bar_last_line[64];                                   ///< 上次实际绘制到屏幕的文本缓存
static char     s_overlay_bar_pending_line[64];                                ///< 待刷新的新文本缓存
static uint32_t s_overlay_bar_last_refresh_ms = 0U;                            ///< 上次状态栏刷新时间戳
static uint32_t s_key2_ignore_until_ms = 0U;                                   ///< 进页后短时间忽略 KEY2，避免入页按键尾波
static uint8_t  s_overlay_bar_last_visible = 0U;                               ///< 上次状态栏是否可见
static uint8_t  s_overlay_bar_last_line_valid = 0U;                            ///< 上次缓存文本是否有效
static uint8_t  s_overlay_bar_pending_dirty = 1U;                              ///< 待刷新文本是否变更 (脏标志)
static uint8_t  s_overlay_bar_dirty_reason_mask = REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE; ///< 当前底栏脏原因掩码

static CCMRAM uint8_t s_diag_pattern_frame[REDPIC1_THERMAL_PIXEL_COUNT];       ///< 诊断测试图案灰度数据 (CCM RAM)

static uint8_t redpic1_thermal_present_gray_frame(const uint8_t *gray_frame);
/* ========================================================================= */
/*  基础辅助函数 (Core Helpers)                                              */
/* ========================================================================= */


/**
 * @brief 检查 FreeRTOS 调度器是否处于运行状态。
 * @return 1U 表示调度器运行中，0U 表示未启动或已挂起。
 * @note 用于临界区保护的安全判断，避免在调度器未启动时调用 taskENTER_CRITICAL。
 */
static uint8_t redpic1_thermal_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}


/**
 * @brief 安全进入临界区 (仅当调度器运行时生效)。
 * @details 保护多任务共享的槽位索引与状态标志，防止数据竞争。
 */
static void redpic1_thermal_enter_critical(void)
{
    if (redpic1_thermal_scheduler_running() != 0U)
    {
        taskENTER_CRITICAL();
    }
}

/**
 * @brief 安全退出临界区。
 */
static void redpic1_thermal_exit_critical(void)
{
    if (redpic1_thermal_scheduler_running() != 0U)
    {
        taskEXIT_CRITICAL();
    }
}

static uint32_t redpic1_thermal_frame_slot_tick_ms(void)
{
    return power_manager_get_tick_ms();
}

static uint8_t redpic1_thermal_frame_slot_run_enabled(void)
{
    return s_runEnabled;
}

static uint8_t redpic1_thermal_frame_slot_display_paused(void)
{
    return s_display_paused;
}

static uint8_t redpic1_thermal_frame_slot_overlay_hold(void)
{
    return s_overlayHold;
}

/**
 * @brief 格式化温度值为带一位小数的字符串。
 * @param buffer      输出缓冲区
 * @param buffer_len  缓冲区长度
 * @param temp        待格式化的温度值 (℃)
 * @param has_value   数据有效性标志 (0 时显示 "--.-")
 * @note 采用定点数思想避免浮点 snprintf 的潜在精度/体积问题，适合资源受限 MCU。
 */
static void redpic1_thermal_format_overlay_temp(char *buffer,uint16_t buffer_len,float temp,uint8_t has_value)
{
    int32_t scaled = 0;
    int32_t whole = 0;
    int32_t frac = 0;

    if (buffer == 0 || buffer_len == 0U)
    {
        return;
    }

    if (has_value == 0U)
    {
        snprintf(buffer, buffer_len, "%s", "--.-");
        return;
    }

    scaled = (temp >= 0.0f) ?
             (int32_t)(temp * 10.0f + 0.5f) :
             (int32_t)(temp * 10.0f - 0.5f);
    whole = scaled / 10;
    frac = scaled % 10;
    if (frac < 0)
    {
        frac = -frac;
    }

    snprintf(buffer, buffer_len, "%ld.%ld", (long)whole, (long)frac);
}

/**
 * @brief 构建底部状态栏显示文本。
 * @param line_text     输出缓冲区
 * @param line_text_len 缓冲区长度
 * @details 从性能基线模块获取 FPS 与极值温度，拼接为 UTF-8 编码字符串。
 */
static void redpic1_thermal_build_bottom_bar_line(char *line_text, uint16_t line_text_len)
{
    app_perf_baseline_snapshot_t snapshot;
    char min_text[12];
    char max_text[12];
    char center_text[12];
    uint8_t has_value = 0U;

    if (line_text == 0 || line_text_len == 0U)
    {
        return;
    }

    app_perf_baseline_get_snapshot(&snapshot);
    has_value = (snapshot.thermal_capture_frames != 0U) ? 1U : 0U;

    redpic1_thermal_format_overlay_temp(min_text,sizeof(min_text),snapshot.latest_min_temp,has_value);
		
    redpic1_thermal_format_overlay_temp(max_text,sizeof(max_text),snapshot.latest_max_temp,has_value);
		
    redpic1_thermal_format_overlay_temp(center_text,sizeof(center_text),snapshot.latest_center_temp,has_value);

    snprintf(line_text,
             line_text_len,"FPS:%lu  "
             "\xE6\x9C\x80\xE4\xBD\x8E:%s  "
             "\xE6\x9C\x80\xE9\xAB\x98:%s  "
             "\xE4\xB8\xAD\xE5\xBF\x83:%s",
             (unsigned long)snapshot.thermal_display_fps,min_text,max_text,center_text);
}

/**
 * @brief 在 LCD 底部绘制状态栏文本。
 * @param line_text 待绘制的 UTF-8 字符串
 */
static void redpic1_thermal_draw_bottom_bar_line(const char *line_text)
{
    uint16_t bar_top = 0U;

    if (line_text == 0)
    {
        return;
    }

    if (LCD_H > REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT)
    {
        bar_top = (uint16_t)(LCD_H - REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT);
    }

    LCD_Fill(0U, bar_top, (uint16_t)(LCD_W - 1U), (uint16_t)(LCD_H - 1U), BLACK);
    if (bar_top > 0U)
    {
        LCD_DrawLine(0U,
                     (uint16_t)(bar_top - 1U),
                     (uint16_t)(LCD_W - 1U),
                     (uint16_t)(bar_top - 1U),
                     WHITE);
    }
    LCD_ShowUTF8String(REDPIC1_THERMAL_OVERLAY_BAR_TEXT_X,
                       (uint16_t)(bar_top + REDPIC1_THERMAL_OVERLAY_BAR_TEXT_Y_OFFSET),
                       line_text,
                       YELLOW,
                       BLACK,
                       16,
                       0);
}

/**
 * @brief 清除底部状态栏 (恢复为全黑)。
 */
static void redpic1_thermal_clear_bottom_bar(void)
{
    uint16_t bar_top = 0U;
    uint16_t clear_top = 0U;

    if (LCD_H > REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT)
    {
        bar_top = (uint16_t)(LCD_H - REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT);
    }

    clear_top = (bar_top > 0U) ? (uint16_t)(bar_top - 1U) : bar_top;
    LCD_Fill(0U, clear_top, (uint16_t)(LCD_W - 1U), (uint16_t)(LCD_H - 1U), BLACK);
}

static void redpic1_thermal_mark_bottom_bar_dirty(uint8_t reason_mask)
{
    s_overlay_bar_pending_dirty = 1U;
    s_overlay_bar_dirty_reason_mask = (uint8_t)(s_overlay_bar_dirty_reason_mask | reason_mask);
}

static void redpic1_thermal_clear_bottom_bar_dirty(void)
{
    s_overlay_bar_pending_dirty = 0U;
    s_overlay_bar_dirty_reason_mask = 0U;
}

/**
 * @brief 重置状态栏缓存状态，强制下次刷新时重绘。
 */
static void redpic1_thermal_reset_bottom_bar_cache(void)
{
    s_overlay_bar_last_line[0] = '\0';
    s_overlay_bar_pending_line[0] = '\0';
    s_overlay_bar_last_refresh_ms = 0U;
    s_overlay_bar_last_visible = 0U;
    s_overlay_bar_last_line_valid = 0U;
    s_overlay_bar_dirty_reason_mask = 0U;
    redpic1_thermal_mark_bottom_bar_dirty((uint8_t)(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE |
                                                     REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_VISIBLE));
}

/**
 * @brief 获取图像中心点温度。
 * @param frame_data 原始温度数组指针
 * @return 中心点 (12,16) 的温度值。若指针为空返回 0.0f。
 */
static float redpic1_thermal_center_temp(const float *frame_data)
{
    return redpic1_thermal_visual_center_temp(frame_data);
}

static void redpic1_thermal_reset_processing_history(void)
{
    redpic1_thermal_visual_reset_history();
    redpic1_thermal_pair_reset();
}

static void redpic1_thermal_stage6l3_invalidate_history(void)
{
    redpic1_thermal_visual_invalidate_history();
    redpic1_thermal_pair_reset();
}

static uint8_t redpic1_thermal_stage6l3_capture_gap_exceeded(uint32_t capture_tick_ms)
{
    return redpic1_thermal_visual_capture_gap_exceeded(capture_tick_ms);
}

static uint32_t redpic1_thermal_refresh_rate_to_period_ms(uint8_t refresh_rate)
{
    switch (refresh_rate)
    {
    case FPS1HZ:
        return 1000UL;
    case FPS2HZ:
        return 500UL;
    case FPS4HZ:
        return 250UL;
    case FPS8HZ:
        return 125UL;
    case FPS16HZ:
        return 63UL;
    case FPS32HZ:
        return 32UL;
    default:
        return 63UL;
    }
}

static void redpic1_thermal_apply_refresh_rate_internal(uint8_t refresh_rate, uint8_t force_write)
{
    if (force_write == 0U && s_refreshRate == refresh_rate)
    {
        return;
    }

    if (MLX90640_SetRefreshRate(MLX90640_ADDR, refresh_rate) == 0)
    {
        s_refreshRate = refresh_rate;
    }
}

static void redpic1_thermal_apply_refresh_rate(uint8_t refresh_rate)
{
    redpic1_thermal_apply_refresh_rate_internal(refresh_rate, 0U);
}

static uint8_t redpic1_thermal_capture_get_refresh_rate(void)
{
    return s_refreshRate;
}

static void redpic1_thermal_capture_apply_refresh_rate(uint8_t refresh_rate, uint8_t force_write)
{
    redpic1_thermal_apply_refresh_rate_internal(refresh_rate, force_write);
}

static void redpic1_thermal_capture_invalidate_history(void)
{
    redpic1_thermal_stage6l3_invalidate_history();
}

/* 构造诊断测试图案。
 * 仅在诊断模式下送显，用于验证 display runtime 与 DMA 路径是否正常。 */
static void redpic1_thermal_build_diag_pattern(void)
{
    uint16_t row = 0U;

    for (row = 0U; row < REDPIC1_THERMAL_SRC_COLS; ++row)
    {
        uint16_t col = 0U;

        for (col = 0U; col < REDPIC1_THERMAL_SRC_ROWS; ++col)
        {
            uint16_t index = (uint16_t)(row * REDPIC1_THERMAL_SRC_ROWS + col);
            uint8_t gray = (uint8_t)((col * 255U) / (REDPIC1_THERMAL_SRC_ROWS - 1U));

            if ((row & 0x04U) != 0U)
            {
                gray = (uint8_t)(255U - gray);
            }

            if (row == (REDPIC1_THERMAL_SRC_COLS / 2U) ||
                col == (REDPIC1_THERMAL_SRC_ROWS / 2U))
            {
                gray = 255U;
            }

            if (((row + col) & 0x07U) == 0U)
            {
                gray = 32U;
            }

            s_diag_pattern_frame[index] = gray;
        }
    }

    s_diag_pattern_ready = 1U;
}

/* 通过 display runtime 提交一帧灰度图。
 * 该入口只负责当前 front 帧的送显，暂停、叠加占用时直接拒绝提交。 */
static uint8_t redpic1_thermal_present_gray_frame(const uint8_t *gray_frame)
{
    uint8_t ok = 0U;

    if (gray_frame == 0 ||
        s_runEnabled == 0U ||
        s_display_paused != 0U ||
        s_overlayHold != 0U)
    {
        return 0U;
    }

    power_manager_acquire_lock(POWER_LOCK_DISPLAY_DMA);
    ok = app_display_runtime_present_thermal_frame((uint8_t *)gray_frame);
    power_manager_release_lock(POWER_LOCK_DISPLAY_DMA);
    return ok;
}

static uint32_t redpic1_thermal_get_expired_frame_threshold_ms(void)
{
    uint32_t active_period_ms = redpic1_thermal_refresh_rate_to_period_ms(s_refreshRate);
    uint32_t threshold_ms = active_period_ms * 2U;

    if (threshold_ms < REDPIC1_THERMAL_DROP_EXPIRED_FRAME_MIN_MS)
    {
        threshold_ms = REDPIC1_THERMAL_DROP_EXPIRED_FRAME_MIN_MS;
    }

    return threshold_ms;
}

static redpic1_thermal_frame_slot_t *redpic1_thermal_get_back_slot(void)
{
    return redpic1_thermal_frame_slot_acquire_back();
}

static void redpic1_thermal_release_back_slot(redpic1_thermal_frame_slot_t *slot)
{
    redpic1_thermal_frame_slot_release_back(slot);
}

static void redpic1_thermal_publish_back_slot(redpic1_thermal_frame_slot_t *slot)
{
    redpic1_thermal_frame_slot_publish_back(slot);
}

static void redpic1_thermal_present_front_slot(void)
{
    redpic1_thermal_frame_slot_present_front();
}

static void redpic1_thermal_cancel_pending_present_and_clear_submit(void)
{
    redpic1_thermal_frame_slot_cancel_pending_present();
}

static void redpic1_thermal_drop_non_inflight_slots(void)
{
    redpic1_thermal_frame_slot_drop_non_inflight();
    redpic1_thermal_capture_reset();
    redpic1_thermal_reset_processing_history();
}

/* 完整重置所有槽位与运行时缓存。
 * 初始化阶段调用该函数，确保 front/ready/inflight 和显示窗口/滤波历史全部回到初始状态。 */
static void redpic1_thermal_reset_slots(void)
{
    redpic1_thermal_frame_slot_reset();
    redpic1_thermal_capture_reset();
    redpic1_thermal_reset_processing_history();
}

void redpic1_thermal_init(void)
{
    redpic1_thermal_capture_ops_t capture_ops;
    redpic1_thermal_frame_slot_ops_t slot_ops;
    redpic1_thermal_visual_ops_t visual_ops;

    while (mlx90640_init() != 0)
    {
        delay_ms(200);
    }

    s_colorMode = 3U;
    s_runEnabled = 1U;
    s_refreshRate = REDPIC1_THERMAL_ACTIVE_REFRESH_RATE;
    s_overlayHold = 0U;
    s_display_paused = 0U;
    s_runtime_overlay_visible = 1U;
    redpic1_thermal_reset_bottom_bar_cache();
    set_color_mode(s_colorMode);
    redpic1_thermal_apply_refresh_rate_internal(s_refreshRate, 1U);

    memset(&capture_ops, 0, sizeof(capture_ops));
    capture_ops.get_refresh_rate = redpic1_thermal_capture_get_refresh_rate;
    capture_ops.apply_refresh_rate = redpic1_thermal_capture_apply_refresh_rate;
    capture_ops.invalidate_history = redpic1_thermal_capture_invalidate_history;
    redpic1_thermal_capture_init(&capture_ops);

    redpic1_thermal_cloud_init();

    memset(&visual_ops, 0, sizeof(visual_ops));
    visual_ops.get_active_period_ms = redpic1_thermal_get_active_period_ms;
    redpic1_thermal_visual_init(&visual_ops);

    memset(&slot_ops, 0, sizeof(slot_ops));
    slot_ops.enter_critical = redpic1_thermal_enter_critical;
    slot_ops.exit_critical = redpic1_thermal_exit_critical;
    slot_ops.get_tick_ms = redpic1_thermal_frame_slot_tick_ms;
    slot_ops.get_expired_frame_threshold_ms = redpic1_thermal_get_expired_frame_threshold_ms;
    slot_ops.present_gray_frame = redpic1_thermal_present_gray_frame;
    slot_ops.run_enabled = redpic1_thermal_frame_slot_run_enabled;
    slot_ops.display_paused = redpic1_thermal_frame_slot_display_paused;
    slot_ops.overlay_hold = redpic1_thermal_frame_slot_overlay_hold;
    redpic1_thermal_frame_slot_init(&slot_ops);

    redpic1_thermal_reset_slots();
    redpic1_thermal_build_diag_pattern();
}

void redpic1_thermal_bind_display_runtime(void)
{
    redpic1_thermal_frame_slot_bind_display_runtime();
}

uint32_t redpic1_thermal_get_active_period_ms(void)
{
    return redpic1_thermal_refresh_rate_to_period_ms(s_refreshRate);
}

/* thermal task 单步执行入口。
 * 固定按“backoff 判定 -> 可选 ready 预检查 -> 取帧 -> 灰度生成 -> 校验 -> 发布/提交”顺序运行。 */
/* thermal task 单步执行入口。
 * 固定按“backoff 判定 -> 可选 ready 预检查 -> 取帧 -> 灰度生成 -> 校验 -> 发布/提交”顺序运行。 */
void redpic1_thermal_step(void)
{
    uint32_t step_start_cycle = app_perf_baseline_cycle_now();
    uint32_t gray_start_cycle = 0U;
    float frame_min_temp = 0.0f;
    float frame_max_temp = 0.0f;
    float frame_center_temp = 0.0f;

    if (s_runEnabled == 0U)
    {
        app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
        return;
    }

    {
        redpic1_thermal_frame_slot_t *back_slot = 0;
        const float *gray_source_frame = 0;
        uint8_t high_motion_frame = 0U;
        uint8_t captured_subpage = 0U;
        uint32_t capture_tick_ms = 0U;
        uint32_t get_temp_elapsed_us = 0U;

#if REDPIC1_THERMAL_DIAG_MODE == REDPIC1_THERMAL_DIAG_MODE_TEST_PATTERN
        redpic1_thermal_present_diag_pattern();
        app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
        return;
#endif

        if (redpic1_thermal_capture_prepare_step() == 0U)
        {
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        back_slot = redpic1_thermal_get_back_slot();
        if (back_slot == 0)
        {
            app_perf_baseline_record_thermal_back_slot_null();
            redpic1_thermal_capture_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        if (redpic1_thermal_capture_read_frame(back_slot->temp_frame,
                                               &captured_subpage,
                                               &capture_tick_ms,
                                               &get_temp_elapsed_us) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        if (redpic1_thermal_pair_try_compose(back_slot->temp_frame,
                                             captured_subpage,
                                             capture_tick_ms,
                                             get_temp_elapsed_us,
                                             app_perf_baseline_elapsed_us(step_start_cycle),
                                             &capture_tick_ms) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        if (redpic1_thermal_visual_frame_data_is_valid(back_slot->temp_frame) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            redpic1_thermal_capture_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        if (redpic1_thermal_stage6l3_capture_gap_exceeded(capture_tick_ms) != 0U)
        {
            redpic1_thermal_stage6l3_invalidate_history();
        }

        gray_source_frame = redpic1_thermal_visual_get_gray_source_frame(back_slot->temp_frame,
                                                                         &high_motion_frame);

        gray_start_cycle = app_perf_baseline_cycle_now();
        redpic1_thermal_visual_prepare_gray_frame(back_slot->temp_frame,
                                                  gray_source_frame,
                                                  high_motion_frame,
                                                  back_slot->gray_frame,
                                                  &frame_min_temp,
                                                  &frame_max_temp);
        app_perf_baseline_record_gray_us(app_perf_baseline_elapsed_us(gray_start_cycle));
        frame_center_temp = redpic1_thermal_center_temp(back_slot->temp_frame);

        if (redpic1_thermal_visual_frame_is_valid(frame_min_temp,
                                                  frame_max_temp,
                                                  frame_center_temp) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            redpic1_thermal_capture_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        if (redpic1_thermal_visual_gray_frame_has_contrast(back_slot->gray_frame) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            redpic1_thermal_capture_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        if (s_runEnabled == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        back_slot->min_temp = frame_min_temp;
        back_slot->max_temp = frame_max_temp;
        back_slot->center_temp = frame_center_temp;
        back_slot->capture_tick_ms = capture_tick_ms;
        back_slot->frame_seq = redpic1_thermal_frame_slot_allocate_sequence();

        app_perf_baseline_record_thermal_capture_success(capture_tick_ms,
                                                         frame_min_temp,
                                                         frame_max_temp,
                                                         frame_center_temp);

        redpic1_thermal_publish_back_slot(back_slot);
        if (s_display_paused == 0U && s_overlayHold == 0U)
        {
            (void)redpic1_thermal_frame_slot_submit_latest_ready();
        }
        redpic1_thermal_capture_note_success();
        redpic1_thermal_visual_note_capture_success(capture_tick_ms);
    }

    app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
}


/* 强制重显最近一帧 front 内容。
 * 该接口只重送最近稳定帧，不触发新的热成像采集。 */
void redpic1_thermal_force_refresh(void)
{
    redpic1_thermal_mark_bottom_bar_dirty(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE);
#if REDPIC1_THERMAL_DIAG_MODE == REDPIC1_THERMAL_DIAG_MODE_TEST_PATTERN
    redpic1_thermal_present_diag_pattern();
#else
    redpic1_thermal_present_front_slot();
#endif
}

void redpic1_thermal_render_runtime_overlay(void)
{
    char line_text[64];
    uint32_t now_ms = power_manager_get_tick_ms();

    if (s_runtime_overlay_visible == 0U)
    {
        if (s_overlay_bar_last_visible != 0U || s_overlay_bar_last_line_valid != 0U)
        {
            redpic1_thermal_clear_bottom_bar();
            s_overlay_bar_last_visible = 0U;
            s_overlay_bar_last_line_valid = 0U;
            redpic1_thermal_mark_bottom_bar_dirty((uint8_t)(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE |
                                                             REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_VISIBLE));
        }
        return;
    }

    redpic1_thermal_build_bottom_bar_line(line_text, sizeof(line_text));
    if (strcmp(s_overlay_bar_pending_line, line_text) != 0)
    {
        snprintf(s_overlay_bar_pending_line, sizeof(s_overlay_bar_pending_line), "%s", line_text);
        redpic1_thermal_mark_bottom_bar_dirty(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_TEXT);
    }

    if (s_overlay_bar_last_visible == 0U || s_overlay_bar_last_line_valid == 0U)
    {
        redpic1_thermal_draw_bottom_bar_line(s_overlay_bar_pending_line);
        snprintf(s_overlay_bar_last_line, sizeof(s_overlay_bar_last_line), "%s", s_overlay_bar_pending_line);
        s_overlay_bar_last_visible = 1U;
        s_overlay_bar_last_line_valid = 1U;
        redpic1_thermal_clear_bottom_bar_dirty();
        s_overlay_bar_last_refresh_ms = now_ms;
        return;
    }

    if (s_overlay_bar_pending_dirty == 0U)
    {
        return;
    }

    if (((s_overlay_bar_dirty_reason_mask &
          (REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE | REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_VISIBLE)) == 0U) &&
        ((uint32_t)(now_ms - s_overlay_bar_last_refresh_ms) < REDPIC1_THERMAL_OVERLAY_BAR_REFRESH_MS))
    {
        return;
    }

    redpic1_thermal_draw_bottom_bar_line(s_overlay_bar_pending_line);
    snprintf(s_overlay_bar_last_line, sizeof(s_overlay_bar_last_line), "%s", s_overlay_bar_pending_line);
    s_overlay_bar_last_visible = 1U;
    s_overlay_bar_last_line_valid = 1U;
    redpic1_thermal_clear_bottom_bar_dirty();
    s_overlay_bar_last_refresh_ms = now_ms;
}

uint8_t redpic1_thermal_runtime_overlay_visible(void)
{
    return s_runtime_overlay_visible;
}

/* 处理热成像页本地按键。
 * KEY1 顺时针切色板；KEY2 切暂停/恢复，且在“暂停发送温度”设置开启时发送一次摘要；
 * KEY3 逆时针切色板。 */
void redpic1_thermal_handle_key(uint8_t key_value)
{
    switch (key_value)
    {
    case KEY1_PRES:
        s_colorMode++;
        if (s_colorMode > 4U)
        {
            s_colorMode = 0U;
        }
        set_color_mode(s_colorMode);
        redpic1_thermal_mark_bottom_bar_dirty(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PALETTE);
        break;

    case KEY2_PRES:
    {
        uint8_t was_display_paused = s_display_paused;

        if (power_manager_get_tick_ms() < s_key2_ignore_until_ms)
        {
            break;
        }

        s_display_paused = (uint8_t)!s_display_paused;
        if (s_display_paused != 0U)
        {
            redpic1_thermal_cancel_pending_present_and_clear_submit();
            if (was_display_paused == 0U &&
                redpic1_thermal_cloud_pause_send_esp_enabled() != 0U)
            {
                (void)redpic1_thermal_cloud_submit_snapshot_to_esp();
            }
        }
        redpic1_thermal_mark_bottom_bar_dirty(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PAUSE);
        redpic1_thermal_stage6l3_invalidate_history();
        if (was_display_paused != 0U && s_display_paused == 0U)
        {
            redpic1_thermal_frame_slot_try_submit_if_possible();
        }
    }
        break;

    case KEY3_PRES:
        if (s_colorMode == 0U)
        {
            s_colorMode = 4U;
        }
        else
        {
            s_colorMode--;
        }
        set_color_mode(s_colorMode);
        redpic1_thermal_mark_bottom_bar_dirty(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PALETTE);
        break;

    default:
        break;
    }
}

/* 暂停热成像采集与送显。
 * 保持原有语义：切低刷新率、取消待送显请求、丢弃本地缓存并释放 thermal 电源锁。 */
void redpic1_thermal_suspend(void)
{
    s_runEnabled = 0U;
    redpic1_thermal_apply_refresh_rate(REDPIC1_THERMAL_IDLE_REFRESH_RATE);
    redpic1_thermal_reset_bottom_bar_cache();

    redpic1_thermal_cancel_pending_present_and_clear_submit();
    redpic1_thermal_drop_non_inflight_slots();
    redpic1_thermal_cloud_reset();
    if (s_diag_pattern_ready == 0U)
    {
        redpic1_thermal_build_diag_pattern();
    }

    power_manager_release_lock(POWER_LOCK_THERMAL);
}

/* 恢复热成像采集与送显。
 * 恢复时清除 overlay hold/暂停状态，重新获取 thermal 电源锁，并尝试补提交 READY 帧。 */
void redpic1_thermal_resume(void)
{
    redpic1_thermal_apply_refresh_rate(REDPIC1_THERMAL_ACTIVE_REFRESH_RATE);
    s_runEnabled = 1U;
    s_key2_ignore_until_ms = power_manager_get_tick_ms() + REDPIC1_THERMAL_KEY2_ENTRY_GUARD_MS;
    s_overlayHold = 0U;
    s_display_paused = 0U;
    redpic1_thermal_reset_bottom_bar_cache();

    redpic1_thermal_drop_non_inflight_slots();
    redpic1_thermal_cloud_reset();

    power_manager_acquire_lock(POWER_LOCK_THERMAL);
    power_manager_notify_activity();

    redpic1_thermal_frame_slot_try_submit_if_possible();
}

/* STOP 唤醒后恢复 MLX90640 总线。
 * 若调度器已运行，则只设置延后恢复标志，由 thermal task 在安全上下文中完成恢复。 */
void redpic1_thermal_restore_bus_after_stop(void)
{
    redpic1_thermal_capture_request_restore_after_stop(redpic1_thermal_scheduler_running());
}

/* 控制 thermal 叠加层持有状态。
 * 持有期间禁止新的送显提交；若已有待处理提交，则按既有语义立即取消。 */
void redpic1_thermal_set_overlay_hold(uint8_t enabled)
{
    s_overlayHold = enabled;
    if (enabled != 0U)
    {
        redpic1_thermal_cancel_pending_present_and_clear_submit();
    }
}
