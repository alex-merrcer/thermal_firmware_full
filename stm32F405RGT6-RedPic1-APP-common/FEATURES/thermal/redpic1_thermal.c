/**
 * @file    redpic1_thermal.c
 * @brief   热成像采集、灰度生成与异步显示提交模块
 * @note    本模块是热成像功能的核心控制器，负责：
 *          1. MLX90640 传感器 I2C 采集与总线退避/恢复机制
 *          2. 帧槽位所有权状态机（FREE → WRITING → READY → INFLIGHT → FRONT）
 *          3. Display Runtime 异步回调后的帧生命周期管理
 *          4. 热成像页运行时叠加条（Overlay）与本地按键控制
 *          5. KEY2 快照保存功能
 *
 * @par 核心架构
 *      采用"三槽位异步 Present 路径"解耦传感器采集与屏幕刷新。
 *      thermal task 单步执行顺序：backoff 判定 → 取帧 → 子页配对 → 灰度生成 → 校验 → 发布/提交。
 *
 * @par 底部状态栏布局
 *      | FPS  |  最低  |  最高  |  中心  |
 *      分割线位于 X=54, X=138, X=222。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

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
#include "redpic1_app.h"
#include "MLX90640.h"
#include "lcd_dma.h"
#include "snapshot_storage.h"
#include "thermal_capture.h"
#include "thermal_cloud.h"
#include "thermal_frame_slot.h"
#include "thermal_pair.h"
#include "thermal_visual.h"

/* =========================================================================
 *  2. 宏定义 —— 基础参数
 * ======================================================================= */

#define REDPIC1_THERMAL_ACTIVE_REFRESH_RATE            RefreshRate     /**< 活跃模式硬件帧率    */
#define REDPIC1_THERMAL_IDLE_REFRESH_RATE              FPS1HZ          /**< 休眠模式帧率        */
#define REDPIC1_THERMAL_SRC_ROWS                       24U             /**< MLX90640 行数       */
#define REDPIC1_THERMAL_SRC_COLS                       32U             /**< MLX90640 列数       */
#define REDPIC1_THERMAL_PIXEL_COUNT                    768U            /**< 总像素数 (24×32)    */

/* =========================================================================
 *  3. 宏定义 —— 显示与 UI 布局参数
 * ======================================================================= */

#define REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT             20U             /**< 底部状态栏高度      */
#define REDPIC1_THERMAL_VIEWPORT_HEIGHT                (LCD_H - REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT) /**< 成像可视区域高度 */
#define REDPIC1_THERMAL_OVERLAY_BAR_TEXT_Y_OFFSET      2U              /**< 状态栏文本 Y 偏移   */
#define REDPIC1_THERMAL_OVERLAY_BAR_TEXT_X             4U              /**< 状态栏文本 X 起始   */
#define REDPIC1_THERMAL_OVERLAY_CROSS_HALF_SIZE        6U              /**< 中心十字准星半宽    */
#define REDPIC1_THERMAL_OVERLAY_BAR_REFRESH_MS         250UL           /**< 状态栏刷新防抖（ms）*/

/* =========================================================================
 *  4. 宏定义 —— 底栏脏标志掩码
 * ======================================================================= */

#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_TEXT      (1U << 0)       /**< 文本内容变更        */
#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PALETTE   (1U << 1)       /**< 调色板切换          */
#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PAUSE     (1U << 2)       /**< 暂停/恢复切换       */
#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE     (1U << 3)       /**< 强制重绘            */
#define REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_VISIBLE   (1U << 4)       /**< 可见性变更          */

/* =========================================================================
 *  5. 静态全局变量 —— 显示控制与 UI 状态
 * ======================================================================= */

static uint8_t  s_display_paused       = 0U;                          /**< 暂停送显标志        */
static uint8_t  s_runEnabled           = 1U;                          /**< 模块总使能开关      */
static uint8_t  s_refreshRate          = REDPIC1_THERMAL_ACTIVE_REFRESH_RATE; /**< 当前帧率   */
static uint8_t  s_overlayHold          = 0U;                          /**< 叠加层持有标志      */
static uint8_t  s_colorMode            = 0U;                          /**< 调色板模式 (0~4)    */
static uint8_t  s_diag_pattern_ready   = 0U;                          /**< 诊断图案就绪标志    */
static uint8_t  s_runtime_overlay_visible = 1U;                        /**< 状态栏可见性开关    */
static uint32_t s_overlay_bar_last_refresh_ms = 0U;                    /**< 上次状态栏刷新时间  */
static uint32_t s_key2_ignore_until_ms = 0U;                          /**< KEY2 入页忽略截止   */
static uint8_t  s_overlay_bar_last_visible = 0U;                       /**< 上次状态栏可见性    */
static uint8_t  s_overlay_bar_pending_dirty = 1U;                      /**< 状态栏脏标志        */
static uint8_t  s_overlay_bar_dirty_reason_mask =                      /**< 脏原因掩码          */
    REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE;

/* 诊断测试图案灰度数据（CCM RAM，快速访问） */
static CCMRAM uint8_t s_diag_pattern_frame[REDPIC1_THERMAL_PIXEL_COUNT];

/* 最新热成像快照（用于 KEY2 快照保存） */
static redpic1_thermal_snapshot_t s_latest_snapshot;

/* KEY2 快照状态 */
static uint8_t s_key2_snapshot_pending      = 0U;                      /**< 快照待执行标志      */
static uint8_t s_key2_snapshot_in_progress  = 0U;                      /**< 快照进行中标志      */
static uint8_t s_key2_snapshot_status_visible = 0U;                    /**< 快照状态可见标志    */

/* =========================================================================
 *  6. 宏定义 —— 叠加层颜色与位置
 * ======================================================================= */

/** RGB565 颜色构造宏 */
#define REDPIC1_THERMAL_UI_RGB565(r, g, b) \
    ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | \
                (((uint16_t)(g) & 0xFCU) << 3) | \
                (((uint16_t)(b) & 0xF8U) >> 3)))

/* 叠加层颜色定义 */
#define REDPIC1_THERMAL_OVERLAY_BG_COLOR               REDPIC1_THERMAL_UI_RGB565(5U, 18U, 30U)
#define REDPIC1_THERMAL_OVERLAY_TOP_LINE_COLOR         REDPIC1_THERMAL_UI_RGB565(48U, 78U, 96U)
#define REDPIC1_THERMAL_OVERLAY_LABEL_FPS_COLOR        REDPIC1_THERMAL_UI_RGB565(90U, 220U, 230U)
#define REDPIC1_THERMAL_OVERLAY_LABEL_MIN_COLOR        REDPIC1_THERMAL_UI_RGB565(90U, 220U, 230U)
#define REDPIC1_THERMAL_OVERLAY_LABEL_MAX_COLOR        REDPIC1_THERMAL_UI_RGB565(255U, 176U, 64U)
#define REDPIC1_THERMAL_OVERLAY_LABEL_CENTER_COLOR     WHITE
#define REDPIC1_THERMAL_OVERLAY_VALUE_COLOR            WHITE
#define REDPIC1_THERMAL_OVERLAY_SEGMENT_LINE_COLOR     REDPIC1_THERMAL_UI_RGB565(18U, 40U, 58U)

/* 叠加层各区域 X 坐标与宽度 */
#define REDPIC1_THERMAL_OVERLAY_FPS_LABEL_X            4U
#define REDPIC1_THERMAL_OVERLAY_FPS_VALUE_X            30U
#define REDPIC1_THERMAL_OVERLAY_FPS_VALUE_W            24U
#define REDPIC1_THERMAL_OVERLAY_MIN_LABEL_X            58U
#define REDPIC1_THERMAL_OVERLAY_MIN_VALUE_X            90U
#define REDPIC1_THERMAL_OVERLAY_MIN_VALUE_W            48U
#define REDPIC1_THERMAL_OVERLAY_MAX_LABEL_X            142U
#define REDPIC1_THERMAL_OVERLAY_MAX_VALUE_X            174U
#define REDPIC1_THERMAL_OVERLAY_MAX_VALUE_W            48U
#define REDPIC1_THERMAL_OVERLAY_CENTER_LABEL_X         226U
#define REDPIC1_THERMAL_OVERLAY_CENTER_VALUE_X         258U
#define REDPIC1_THERMAL_OVERLAY_CENTER_VALUE_W         58U

/* =========================================================================
 *  7. 内部数据类型定义
 * ======================================================================= */

/** @brief 底部状态栏视图缓存（用于脏检测与局部刷新） */
typedef struct
{
    uint8_t     valid;              /**< 数据有效性标志          */
    uint32_t    lcd_fps;            /**< LCD 送显帧率            */
    char        min_text[12];       /**< 最低温度文本            */
    char        max_text[12];       /**< 最高温度文本            */
    char        center_text[12];    /**< 中心温度文本            */
} redpic1_thermal_bottom_bar_view_t;

/** @brief KEY2 快照状态枚举 */
typedef enum
{
    REDPIC1_THERMAL_SNAPSHOT_STATUS_IDLE    = 0,    /**< 空闲              */
    REDPIC1_THERMAL_SNAPSHOT_STATUS_QUEUED,         /**< 已排队            */
    REDPIC1_THERMAL_SNAPSHOT_STATUS_SAVING,         /**< 保存中            */
    REDPIC1_THERMAL_SNAPSHOT_STATUS_OK,             /**< 保存成功          */
    REDPIC1_THERMAL_SNAPSHOT_STATUS_FAIL            /**< 保存失败          */
} redpic1_thermal_snapshot_status_t;

/* =========================================================================
 *  8. 静态全局变量 —— 视图缓存与快照状态
 * ======================================================================= */

static redpic1_thermal_bottom_bar_view_t s_overlay_bar_last_view;      /**< 上次状态栏视图      */
static redpic1_thermal_snapshot_status_t s_key2_snapshot_status =      /**< 快照状态            */
    REDPIC1_THERMAL_SNAPSHOT_STATUS_IDLE;

/* =========================================================================
 *  9. 内部函数前向声明
 * ======================================================================= */

static uint8_t redpic1_thermal_present_gray_frame(const uint8_t *gray_frame);

/* =========================================================================
 *  10. 内部函数实现 —— 基础辅助（调度器与临界区）
 * ======================================================================= */

/**
 * @brief  检查 FreeRTOS 调度器是否运行中
 * @retval 1 — 调度器运行中；0 — 未启动或已挂起
 */
static uint8_t redpic1_thermal_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

/**
 * @brief  安全进入临界区（仅当调度器运行时生效）
 * @note   保护多任务共享的槽位索引与状态标志，防止数据竞争。
 */
static void redpic1_thermal_enter_critical(void)
{
    if (redpic1_thermal_scheduler_running() != 0U)
    {
        taskENTER_CRITICAL();
    }
}

/**
 * @brief  安全退出临界区
 */
static void redpic1_thermal_exit_critical(void)
{
    if (redpic1_thermal_scheduler_running() != 0U)
    {
        taskEXIT_CRITICAL();
    }
}

/* =========================================================================
 *  11. 内部函数实现 —— 帧槽位辅助代理
 * ======================================================================= */

/** @brief  获取当前系统 tick（ms） */
static uint32_t redpic1_thermal_frame_slot_tick_ms(void)
{
    return power_manager_get_tick_ms();
}

/** @brief  查询模块使能状态 */
static uint8_t redpic1_thermal_frame_slot_run_enabled(void)
{
    return s_runEnabled;
}

/** @brief  查询送显暂停状态 */
static uint8_t redpic1_thermal_frame_slot_display_paused(void)
{
    return s_display_paused;
}

/** @brief  查询叠加层持有状态 */
static uint8_t redpic1_thermal_frame_slot_overlay_hold(void)
{
    return s_overlayHold;
}

/* =========================================================================
 *  12. 内部函数实现 —— 温度格式化与转换
 * ======================================================================= */

/**
 * @brief  格式化温度值为带一位小数的字符串
 * @note   采用定点数思想避免浮点 snprintf 的精度/体积问题，适合资源受限 MCU。
 * @param  buffer     — 输出缓冲区
 * @param  buffer_len — 缓冲区长度
 * @param  temp       — 温度值（°C）
 * @param  has_value  — 数据有效性标志（0 时显示 "--.-"）
 */
static void redpic1_thermal_format_overlay_temp(char *buffer,
                                                uint16_t buffer_len,
                                                float temp,
                                                uint8_t has_value)
{
    int32_t scaled = 0;
    int32_t whole  = 0;
    int32_t frac   = 0;

    if (buffer == 0 || buffer_len == 0U)
    {
        return;
    }

    /* 无效数据显示占位符 */
    if (has_value == 0U)
    {
        snprintf(buffer, buffer_len, "%s", "--.-");
        return;
    }

    /* 浮点转定点（×10，四舍五入） */
    scaled = (temp >= 0.0f) ?
             (int32_t)(temp * 10.0f + 0.5f) :
             (int32_t)(temp * 10.0f - 0.5f);

    whole = scaled / 10;
    frac  = scaled % 10;
    if (frac < 0)
    {
        frac = -frac;
    }

    snprintf(buffer, buffer_len, "%ld.%ld", (long)whole, (long)frac);
}

/**
 * @brief  将浮点温度转换为 ×10 定点格式（int16_t）
 * @param  temp — 温度值（°C）
 * @return ×10 定点温度值
 */
static int16_t redpic1_thermal_temp_to_x10(float temp)
{
    float scaled = temp * 10.0f;

    if (scaled >= 0.0f)
    {
        scaled += 0.5f;
    }
    else
    {
        scaled -= 0.5f;
    }

    /* 钳位到 int16_t 范围 */
    if (scaled > 32767.0f)
    {
        scaled = 32767.0f;
    }
    else if (scaled < -32768.0f)
    {
        scaled = -32768.0f;
    }

    return (int16_t)scaled;
}

/* =========================================================================
 *  13. 内部函数实现 —— 最新快照管理
 * ======================================================================= */

/**
 * @brief  更新最新热成像快照
 * @note   在临界区内将帧数据写入 s_latest_snapshot。
 * @param  frame_data      — 帧温度数据（768 floats）
 * @param  min_temp        — 最低温度
 * @param  max_temp        — 最高温度
 * @param  center_temp     — 中心温度
 * @param  frame_id        — 帧序号
 * @param  capture_tick_ms — 采集时间戳
 */
static void redpic1_thermal_update_latest_snapshot(const float *frame_data,
                                                   float min_temp,
                                                   float max_temp,
                                                   float center_temp,
                                                   uint32_t frame_id,
                                                   uint32_t capture_tick_ms)
{
    uint16_t index = 0U;

    if (frame_data == 0)
    {
        return;
    }

    redpic1_thermal_enter_critical();

    s_latest_snapshot.valid         = 1U;
    s_latest_snapshot.frame_id      = frame_id;
    s_latest_snapshot.timestamp_ms  = capture_tick_ms;
    s_latest_snapshot.min_x10       = redpic1_thermal_temp_to_x10(min_temp);
    s_latest_snapshot.max_x10       = redpic1_thermal_temp_to_x10(max_temp);
    s_latest_snapshot.center_x10    = redpic1_thermal_temp_to_x10(center_temp);

    /* 逐像素转换为 ×10 定点格式 */
    for (index = 0U; index < REDPIC1_THERMAL_SNAPSHOT_PIXEL_COUNT; ++index)
    {
        s_latest_snapshot.pixels_x10[index] = redpic1_thermal_temp_to_x10(frame_data[index]);
    }

    redpic1_thermal_exit_critical();
}

/* =========================================================================
 *  14. 内部函数实现 —— KEY2 快照管理
 * ======================================================================= */

/**
 * @brief  查询 KEY2 快照功能是否启用
 * @retval 1 — 已启用；0 — 未启用
 */
static uint8_t redpic1_thermal_key2_snapshot_enabled(void)
{
    device_settings_t settings;

    memset(&settings, 0, sizeof(settings));
    app_rtos_settings_copy(&settings);
    return settings.key2_snapshot_enabled;
}

/**
 * @brief  获取快照状态的显示文本
 * @return 状态文本指针；IDLE 状态返回 NULL
 */
static const char *redpic1_thermal_snapshot_status_text(void)
{
    switch (s_key2_snapshot_status)
    {
    case REDPIC1_THERMAL_SNAPSHOT_STATUS_QUEUED:
        return "SNAP...";
    case REDPIC1_THERMAL_SNAPSHOT_STATUS_SAVING:
        return "SAVING";
    case REDPIC1_THERMAL_SNAPSHOT_STATUS_OK:
        return "SHOT OK";
    case REDPIC1_THERMAL_SNAPSHOT_STATUS_FAIL:
        return "SHOT ERR";
    case REDPIC1_THERMAL_SNAPSHOT_STATUS_IDLE:
    default:
        return 0;
    }
}

/**
 * @brief  绘制快照状态叠加层
 * @note   在 LCD 右上角显示快照状态（排队/保存中/成功/失败）。
 */
static void redpic1_thermal_draw_snapshot_status_overlay(void)
{
    const char *text = 0;
    uint16_t box_left   = (uint16_t)(LCD_W - 92U);
    uint16_t box_top    = 4U;
    uint16_t box_right  = (uint16_t)(LCD_W - 4U);
    uint16_t box_bottom = 19U;
    uint16_t text_color = WHITE;
    uint16_t bg_color   = REDPIC1_THERMAL_UI_RGB565(8U, 20U, 32U);

    text = redpic1_thermal_snapshot_status_text();

    /* 非暂停状态或无文本：清除状态显示 */
    if (s_display_paused == 0U || text == 0)
    {
        if (s_key2_snapshot_status_visible == 0U)
        {
            return;
        }
        app_display_runtime_lock();
        LCD_Fill(box_left, box_top, box_right, box_bottom, BLACK);
        app_display_runtime_unlock();
        s_key2_snapshot_status_visible = 0U;
        return;
    }

    /* 根据状态选择颜色 */
    if (s_key2_snapshot_status == REDPIC1_THERMAL_SNAPSHOT_STATUS_OK)
    {
        bg_color   = GREEN;
        text_color = BLACK;
    }
    else if (s_key2_snapshot_status == REDPIC1_THERMAL_SNAPSHOT_STATUS_FAIL)
    {
        bg_color = RED;
    }
    else if (s_key2_snapshot_status == REDPIC1_THERMAL_SNAPSHOT_STATUS_SAVING)
    {
        bg_color   = REDPIC1_THERMAL_UI_RGB565(255U, 120U, 0U);
        text_color = BLACK;
    }

    /* 绘制状态框 */
    app_display_runtime_lock();
    LCD_Fill(box_left, box_top, box_right, box_bottom, bg_color);
    LCD_ShowString((uint16_t)(box_left + 4U),
                   (uint16_t)(box_top + 2U),
                   (const u8 *)text,
                   text_color, bg_color, 12, 0);
    app_display_runtime_unlock();
    s_key2_snapshot_status_visible = 1U;
}

/**
 * @brief  重置快照状态
 */
static void redpic1_thermal_snapshot_status_reset(void)
{
    s_key2_snapshot_pending     = 0U;
    s_key2_snapshot_in_progress = 0U;
    s_key2_snapshot_status      = REDPIC1_THERMAL_SNAPSHOT_STATUS_IDLE;
    redpic1_thermal_draw_snapshot_status_overlay();
}

/* =========================================================================
 *  15. 内部函数实现 —— 底部状态栏视图构建
 * ======================================================================= */

/**
 * @brief  计算状态栏顶部 Y 坐标
 * @return 状态栏顶部 Y 坐标
 */
static uint16_t redpic1_thermal_bottom_bar_top(void)
{
    return (uint16_t)((LCD_H > REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT)
                          ? (LCD_H - REDPIC1_THERMAL_OVERLAY_BAR_HEIGHT)
                          : 0U);
}

/**
 * @brief  构建底部状态栏视图数据
 * @note   从性能基线模块获取 FPS 与极值温度，格式化为文本。
 * @param  view — 输出：视图数据指针
 */
static void redpic1_thermal_build_bottom_bar_view(redpic1_thermal_bottom_bar_view_t *view)
{
    app_perf_baseline_snapshot_t snapshot;
    uint8_t has_value = 0U;

    if (view == 0)
    {
        return;
    }

    memset(view, 0, sizeof(*view));
    app_perf_baseline_get_snapshot(&snapshot);
    has_value = (snapshot.thermal_capture_frames != 0U) ? 1U : 0U;

    view->valid   = 1U;
    view->lcd_fps = snapshot.lcd_present_fps;

    redpic1_thermal_format_overlay_temp(view->min_text,
                                        sizeof(view->min_text),
                                        snapshot.latest_min_temp,
                                        has_value);
    redpic1_thermal_format_overlay_temp(view->max_text,
                                        sizeof(view->max_text),
                                        snapshot.latest_max_temp,
                                        has_value);
    redpic1_thermal_format_overlay_temp(view->center_text,
                                        sizeof(view->center_text),
                                        snapshot.latest_center_temp,
                                        has_value);
}

/**
 * @brief  比较两个视图数据是否相同
 * @param  a — 视图 A
 * @param  b — 视图 B
 * @retval 1 — 相同；0 — 不同
 */
static uint8_t redpic1_thermal_bottom_bar_view_equal(
    const redpic1_thermal_bottom_bar_view_t *a,
    const redpic1_thermal_bottom_bar_view_t *b)
{
    if (a == 0 || b == 0)
    {
        return 0U;
    }

    if (a->valid != b->valid || a->lcd_fps != b->lcd_fps)
    {
        return 0U;
    }
    if (strcmp(a->min_text, b->min_text) != 0)
    {
        return 0U;
    }
    if (strcmp(a->max_text, b->max_text) != 0)
    {
        return 0U;
    }
    if (strcmp(a->center_text, b->center_text) != 0)
    {
        return 0U;
    }

    return 1U;
}

/* =========================================================================
 *  16. 内部函数实现 —— 底部状态栏绘制
 * ======================================================================= */

/**
 * @brief  绘制底部状态栏背景与静态标签
 * @note   包括背景填充、顶部分割线、竖向分隔线和"FPS/最低/最高/中心"标签。
 */
static void redpic1_thermal_draw_bottom_bar_bg(void)
{
    uint16_t bar_top    = redpic1_thermal_bottom_bar_top();
    uint16_t bar_bottom = (uint16_t)(LCD_H - 1U);

    /* 背景填充 */
    LCD_Fill(0U, bar_top, (uint16_t)(LCD_W - 1U), bar_bottom,
             REDPIC1_THERMAL_OVERLAY_BG_COLOR);

    /* 顶部分割线 */
    if (bar_top > 0U)
    {
        LCD_DrawLine(0U,
                     (uint16_t)(bar_top - 1U),
                     (uint16_t)(LCD_W - 1U),
                     (uint16_t)(bar_top - 1U),
                     REDPIC1_THERMAL_OVERLAY_TOP_LINE_COLOR);
    }

    /* 竖向分隔线 */
    LCD_DrawLine(54U,  bar_top, 54U,  bar_bottom, REDPIC1_THERMAL_OVERLAY_SEGMENT_LINE_COLOR);
    LCD_DrawLine(138U, bar_top, 138U, bar_bottom, REDPIC1_THERMAL_OVERLAY_SEGMENT_LINE_COLOR);
    LCD_DrawLine(222U, bar_top, 222U, bar_bottom, REDPIC1_THERMAL_OVERLAY_SEGMENT_LINE_COLOR);

    /* "FPS" 标签 */
    LCD_ShowUTF8String(REDPIC1_THERMAL_OVERLAY_FPS_LABEL_X,
                       (uint16_t)(bar_top + REDPIC1_THERMAL_OVERLAY_BAR_TEXT_Y_OFFSET),
                       "FPS",
                       REDPIC1_THERMAL_OVERLAY_LABEL_FPS_COLOR,
                       REDPIC1_THERMAL_OVERLAY_BG_COLOR, 16, 0);

    /* "最低" 标签（UTF-8 编码） */
    LCD_ShowUTF8String(REDPIC1_THERMAL_OVERLAY_MIN_LABEL_X,
                       (uint16_t)(bar_top + REDPIC1_THERMAL_OVERLAY_BAR_TEXT_Y_OFFSET),
                       "\xE6\x9C\x80\xE4\xBD\x8E",
                       REDPIC1_THERMAL_OVERLAY_LABEL_MIN_COLOR,
                       REDPIC1_THERMAL_OVERLAY_BG_COLOR, 16, 0);

    /* "最高" 标签 */
    LCD_ShowUTF8String(REDPIC1_THERMAL_OVERLAY_MAX_LABEL_X,
                       (uint16_t)(bar_top + REDPIC1_THERMAL_OVERLAY_BAR_TEXT_Y_OFFSET),
                       "\xE6\x9C\x80\xE9\xAB\x98",
                       REDPIC1_THERMAL_OVERLAY_LABEL_MAX_COLOR,
                       REDPIC1_THERMAL_OVERLAY_BG_COLOR, 16, 0);

    /* "中心" 标签 */
    LCD_ShowUTF8String(REDPIC1_THERMAL_OVERLAY_CENTER_LABEL_X,
                       (uint16_t)(bar_top + REDPIC1_THERMAL_OVERLAY_BAR_TEXT_Y_OFFSET),
                       "\xE4\xB8\xAD\xE5\xBF\x83",
                       REDPIC1_THERMAL_OVERLAY_LABEL_CENTER_COLOR,
                       REDPIC1_THERMAL_OVERLAY_BG_COLOR, 16, 0);
}

/**
 * @brief  绘制状态栏单个数值区域
 * @param  value_x     — 数值区域 X 起始坐标
 * @param  value_width — 数值区域宽度
 * @param  value_text  — 数值文本
 */
static void redpic1_thermal_draw_bottom_bar_value(uint16_t value_x,
                                                  uint16_t value_width,
                                                  const char *value_text)
{
    uint16_t bar_top    = redpic1_thermal_bottom_bar_top();
    uint16_t value_x_end = (uint16_t)(value_x + value_width - 1U);

    if (value_text == 0 || value_width == 0U)
    {
        return;
    }

    /* 清除旧内容 */
    LCD_Fill(value_x, bar_top, value_x_end, (uint16_t)(LCD_H - 1U),
             REDPIC1_THERMAL_OVERLAY_BG_COLOR);

    /* 绘制新数值 */
    LCD_ShowUTF8String(value_x,
                       (uint16_t)(bar_top + REDPIC1_THERMAL_OVERLAY_BAR_TEXT_Y_OFFSET),
                       value_text,
                       REDPIC1_THERMAL_OVERLAY_VALUE_COLOR,
                       REDPIC1_THERMAL_OVERLAY_BG_COLOR, 16, 0);
}

/**
 * @brief  绘制状态栏所有数值（全量刷新）
 * @param  view — 视图数据指针
 */
static void redpic1_thermal_draw_bottom_bar_values(
    const redpic1_thermal_bottom_bar_view_t *view)
{
    char fps_text[8];
    char temp_text[16];

    if (view == 0)
    {
        return;
    }

    /* FPS 数值 */
    snprintf(fps_text, sizeof(fps_text), "%lu", (unsigned long)view->lcd_fps);
    redpic1_thermal_draw_bottom_bar_value(REDPIC1_THERMAL_OVERLAY_FPS_VALUE_X,
                                          REDPIC1_THERMAL_OVERLAY_FPS_VALUE_W,
                                          fps_text);

    /* 最低温度 */
    snprintf(temp_text, sizeof(temp_text), "%sC", view->min_text);
    redpic1_thermal_draw_bottom_bar_value(REDPIC1_THERMAL_OVERLAY_MIN_VALUE_X,
                                          REDPIC1_THERMAL_OVERLAY_MIN_VALUE_W,
                                          temp_text);

    /* 最高温度 */
    snprintf(temp_text, sizeof(temp_text), "%sC", view->max_text);
    redpic1_thermal_draw_bottom_bar_value(REDPIC1_THERMAL_OVERLAY_MAX_VALUE_X,
                                          REDPIC1_THERMAL_OVERLAY_MAX_VALUE_W,
                                          temp_text);

    /* 中心温度 */
    snprintf(temp_text, sizeof(temp_text), "%sC", view->center_text);
    redpic1_thermal_draw_bottom_bar_value(REDPIC1_THERMAL_OVERLAY_CENTER_VALUE_X,
                                          REDPIC1_THERMAL_OVERLAY_CENTER_VALUE_W,
                                          temp_text);
}

/**
 * @brief  差异刷新状态栏数值（仅更新变化的区域）
 * @param  view — 新视图数据指针
 */
static void redpic1_thermal_draw_bottom_bar_diff(
    const redpic1_thermal_bottom_bar_view_t *view)
{
    char fps_text[8];
    char temp_text[16];

    if (view == 0)
    {
        return;
    }

    /* FPS 变化 */
    if (view->lcd_fps != s_overlay_bar_last_view.lcd_fps)
    {
        snprintf(fps_text, sizeof(fps_text), "%lu", (unsigned long)view->lcd_fps);
        redpic1_thermal_draw_bottom_bar_value(REDPIC1_THERMAL_OVERLAY_FPS_VALUE_X,
                                              REDPIC1_THERMAL_OVERLAY_FPS_VALUE_W,
                                              fps_text);
    }

    /* 最低温度变化 */
    if (strcmp(view->min_text, s_overlay_bar_last_view.min_text) != 0)
    {
        snprintf(temp_text, sizeof(temp_text), "%sC", view->min_text);
        redpic1_thermal_draw_bottom_bar_value(REDPIC1_THERMAL_OVERLAY_MIN_VALUE_X,
                                              REDPIC1_THERMAL_OVERLAY_MIN_VALUE_W,
                                              temp_text);
    }

    /* 最高温度变化 */
    if (strcmp(view->max_text, s_overlay_bar_last_view.max_text) != 0)
    {
        snprintf(temp_text, sizeof(temp_text), "%sC", view->max_text);
        redpic1_thermal_draw_bottom_bar_value(REDPIC1_THERMAL_OVERLAY_MAX_VALUE_X,
                                              REDPIC1_THERMAL_OVERLAY_MAX_VALUE_W,
                                              temp_text);
    }

    /* 中心温度变化 */
    if (strcmp(view->center_text, s_overlay_bar_last_view.center_text) != 0)
    {
        snprintf(temp_text, sizeof(temp_text), "%sC", view->center_text);
        redpic1_thermal_draw_bottom_bar_value(REDPIC1_THERMAL_OVERLAY_CENTER_VALUE_X,
                                              REDPIC1_THERMAL_OVERLAY_CENTER_VALUE_W,
                                              temp_text);
    }
}

/**
 * @brief  清除底部状态栏（恢复为全黑）
 */
static void redpic1_thermal_clear_bottom_bar(void)
{
    uint16_t bar_top   = redpic1_thermal_bottom_bar_top();
    uint16_t clear_top = (bar_top > 0U) ? (uint16_t)(bar_top - 1U) : bar_top;

    LCD_Fill(0U, clear_top, (uint16_t)(LCD_W - 1U), (uint16_t)(LCD_H - 1U), BLACK);
}

/* =========================================================================
 *  17. 内部函数实现 —— 状态栏脏标志管理
 * ======================================================================= */

/**
 * @brief  标记状态栏需要刷新
 * @param  reason_mask — 脏原因掩码
 */
static void redpic1_thermal_mark_bottom_bar_dirty(uint8_t reason_mask)
{
    s_overlay_bar_pending_dirty = 1U;
    s_overlay_bar_dirty_reason_mask =
        (uint8_t)(s_overlay_bar_dirty_reason_mask | reason_mask);
}

/**
 * @brief  清除状态栏脏标志
 */
static void redpic1_thermal_clear_bottom_bar_dirty(void)
{
    s_overlay_bar_pending_dirty     = 0U;
    s_overlay_bar_dirty_reason_mask = 0U;
}

/**
 * @brief  重置状态栏缓存状态
 * @note   强制下次刷新时全量重绘。
 */
static void redpic1_thermal_reset_bottom_bar_cache(void)
{
    memset(&s_overlay_bar_last_view, 0, sizeof(s_overlay_bar_last_view));
    s_overlay_bar_last_refresh_ms = 0U;
    s_overlay_bar_last_visible    = 0U;
    s_overlay_bar_dirty_reason_mask = 0U;
    redpic1_thermal_mark_bottom_bar_dirty(
        (uint8_t)(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE |
                  REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_VISIBLE));
}

/* =========================================================================
 *  18. 内部函数实现 —— 中心温度与处理历史
 * ======================================================================= */

/**
 * @brief  获取图像中心点温度
 * @param  frame_data — 原始温度数组指针
 * @return 中心点温度值；指针为空时返回 0.0f
 */
static float redpic1_thermal_center_temp(const float *frame_data)
{
    return redpic1_thermal_visual_center_temp(frame_data);
}

/**
 * @brief  重置处理历史（滤波窗口与子页配对）
 */
static void redpic1_thermal_reset_processing_history(void)
{
    redpic1_thermal_visual_reset_history();
    redpic1_thermal_pair_reset();
}

/**
 * @brief  失效处理历史（采集间隔过大或错误后调用）
 */
static void redpic1_thermal_stage6l3_invalidate_history(void)
{
    redpic1_thermal_visual_invalidate_history();
    redpic1_thermal_pair_reset();
}

/**
 * @brief  检查采集间隔是否超过阈值
 * @param  capture_tick_ms — 采集时间戳
 * @retval 1 — 超过阈值；0 — 正常
 */
static uint8_t redpic1_thermal_stage6l3_capture_gap_exceeded(uint32_t capture_tick_ms)
{
    return redpic1_thermal_visual_capture_gap_exceeded(capture_tick_ms);
}

/* =========================================================================
 *  19. 内部函数实现 —— 刷新率管理
 * ======================================================================= */

/**
 * @brief  将刷新率枚举转换为周期（ms）
 * @param  refresh_rate — 刷新率枚举值
 * @return 对应的帧周期（ms）
 */
static uint32_t redpic1_thermal_refresh_rate_to_period_ms(uint8_t refresh_rate)
{
    switch (refresh_rate)
    {
    case FPS1HZ:  return 1000UL;
    case FPS2HZ:  return 500UL;
    case FPS4HZ:  return 250UL;
    case FPS8HZ:  return 125UL;
    case FPS16HZ: return 63UL;
    case FPS32HZ: return 32UL;
    default:      return 63UL;
    }
}

/**
 * @brief  应用刷新率设置到 MLX90640
 * @param  refresh_rate — 目标刷新率
 * @param  force_write  — 是否强制写入
 */
static void redpic1_thermal_apply_refresh_rate_internal(uint8_t refresh_rate,
                                                        uint8_t force_write)
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

/**
 * @brief  应用刷新率（非强制模式）
 * @param  refresh_rate — 目标刷新率
 */
static void redpic1_thermal_apply_refresh_rate(uint8_t refresh_rate)
{
    redpic1_thermal_apply_refresh_rate_internal(refresh_rate, 0U);
}

static void redpic1_thermal_apply_data_hold(uint8_t enable)
{
    (void)MLX90640_SetDataHold(MLX90640_ADDR, enable);
}

/* =========================================================================
 *  20. 内部函数实现 —— 采集回调代理
 * ======================================================================= */

/** @brief  获取当前刷新率（供 capture 模块回调） */
static uint8_t redpic1_thermal_capture_get_refresh_rate(void)
{
    return s_refreshRate;
}

/** @brief  应用刷新率（供 capture 模块回调） */
static void redpic1_thermal_capture_apply_refresh_rate(uint8_t refresh_rate,
                                                       uint8_t force_write)
{
    redpic1_thermal_apply_refresh_rate_internal(refresh_rate, force_write);
}

/** @brief  失效历史（供 capture 模块回调） */
static void redpic1_thermal_capture_invalidate_history(void)
{
    redpic1_thermal_stage6l3_invalidate_history();
}

/* =========================================================================
 *  21. 内部函数实现 —— 诊断测试图案
 * ======================================================================= */

/**
 * @brief  构建诊断测试图案
 * @note   仅在诊断模式下送显，用于验证 display runtime 与 DMA 路径。
 *         图案包含：水平渐变、每 4 行反转、十字线、每 8 像素低灰度标记。
 */
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

            /* 每 4 行反转灰度 */
            if ((row & 0x04U) != 0U)
            {
                gray = (uint8_t)(255U - gray);
            }

            /* 中心十字线 */
            if (row == (REDPIC1_THERMAL_SRC_COLS / 2U) ||
                col == (REDPIC1_THERMAL_SRC_ROWS / 2U))
            {
                gray = 255U;
            }

            /* 每 8 像素标记 */
            if (((row + col) & 0x07U) == 0U)
            {
                gray = 32U;
            }

            s_diag_pattern_frame[index] = gray;
        }
    }

    s_diag_pattern_ready = 1U;
}

/* =========================================================================
 *  22. 内部函数实现 —— 灰度帧送显
 * ======================================================================= */

/**
 * @brief  通过 display runtime 提交一帧灰度图
 * @note   暂停、叠加占用或未使能时直接拒绝提交。
 * @param  gray_frame — 灰度帧数据（768 字节）
 * @retval 1 — 提交成功；0 — 提交失败
 */
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

/* =========================================================================
 *  23. 内部函数实现 —— 帧过期阈值与槽位管理
 * ======================================================================= */

/**
 * @brief  获取帧过期判定阈值（ms）
 * @note   阈值 = max(2 × 帧周期, DROP_EXPIRED_FRAME_MIN_MS)
 * @return 过期阈值（ms）
 */
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

/* 槽位操作代理函数 */
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

/**
 * @brief  丢弃非 inflight 槽位并重置采集状态
 */
static void redpic1_thermal_drop_non_inflight_slots(void)
{
    redpic1_thermal_frame_slot_drop_non_inflight();
    redpic1_thermal_capture_reset();
    redpic1_thermal_reset_processing_history();
}

/**
 * @brief  完整重置所有槽位与运行时缓存
 * @note   初始化阶段调用，确保所有状态回到初始值。
 */
static void redpic1_thermal_reset_slots(void)
{
    redpic1_thermal_frame_slot_reset();
    redpic1_thermal_capture_reset();
    redpic1_thermal_reset_processing_history();
}

/* =========================================================================
 *  24. 公共接口实现 —— 初始化
 * ======================================================================= */

/**
 * @brief  初始化热成像模块
 * @note   流程：传感器初始化 → 状态变量初始化 → 注册回调 → 重置槽位 → 构建诊断图案。
 */
void redpic1_thermal_init(void)
{
    redpic1_thermal_capture_ops_t capture_ops;
    redpic1_thermal_frame_slot_ops_t slot_ops;
    redpic1_thermal_visual_ops_t visual_ops;

    /* 等待 MLX90640 初始化成功（C2 修复：加重试上限防止死循环） */
    {
        #define MLX90640_INIT_MAX_RETRIES  50U
        uint32_t init_retry = 0U;
        while (mlx90640_init() != 0)
        {
            delay_ms(200);
            if (++init_retry >= MLX90640_INIT_MAX_RETRIES)
            {
                break;
            }
        }
    }

    /* 初始化状态变量 */
    s_colorMode            = 0U;
    s_runEnabled           = 1U;
    s_refreshRate          = REDPIC1_THERMAL_ACTIVE_REFRESH_RATE;
    s_overlayHold          = 0U;
    s_display_paused       = 0U;
    s_runtime_overlay_visible = 1U;
    memset(&s_latest_snapshot, 0, sizeof(s_latest_snapshot));
    redpic1_thermal_snapshot_status_reset();
    redpic1_thermal_reset_bottom_bar_cache();

    /* 应用调色板与刷新率 */
    set_color_mode(s_colorMode);
    redpic1_thermal_apply_refresh_rate_internal(s_refreshRate, 1U);

    /* 注册采集回调 */
    memset(&capture_ops, 0, sizeof(capture_ops));
    capture_ops.get_refresh_rate    = redpic1_thermal_capture_get_refresh_rate;
    capture_ops.apply_refresh_rate  = redpic1_thermal_capture_apply_refresh_rate;
    capture_ops.invalidate_history  = redpic1_thermal_capture_invalidate_history;
    redpic1_thermal_capture_init(&capture_ops);

    /* 初始化云端上传模块 */
    redpic1_thermal_cloud_init();

    /* 注册可视化回调 */
    memset(&visual_ops, 0, sizeof(visual_ops));
    visual_ops.get_active_period_ms = redpic1_thermal_get_active_period_ms;
    redpic1_thermal_visual_init(&visual_ops);

    /* 注册槽位回调 */
    memset(&slot_ops, 0, sizeof(slot_ops));
    slot_ops.enter_critical                 = redpic1_thermal_enter_critical;
    slot_ops.exit_critical                  = redpic1_thermal_exit_critical;
    slot_ops.get_tick_ms                    = redpic1_thermal_frame_slot_tick_ms;
    slot_ops.get_expired_frame_threshold_ms = redpic1_thermal_get_expired_frame_threshold_ms;
    slot_ops.present_gray_frame             = redpic1_thermal_present_gray_frame;
    slot_ops.run_enabled                    = redpic1_thermal_frame_slot_run_enabled;
    slot_ops.display_paused                 = redpic1_thermal_frame_slot_display_paused;
    slot_ops.overlay_hold                   = redpic1_thermal_frame_slot_overlay_hold;
    redpic1_thermal_frame_slot_init(&slot_ops);

    /* 重置槽位并构建诊断图案 */
    redpic1_thermal_reset_slots();
    redpic1_thermal_build_diag_pattern();
}

/* =========================================================================
 *  25. 公共接口实现 —— Display Runtime 绑定
 * ======================================================================= */

/**
 * @brief  绑定 display runtime 回调
 */
void redpic1_thermal_bind_display_runtime(void)
{
    redpic1_thermal_frame_slot_bind_display_runtime();
}

/* =========================================================================
 *  26. 公共接口实现 —— 帧周期查询
 * ======================================================================= */

/**
 * @brief  获取当前活跃帧周期（ms）
 * @return 帧周期（ms）
 */
uint32_t redpic1_thermal_get_active_period_ms(void)
{
    return redpic1_thermal_refresh_rate_to_period_ms(s_refreshRate);
}

/* =========================================================================
 *  27. 公共接口实现 —— 主循环单步执行
 * ======================================================================= */

/**
 * @brief  thermal task 单步执行入口
 * @note   固定按以下顺序运行：
 *         backoff 判定 → KEY2 快照处理 → 取帧 → 子页配对 → 数据校验 →
 *         灰度生成 → 对比度校验 → 发布槽位 → 提交送显。
 */
void redpic1_thermal_step(void)
{
    uint32_t step_start_cycle    = app_perf_baseline_cycle_now();
    uint32_t gray_start_cycle    = 0U;
    float    frame_min_temp      = 0.0f;
    float    frame_max_temp      = 0.0f;
    float    frame_center_temp   = 0.0f;

    /* 模块未使能：直接返回 */
    if (s_runEnabled == 0U)
    {
        app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
        return;
    }

    /* ---- KEY2 快照处理 ---- */
    if (s_display_paused != 0U &&
        s_key2_snapshot_pending != 0U &&
        s_key2_snapshot_in_progress == 0U)
    {
        storage_status_t snapshot_status = STORAGE_STATUS_OK;
        uint32_t saved_index = 0U;

        s_key2_snapshot_in_progress = 1U;
        s_key2_snapshot_pending     = 0U;
        s_key2_snapshot_status      = REDPIC1_THERMAL_SNAPSHOT_STATUS_SAVING;
        redpic1_thermal_draw_snapshot_status_overlay();

        /* 执行快照保存 */
        snapshot_status = snapshot_storage_save_latest(&saved_index);
        s_key2_snapshot_in_progress = 0U;
        s_key2_snapshot_status = (snapshot_status == STORAGE_STATUS_OK) ?
                                 REDPIC1_THERMAL_SNAPSHOT_STATUS_OK :
                                 REDPIC1_THERMAL_SNAPSHOT_STATUS_FAIL;
        redpic1_thermal_draw_snapshot_status_overlay();
    }

    /* ---- 主采集流程 ---- */
    {
        redpic1_thermal_frame_slot_t *back_slot           = 0;
        const float *gray_source_frame                     = 0;
        uint8_t  high_motion_frame                         = 0U;
        uint8_t  captured_subpage                          = 0U;
        uint32_t capture_tick_ms                           = 0U;
        uint32_t get_temp_elapsed_us                       = 0U;

#if REDPIC1_THERMAL_DIAG_MODE == REDPIC1_THERMAL_DIAG_MODE_TEST_PATTERN
        /* 诊断模式：直接送显测试图案 */
        redpic1_thermal_present_diag_pattern();
        app_perf_baseline_record_thermal_step_us(
            app_perf_baseline_elapsed_us(step_start_cycle));
        return;
#endif

        /* 步骤 1：退避判定 */
        if (redpic1_thermal_capture_prepare_step() == 0U)
        {
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        /* 步骤 2：获取 back 槽位 */
        back_slot = redpic1_thermal_get_back_slot();
        if (back_slot == 0)
        {
            app_perf_baseline_record_thermal_back_slot_null();
            redpic1_thermal_capture_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(
                app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        /* 步骤 3：读取帧数据 */
        if (redpic1_thermal_capture_read_frame(back_slot->temp_frame,
                                               &captured_subpage,
                                               &capture_tick_ms,
                                               &get_temp_elapsed_us) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            app_perf_baseline_record_thermal_step_us(
                app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        /* 步骤 4：子页配对合成 */
        if (redpic1_thermal_pair_try_compose(back_slot->temp_frame,
                                             captured_subpage,
                                             capture_tick_ms,
                                             get_temp_elapsed_us,
                                             app_perf_baseline_elapsed_us(step_start_cycle),
                                             &capture_tick_ms) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            app_perf_baseline_record_thermal_step_us(
                app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        /* 步骤 5：帧数据有效性校验 */
        if (redpic1_thermal_visual_frame_data_is_valid(back_slot->temp_frame) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            redpic1_thermal_capture_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(
                app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        /* 步骤 6：采集间隔检查，过大则失效历史 */
        if (redpic1_thermal_stage6l3_capture_gap_exceeded(capture_tick_ms) != 0U)
        {
            redpic1_thermal_stage6l3_invalidate_history();
        }

        /* 步骤 7：获取灰度源帧（含运动检测） */
        gray_source_frame = redpic1_thermal_visual_get_gray_source_frame(
            back_slot->temp_frame, &high_motion_frame);

        /* 步骤 8：生成灰度帧 */
        gray_start_cycle = app_perf_baseline_cycle_now();
        redpic1_thermal_visual_prepare_gray_frame(back_slot->temp_frame,
                                                  gray_source_frame,
                                                  high_motion_frame,
                                                  back_slot->gray_frame,
                                                  &frame_min_temp,
                                                  &frame_max_temp);
        app_perf_baseline_record_gray_us(
            app_perf_baseline_elapsed_us(gray_start_cycle));

        /* 步骤 9：获取中心温度 */
        frame_center_temp = redpic1_thermal_center_temp(back_slot->temp_frame);

        /* 步骤 10：温度范围校验 */
        if (redpic1_thermal_visual_frame_is_valid(frame_min_temp,
                                                  frame_max_temp,
                                                  frame_center_temp) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            redpic1_thermal_capture_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(
                app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        /* 步骤 11：灰度对比度校验 */
        if (redpic1_thermal_visual_gray_frame_has_contrast(back_slot->gray_frame) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            redpic1_thermal_capture_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(
                app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        /* 步骤 12：二次使能检查 */
        if (s_runEnabled == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            app_perf_baseline_record_thermal_step_us(
                app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        /* 步骤 13：填充槽位元数据 */
        back_slot->min_temp        = frame_min_temp;
        back_slot->max_temp        = frame_max_temp;
        back_slot->center_temp     = frame_center_temp;
        back_slot->capture_tick_ms = capture_tick_ms;
        back_slot->frame_seq       = redpic1_thermal_frame_slot_allocate_sequence();

        /* 步骤 14：记录性能基线 */
        app_perf_baseline_record_thermal_capture_success(capture_tick_ms,
                                                         frame_min_temp,
                                                         frame_max_temp,
                                                         frame_center_temp);

        /* 步骤 15：更新最新快照 */
        redpic1_thermal_update_latest_snapshot(back_slot->temp_frame,
                                               frame_min_temp,
                                               frame_max_temp,
                                               frame_center_temp,
                                               back_slot->frame_seq,
                                               capture_tick_ms);

        /* 步骤 16：发布槽位并提交送显 */
        redpic1_thermal_publish_back_slot(back_slot);
        if (s_display_paused == 0U && s_overlayHold == 0U)
        {
            (void)redpic1_thermal_frame_slot_submit_latest_ready();
        }

        /* 步骤 17：通知采集成功 */
        redpic1_thermal_capture_note_success();
        redpic1_thermal_visual_note_capture_success(capture_tick_ms);
    }

    app_perf_baseline_record_thermal_step_us(
        app_perf_baseline_elapsed_us(step_start_cycle));
}

/* =========================================================================
 *  28. 公共接口实现 —— 强制刷新
 * ======================================================================= */

/**
 * @brief  强制重显最近一帧 front 内容
 * @note   不触发新的热成像采集，仅重送最近稳定帧。
 */
void redpic1_thermal_force_refresh(void)
{
    redpic1_thermal_mark_bottom_bar_dirty(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE);

#if REDPIC1_THERMAL_DIAG_MODE == REDPIC1_THERMAL_DIAG_MODE_TEST_PATTERN
    redpic1_thermal_present_diag_pattern();
#else
    redpic1_thermal_present_front_slot();
#endif
}

/* =========================================================================
 *  29. 公共接口实现 —— 运行时叠加层渲染
 * ======================================================================= */

/**
 * @brief  渲染底部运行时叠加层
 * @note   状态栏刷新策略：
 *         - 不可见时：清除并返回
 *         - 首次可见：全量绘制
 *         - 内容变化：标记脏 → 差异刷新（250ms 防抖）
 *         - 强制/可见性变更：立即全量刷新
 */
void redpic1_thermal_render_runtime_overlay(void)
{
    redpic1_thermal_bottom_bar_view_t current_view;
    uint32_t now_ms = power_manager_get_tick_ms();

    /* 状态栏不可见：清除 */
    if (s_runtime_overlay_visible == 0U)
    {
        if (s_overlay_bar_last_visible != 0U || s_overlay_bar_last_view.valid != 0U)
        {
            redpic1_thermal_clear_bottom_bar();
            s_overlay_bar_last_visible    = 0U;
            s_overlay_bar_last_view.valid = 0U;
            redpic1_thermal_mark_bottom_bar_dirty(
                (uint8_t)(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE |
                          REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_VISIBLE));
        }
        return;
    }

    /* 构建当前视图 */
    redpic1_thermal_build_bottom_bar_view(&current_view);

    /* 首次可见：全量绘制 */
    if (s_overlay_bar_last_visible == 0U || s_overlay_bar_last_view.valid == 0U)
    {
        redpic1_thermal_draw_bottom_bar_bg();
        redpic1_thermal_draw_bottom_bar_values(&current_view);
        s_overlay_bar_last_view      = current_view;
        s_overlay_bar_last_visible   = 1U;
        redpic1_thermal_clear_bottom_bar_dirty();
        s_overlay_bar_last_refresh_ms = now_ms;
        return;
    }

    /* 内容变化检测 */
    if (redpic1_thermal_bottom_bar_view_equal(&current_view,
                                              &s_overlay_bar_last_view) == 0U)
    {
        redpic1_thermal_mark_bottom_bar_dirty(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_TEXT);
    }

    /* 无脏数据：跳过 */
    if (s_overlay_bar_pending_dirty == 0U)
    {
        return;
    }

    /* 防抖：非强制/可见性变更时，检查刷新间隔 */
    if (((s_overlay_bar_dirty_reason_mask &
          (REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE |
           REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_VISIBLE)) == 0U) &&
        ((uint32_t)(now_ms - s_overlay_bar_last_refresh_ms) <
         REDPIC1_THERMAL_OVERLAY_BAR_REFRESH_MS))
    {
        return;
    }

    /* 执行刷新 */
    if ((s_overlay_bar_dirty_reason_mask &
         (REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_FORCE |
          REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_VISIBLE)) != 0U)
    {
        /* 强制/可见性变更：全量刷新 */
        redpic1_thermal_draw_bottom_bar_bg();
        redpic1_thermal_draw_bottom_bar_values(&current_view);
    }
    else
    {
        /* 文本变更：差异刷新 */
        redpic1_thermal_draw_bottom_bar_diff(&current_view);
    }

    /* 更新缓存 */
    s_overlay_bar_last_view       = current_view;
    s_overlay_bar_last_visible    = 1U;
    redpic1_thermal_clear_bottom_bar_dirty();
    s_overlay_bar_last_refresh_ms = now_ms;
}

/* =========================================================================
 *  30. 公共接口实现 —— 状态栏可见性查询
 * ======================================================================= */

/**
 * @brief  查询运行时叠加层是否可见
 * @retval 1 — 可见；0 — 不可见
 */
uint8_t redpic1_thermal_runtime_overlay_visible(void)
{
    return s_runtime_overlay_visible;
}

/* =========================================================================
 *  31. 公共接口实现 —— 按键处理
 * ======================================================================= */

/**
 * @brief  处理热成像页本地按键
 * @note   KEY1: 顺时针切色板
 *         KEY2: 切暂停/恢复，暂停时可触发快照保存和云端上传
 *         KEY3: 逆时针切色板
 * @param  key_value — 按键值
 */
void redpic1_thermal_handle_key(uint8_t key_value)
{
    switch (key_value)
    {
    /* ---- KEY1: 顺时针切色板 ---- */
    case KEY1_PRES:
        s_colorMode++;
        if (s_colorMode > 4U)
        {
            s_colorMode = 0U;
        }
        set_color_mode(s_colorMode);
        redpic1_thermal_mark_bottom_bar_dirty(
            REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PALETTE);
        break;

    /* ---- KEY2: 暂停/恢复 + 快照 ---- */
    case KEY2_PRES:
    {
        uint8_t was_display_paused   = s_display_paused;
        uint8_t key2_snapshot_enabled = redpic1_thermal_key2_snapshot_enabled();

        /* 入页忽略窗口：避免入页按键尾波触发 */
        if (power_manager_get_tick_ms() < s_key2_ignore_until_ms)
        {
            break;
        }

        /* 已暂停且快照进行中：刷新状态显示 */
        if (was_display_paused != 0U && key2_snapshot_enabled != 0U)
        {
            if (s_key2_snapshot_pending != 0U || s_key2_snapshot_in_progress != 0U)
            {
                redpic1_thermal_draw_snapshot_status_overlay();
                break;
            }
        }

        /* 切换暂停状态 */
        s_display_paused = (uint8_t)!s_display_paused;

        if (s_display_paused != 0U)
        {
            /* 进入暂停：取消待送显 */
            redpic1_thermal_cancel_pending_present_and_clear_submit();

            /* 暂停时发送温度摘要到 ESP（如果启用） */
            if (was_display_paused == 0U &&
                redpic1_thermal_cloud_pause_send_esp_enabled() != 0U)
            {
                (void)redpic1_thermal_cloud_submit_snapshot_to_esp();
            }

            /* 暂停时触发快照（如果启用） */
            if (was_display_paused == 0U && key2_snapshot_enabled != 0U)
            {
                s_key2_snapshot_pending = 1U;
                s_key2_snapshot_status  = REDPIC1_THERMAL_SNAPSHOT_STATUS_QUEUED;
                redpic1_thermal_draw_snapshot_status_overlay();
            }
        }

        redpic1_thermal_mark_bottom_bar_dirty(
            REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PAUSE);
        redpic1_thermal_stage6l3_invalidate_history();

        /* 恢复送显：重置快照状态并尝试补提交 */
        if (was_display_paused != 0U && s_display_paused == 0U)
        {
            redpic1_thermal_snapshot_status_reset();
            redpic1_thermal_frame_slot_try_submit_if_possible();
        }
    }
        break;

    /* ---- KEY3: 逆时针切色板 ---- */
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
        redpic1_thermal_mark_bottom_bar_dirty(
            REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PALETTE);
        break;

    default:
        break;
    }
}

/* =========================================================================
 *  32. 公共接口实现 —— 挂起与恢复
 * ======================================================================= */

/**
 * @brief  挂起热成像采集与送显
 * @note   切低刷新率、取消待送显、丢弃本地缓存、释放 thermal 电源锁。
 */
void redpic1_thermal_suspend(void)
{
    s_runEnabled = 0U;
    redpic1_thermal_apply_refresh_rate(REDPIC1_THERMAL_IDLE_REFRESH_RATE);
    redpic1_thermal_apply_data_hold(1U);
    redpic1_thermal_snapshot_status_reset();
    redpic1_thermal_reset_bottom_bar_cache();

    redpic1_thermal_cancel_pending_present_and_clear_submit();
    redpic1_thermal_drop_non_inflight_slots();
    redpic1_thermal_cloud_reset();

    /* 确保诊断图案已构建 */
    if (s_diag_pattern_ready == 0U)
    {
        redpic1_thermal_build_diag_pattern();
    }

    power_manager_release_lock(POWER_LOCK_THERMAL);
}

/**
 * @brief  恢复热成像采集与送显
 * @note   恢复帧率、清除暂停/叠加持有状态、获取 thermal 电源锁、尝试补提交。
 */
void redpic1_thermal_resume(void)
{
    redpic1_thermal_apply_data_hold(0U);
    redpic1_thermal_apply_refresh_rate(REDPIC1_THERMAL_ACTIVE_REFRESH_RATE);
    s_runEnabled = 1U;
    s_key2_ignore_until_ms = power_manager_get_tick_ms() +
                             REDPIC1_THERMAL_KEY2_ENTRY_GUARD_MS;
    s_overlayHold    = 0U;
    s_display_paused = 0U;
    redpic1_thermal_snapshot_status_reset();
    redpic1_thermal_reset_bottom_bar_cache();

    redpic1_thermal_drop_non_inflight_slots();
    redpic1_thermal_cloud_reset();

    power_manager_acquire_lock(POWER_LOCK_THERMAL);
    power_manager_notify_activity();

    redpic1_thermal_frame_slot_try_submit_if_possible();
}

/* =========================================================================
 *  33. 公共接口实现 —— STOP 唤醒后总线恢复
 * ======================================================================= */

/**
 * @brief  STOP 唤醒后恢复 MLX90640 总线
 * @note   若调度器已运行，则设置延后恢复标志；否则立即恢复。
 */
void redpic1_thermal_restore_bus_after_stop(void)
{
    redpic1_thermal_capture_request_restore_after_stop(
        redpic1_thermal_scheduler_running());
}

/* =========================================================================
 *  34. 公共接口实现 —— 叠加层持有控制
 * ======================================================================= */

/**
 * @brief  控制 thermal 叠加层持有状态
 * @note   持有期间禁止新的送显提交；若已有待处理提交则立即取消。
 * @param  enabled — 1=持有；0=释放
 */
void redpic1_thermal_set_overlay_hold(uint8_t enabled)
{
    s_overlayHold = enabled;
    if (enabled != 0U)
    {
        redpic1_thermal_cancel_pending_present_and_clear_submit();
    }
}

/* =========================================================================
 *  35. 公共接口实现 —— 快照拷贝
 * ======================================================================= */

/**
 * @brief  拷贝最新热成像快照（线程安全）
 * @param  out_snapshot — 输出：快照数据指针
 * @retval 1 — 拷贝成功；0 — 无有效快照或参数为空
 */
uint8_t redpic1_thermal_copy_latest_snapshot(redpic1_thermal_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0)
    {
        return 0U;
    }

    redpic1_thermal_enter_critical();

    if (s_latest_snapshot.valid == 0U)
    {
        redpic1_thermal_exit_critical();
        return 0U;
    }

    memcpy(out_snapshot, &s_latest_snapshot, sizeof(*out_snapshot));
    redpic1_thermal_exit_critical();

    return 1U;
}
