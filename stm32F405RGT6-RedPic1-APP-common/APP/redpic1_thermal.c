#include "redpic1_thermal.h"

#include <stdio.h>
#include <string.h>
#include "math.h"

#include "FreeRTOS.h"
#include "task.h"

#include "app_display_runtime.h"
#include "app_perf_baseline.h"
#include "redpic1_app.h"
#include "delay.h"
#include "key.h"
#include "lcd.h"
#include "lcd_utf8.h"
#include "lcd_init.h"
#include "power_manager.h"
#include "MLX90640_I2C_Driver.h"
#include "MLX90640_API.h"
#include "MLX90640.h"
#include "lcd_dma.h"

#define REDPIC1_THERMAL_STAGE6L_2_MOTION_HOT_DELTA_C   0.60f
#define REDPIC1_THERMAL_STAGE6L_2_MOTION_MAX_DELTA_C   1.50f
#define REDPIC1_THERMAL_STAGE6L_2_MOTION_HOT_PIXELS    64U

#if (REDPIC1_THERMAL_STAGE6L_ENABLE != 0U) && (REDPIC1_THERMAL_STAGE6L_1_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGE6L_1_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGE6L_1_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6L_1_ACTIVE != 0U) && (REDPIC1_THERMAL_STAGE6L_1_WINDOW_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGE6L_1_WINDOW_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGE6L_1_WINDOW_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6L_1_ACTIVE != 0U) && (REDPIC1_THERMAL_STAGE6L_1_FILTER_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGE6L_1_FILTER_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGE6L_1_FILTER_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6L_1_ACTIVE != 0U)
    #define REDPIC1_THERMAL_FILTER_PATH_ENABLED REDPIC1_THERMAL_STAGE6L_1_FILTER_ACTIVE
    #define REDPIC1_THERMAL_WINDOW_PATH_ENABLED REDPIC1_THERMAL_STAGE6L_1_WINDOW_ACTIVE
#else
    #define REDPIC1_THERMAL_FILTER_PATH_ENABLED 1U
    #define REDPIC1_THERMAL_WINDOW_PATH_ENABLED 1U
#endif

#if (REDPIC1_THERMAL_STAGE6L_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6L_1_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6L_2_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGE6L_2_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGE6L_2_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6L_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6L_1_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6L_3_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGE6L_3_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGE6L_3_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6L_2_ACTIVE != 0U)
    #define REDPIC1_THERMAL_STAGEP5_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGEP5_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6L_3_ACTIVE != 0U)
    #define REDPIC1_THERMAL_STAGEP6_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGEP6_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6L_1_ACTIVE != 0U) && \
    (REDPIC1_THERMAL_STAGEP5_ACTIVE != 0U) && \
    (REDPIC1_THERMAL_STAGEP7_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGEP7_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGEP7_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6R_ENABLE != 0U) && (REDPIC1_THERMAL_STAGE6R_1_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGE6R_1_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGE6R_1_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6R_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_1_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_2_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGE6R_2_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGE6R_2_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6R_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_1_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_2_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_3_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGE6R_3_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGE6R_3_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_DROP_EXPIRED_FRAME_ENABLE != 0U)
    #define REDPIC1_THERMAL_DROP_EXPIRED_FRAME_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_DROP_EXPIRED_FRAME_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_OVERLAY_DIRTY_ONLY_ENABLE != 0U)
    #define REDPIC1_THERMAL_OVERLAY_DIRTY_ONLY_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_OVERLAY_DIRTY_ONLY_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ENABLE != 0U)
    #define REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ENABLE != 0U)
    #define REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ACTIVE 1U
#else
    #define REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ACTIVE 0U
#endif

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
#if (REDPIC1_THERMAL_32HZ_AB_TEST_ENABLE != 0U)
    #define REDPIC1_THERMAL_ACTIVE_REFRESH_RATE        FPS32HZ
#else
    #define REDPIC1_THERMAL_ACTIVE_REFRESH_RATE        RefreshRate
#endif
#define REDPIC1_THERMAL_IDLE_REFRESH_RATE              FPS1HZ       ///< 休眠/低功耗模式帧率
#define REDPIC1_THERMAL_SRC_ROWS                       24U          ///< MLX90640 原始分辨率行数
#define REDPIC1_THERMAL_SRC_COLS                       32U          ///< MLX90640 原始分辨率列数
#define REDPIC1_THERMAL_PIXEL_COUNT                    768U         ///< 总像素数 (24*32)
#define REDPIC1_THERMAL_VALID_TEMP_MIN_C               (-40.0f)     ///< 有效温度下限
#define REDPIC1_THERMAL_VALID_TEMP_MAX_C               (300.0f)     ///< 有效温度上限
#define REDPIC1_THERMAL_VALID_MIN_SPAN_C               (0.5f)       ///< 最小有效温差跨度，低于此值视为无效帧

/* 总线退避与恢复策略 */
#define REDPIC1_THERMAL_BACKOFF_MS                     20UL         ///< I2C 失败后退避时长 (ms)
#define REDPIC1_THERMAL_RESTORE_THRESHOLD              3U           ///< 连续传输失败阈值，触发总线硬复位
#define MLX90640_REG_STATUS                            0x8000U      ///< MLX90640 状态寄存器地址 (替换魔法数字)
#define MLX90640_STATUS_DATA_READY_MASK                (1U << 3)    ///< 数据就绪标志位 (替换 0x0008U)

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

/* 异步槽位管理参数 */
#define REDPIC1_THERMAL_SLOT_INDEX_NONE                0xFFU        ///< 无效槽位索引标记
#define REDPIC1_THERMAL_TOKEN_SHIFT                    8U           ///< 异步 Token 移位基数
#define REDPIC1_THERMAL_TOKEN_SLOT_MASK                0xFFU        ///< 异步 Token 槽位索引掩码
#define REDPIC1_THERMAL_SLOT_COUNT                     3U           ///< 帧缓冲槽位总数 (双缓冲+1备用)

/* 显示窗口平滑策略参数 */
#define REDPIC1_THERMAL_DISPLAY_WINDOW_MIN_SPAN_C      1.5f         ///< 强制保留的最小显示温差跨度，防止灰度映射失效
#define REDPIC1_THERMAL_DISPLAY_WINDOW_EMA_ALPHA       REDPIC1_THERMAL_STAGEP7_NORMAL_EMA_ALPHA
#define REDPIC1_THERMAL_DISPLAY_WINDOW_MAX_STEP_C      REDPIC1_THERMAL_STAGEP7_NORMAL_MAX_STEP_C
#define REDPIC1_THERMAL_DISPLAY_WINDOW_FAST_EMA_ALPHA  REDPIC1_THERMAL_STAGEP7_HIGH_MOTION_EMA_ALPHA
#define REDPIC1_THERMAL_DISPLAY_WINDOW_FAST_MAX_STEP_C REDPIC1_THERMAL_STAGEP7_HIGH_MOTION_MAX_STEP_C

/* ========================================================================= */
/*  数据结构定义 (Data Types)                                                */
/* ========================================================================= */

/**
 * @enum redpic1_thermal_frame_slot_state_t
 * @brief 帧槽位状态机枚举。
 * @details 状态迁移规则：FREE(空闲) -> WRITING(采集写入中) -> READY(待送显) 
 *          -> INFLIGHT(DMA/显示处理中) -> FRONT(已稳定显示在屏幕最前端)
 * @note READY/INFLIGHT/FRONT 在任意时刻至多各有一个槽位占用，避免资源竞争。
 */
typedef enum
{
    REDPIC1_THERMAL_FRAME_SLOT_FREE = 0,       ///< 空闲：可被采集任务分配
    REDPIC1_THERMAL_FRAME_SLOT_WRITING,        ///< 写入中：I2C 正在填充温度数据
    REDPIC1_THERMAL_FRAME_SLOT_READY,          ///< 就绪：数据完整，等待 Display Runtime 认领
    REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT,       ///< 飞行中：已被 Display 认领，正在 DMA 搬运或刷新
    REDPIC1_THERMAL_FRAME_SLOT_FRONT           ///< 前沿：已成功上屏，可作为“强制刷新”的稳定源
} redpic1_thermal_frame_slot_state_t;

/**
 * @struct redpic1_thermal_frame_slot_t
 * @brief 单个帧槽位的数据结构。
 * @note 必须分配在系统 SRAM (非 CCMRAM)，因异步 Present 路径延长帧生命周期，
 *       系统 SRAM 对 DMA 和跨任务访问更安全。
 */
typedef struct
{
    float   temp_frame[REDPIC1_THERMAL_PIXEL_COUNT];   ///< 原始采集温度数据 (单位: ℃)
    uint8_t gray_frame[REDPIC1_THERMAL_PIXEL_COUNT];   ///< 映射后的灰度显示数据 (0~255)
    float   min_temp;                                  ///< 本帧统计最低温
    float   max_temp;                                  ///< 本帧统计最高温
    float   center_temp;                               ///< 本帧中心点温度
    uint32_t capture_tick_ms;                          ///< 采集完成时的系统 Tick (用于计算 FPS)
    uint32_t ready_tick_ms;                            ///< READY 发布 Tick，用于过期帧判定
    uint32_t frame_seq;                                ///< 帧序列号，用于异步 Token 校验与防重放
    uint8_t valid;                                     ///< 数据有效性标志
    redpic1_thermal_frame_slot_state_t slot_state;     ///< 当前槽位状态机状态
} redpic1_thermal_frame_slot_t;

/* ========================================================================= */
/*  静态全局变量 (Static Globals)                                            */
/* ========================================================================= */

/* [槽位管理区] 帧缓冲池与异步提交指针 */
static redpic1_thermal_frame_slot_t s_frame_slots[REDPIC1_THERMAL_SLOT_COUNT]; ///< 三槽位帧缓冲池 (SRAM)
static uint32_t s_frame_sequence = 0U;                                         ///< 全局递增帧序列号
static uint8_t  s_front_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;          ///< 当前稳定显示在屏幕的槽位索引
static uint8_t  s_ready_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;          ///< 等待被 Display 认领的槽位索引
static uint8_t  s_inflight_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;       ///< 正在 DMA/显示链路中的槽位索引
static uintptr_t s_last_submitted_token = 0U;                                  ///< 最近一次提交给 Display 的 Token
static uint8_t   s_last_submitted_valid = 0U;                                  ///< Token 有效性标志 (防重复提交)

/* [总线与退避区] I2C 通信状态与恢复策略 */
static uint32_t s_backoff_until_ms = 0U;                                       ///< 退避截止时间戳 (ms)
static uint8_t  s_restore_bus_pending = 0U;                                    ///< 延时恢复总线标志 (用于 STOP 唤醒后安全上下文恢复)
static uint8_t  s_consecutive_transport_failures = 0U;                         ///< 连续 I2C 传输失败计数器

/* [显示控制与 UI 区] 运行时状态与叠加层缓存 */
static uint8_t  s_display_paused = 0U;                                         ///< 暂停送显标志 (仅本模块可见)
static uint8_t  s_runEnabled = 1U;                                             ///< 模块总使能开关
static uint8_t  s_refreshRate = REDPIC1_THERMAL_ACTIVE_REFRESH_RATE;           ///< 当前硬件帧率配置
static uint8_t  s_overlayHold = 0U;                                            ///< UI 叠加层持有标志 (持有时禁止新帧提交)
static uint8_t  s_frameReady = 0U;                                             ///< 是否有可用前沿帧 (Front Slot)
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

/* [滤波与窗口平滑区] 温度数据后处理缓存 */
static float    s_display_min_temp = 0.0f;                                     ///< 平滑后的显示窗口下限
static float    s_display_max_temp = 0.0f;                                     ///< 平滑后的显示窗口上限
static uint8_t  s_display_window_valid = 0U;                                   ///< 窗口平滑状态初始化标志
static CCMRAM float s_previous_filtered_temp_frame[REDPIC1_THERMAL_PIXEL_COUNT]; ///< 上一帧滤波结果 (CCM RAM 提速)
static CCMRAM float s_current_visual_temp_frame[REDPIC1_THERMAL_PIXEL_COUNT];    ///< 当前帧自适应滤波结果 (CCM RAM)
static uint8_t  s_filter_history_valid = 0U;                                   ///< 历史滤波数据有效性标志
static CCMRAM uint8_t s_diag_pattern_frame[REDPIC1_THERMAL_PIXEL_COUNT];       ///< 诊断测试图案灰度数据 (CCM RAM)

#if REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ACTIVE
static CCMRAM float s_v4_subpage_temp_frame[2][REDPIC1_THERMAL_PIXEL_COUNT];
static uint32_t s_v4_subpage_tick_ms[2] = { 0U, 0U };
static uint8_t  s_v4_subpage_valid[2] = { 0U, 0U };
#endif

static void redpic1_thermal_free_slot_locked(uint8_t slot_index);
/* ========================================================================= */
/*  基础辅助函数 (Core Helpers)                                              */
/* ========================================================================= */


/**
 * @brief 检查 FreeRTOS 调度器是否处于运行状态。
 * @return 1U 表示调度器运行中，0U 表示未启动或已挂起。
 * @note 用于临界区保护的安全判断，避免在调度器未启动时调用 taskENTER_CRITICAL。
 */
static uint32_t s_last_capture_tick_ms = 0U;

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
#if (REDPIC1_THERMAL_OVERLAY_DIRTY_ONLY_ACTIVE != 0U)
    s_overlay_bar_dirty_reason_mask = (uint8_t)(s_overlay_bar_dirty_reason_mask | reason_mask);
#else
    (void)reason_mask;
#endif
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
    uint16_t center_row = REDPIC1_THERMAL_SRC_ROWS / 2U;
    uint16_t center_col = REDPIC1_THERMAL_SRC_COLS / 2U;

    if (frame_data == 0)
    {
        return 0.0f;
    }

    return frame_data[(center_row * REDPIC1_THERMAL_SRC_COLS) + center_col];
}

static int16_t redpic1_thermal_temp_to_x10(float temp)
{
    int32_t scaled = 0;

    scaled = (temp >= 0.0f) ?
             (int32_t)(temp * 10.0f + 0.5f) :
             (int32_t)(temp * 10.0f - 0.5f);

    if (scaled > 32767)
    {
        scaled = 32767;
    }
    else if (scaled < -32768)
    {
        scaled = -32768;
    }

    return (int16_t)scaled;
}

static uint8_t redpic1_thermal_submit_snapshot_to_esp(void)
{
    app_perf_baseline_snapshot_t snapshot;
    app_service_cmd_t cmd;
    int16_t min_temp_x10 = 0;
    int16_t max_temp_x10 = 0;
    int16_t center_temp_x10 = 0;

    app_perf_baseline_get_snapshot(&snapshot);
    if (snapshot.thermal_capture_frames == 0U)
    {
        return 0U;
    }

    min_temp_x10 = redpic1_thermal_temp_to_x10(snapshot.latest_min_temp);
    max_temp_x10 = redpic1_thermal_temp_to_x10(snapshot.latest_max_temp);
    center_temp_x10 = redpic1_thermal_temp_to_x10(snapshot.latest_center_temp);

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SEND_THERMAL_SNAPSHOT;
    cmd.value = ((uint32_t)(uint16_t)min_temp_x10) |
                (((uint32_t)(uint16_t)max_temp_x10) << 16);
    cmd.arg0 = (uint8_t)((uint16_t)center_temp_x10 & 0xFFU);
    cmd.arg1 = (uint8_t)(((uint16_t)center_temp_x10 >> 8) & 0xFFU);

    return app_service_submit_async(&cmd);
}

static uint8_t redpic1_thermal_pause_send_esp_enabled(void)
{
#if (REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ACTIVE != 0U)
    device_settings_t settings;

    app_rtos_settings_copy(&settings);
    return settings.thermal_pause_send_esp_enabled;
#else
    return 0U;
#endif
}

/* 重置显示温度窗口平滑状态。
 * 在 suspend/resume 或槽位状态整体丢弃后调用，避免旧窗口继续影响新帧映射。 */
static void redpic1_thermal_reset_display_window_state(void)
{
    s_display_min_temp = 0.0f;
    s_display_max_temp = 0.0f;
    s_display_window_valid = 0U;
}

/* 对显示窗口变化量做限幅。
 * 这样可以保留当前温度窗口平滑策略，不让单帧异常值导致画面剧烈跳变。 */
static float redpic1_thermal_limit_display_window_step(float current_value,
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

/* 保证显示温度窗口至少保留最小跨度。
 * 该约束仅影响灰度映射范围，不改变采集出的真实温度值。 */
static void redpic1_thermal_enforce_display_window_min_span(float *window_min_temp,
                                                            float *window_max_temp)
{
    float center_temp = 0.0f;
    float half_span = REDPIC1_THERMAL_DISPLAY_WINDOW_MIN_SPAN_C * 0.5f;

    if (window_min_temp == 0 || window_max_temp == 0)
    {
        return;
    }

    if ((*window_max_temp - *window_min_temp) >= REDPIC1_THERMAL_DISPLAY_WINDOW_MIN_SPAN_C)
    {
        return;
    }

    center_temp = (*window_min_temp + *window_max_temp) * 0.5f;
    *window_min_temp = center_temp - half_span;
    *window_max_temp = center_temp + half_span;
}

/* 根据原始温度范围计算平滑后的显示窗口。
 * 这里固定沿用 Stage6V_3 的窗口平滑策略，避免每帧 min/max 抖动直接传到显示层。 */

/* 提取出半跨度的常量，由预编译期计算完成 */
#define REDPIC1_THERMAL_DISPLAY_WINDOW_HALF_SPAN_C  (REDPIC1_THERMAL_DISPLAY_WINDOW_MIN_SPAN_C * 0.5f)

static void redpic1_thermal_get_display_window_response(uint8_t high_motion_frame,
                                                        float *out_ema_alpha,
                                                        float *out_max_step_c)
{
    float ema_alpha = REDPIC1_THERMAL_DISPLAY_WINDOW_EMA_ALPHA;
    float max_step_c = REDPIC1_THERMAL_DISPLAY_WINDOW_MAX_STEP_C;

#if REDPIC1_THERMAL_STAGEP7_ACTIVE
    if (high_motion_frame != 0U)
    {
        ema_alpha = REDPIC1_THERMAL_DISPLAY_WINDOW_FAST_EMA_ALPHA;
        max_step_c = REDPIC1_THERMAL_DISPLAY_WINDOW_FAST_MAX_STEP_C;
    }
#else
    (void)high_motion_frame;
#endif

    if (out_ema_alpha != 0)
    {
        *out_ema_alpha = ema_alpha;
    }

    if (out_max_step_c != 0)
    {
        *out_max_step_c = max_step_c;
    }
}

static void redpic1_thermal_get_display_window(float raw_min_temp,
                                               float raw_max_temp,
                                               uint8_t high_motion_frame,
                                               float *out_display_min_temp,
                                               float *out_display_max_temp)
{
#if REDPIC1_THERMAL_WINDOW_PATH_ENABLED
    float ema_alpha = REDPIC1_THERMAL_DISPLAY_WINDOW_EMA_ALPHA;
    float max_step_c = REDPIC1_THERMAL_DISPLAY_WINDOW_MAX_STEP_C;

    /* 优化点1：无分支编程 (Branchless)。利用硬件 FPU 的 fmaxf 指令替代 if 判断 */
    float center_temp = (raw_min_temp + raw_max_temp) * 0.5f;
    float half_span = fmaxf((raw_max_temp - raw_min_temp) * 0.5f, REDPIC1_THERMAL_DISPLAY_WINDOW_HALF_SPAN_C);
    
    float target_min_temp = center_temp - half_span;
    float target_max_temp = center_temp + half_span;

    if (s_display_window_valid == 0U)
    {
        s_display_min_temp = target_min_temp;
        s_display_max_temp = target_max_temp;
        s_display_window_valid = 1U;
    }
    else
    {
        redpic1_thermal_get_display_window_response(high_motion_frame,
                                                    &ema_alpha,
                                                    &max_step_c);

        // EMA 指数平滑计算
        float ema_min_temp = s_display_min_temp + ((target_min_temp - s_display_min_temp) * ema_alpha);
        float ema_max_temp = s_display_max_temp + ((target_max_temp - s_display_max_temp) * ema_alpha);

        // 步长限幅
        s_display_min_temp = redpic1_thermal_limit_display_window_step(s_display_min_temp,
                                                                       ema_min_temp,
                                                                       max_step_c);
        s_display_max_temp = redpic1_thermal_limit_display_window_step(s_display_max_temp,
                                                                       ema_max_temp,
                                                                       max_step_c);

        /* 优化点2：直接用 FPU 计算抹掉外部函数 redpic1_thermal_enforce_display_window_min_span */
        float cur_center = (s_display_min_temp + s_display_max_temp) * 0.5f;
        float cur_half_span = fmaxf((s_display_max_temp - s_display_min_temp) * 0.5f, REDPIC1_THERMAL_DISPLAY_WINDOW_HALF_SPAN_C);
        
        s_display_min_temp = cur_center - cur_half_span;
        s_display_max_temp = cur_center + cur_half_span;

        /* 兜底保护 */
        if (s_display_max_temp <= s_display_min_temp)
        {
            s_display_min_temp = target_min_temp;
            s_display_max_temp = target_max_temp;
        }
    }

    /* 优化点3：去掉不必要的判空。关键帧渲染路径直接赋值，省去流水线预测失败开销 */
    *out_display_min_temp = s_display_min_temp;
    *out_display_max_temp = s_display_max_temp;

#else
    (void)high_motion_frame;
    *out_display_min_temp = raw_min_temp;
    *out_display_max_temp = raw_max_temp;
#endif
}


/* 重置视觉滤波历史。
 * 在热成像恢复、停止唤醒后清空历史，避免旧帧残影参与下一轮滤波。 */
static void redpic1_thermal_reset_visual_filter_state(void)
{
    s_filter_history_valid = 0U;
}

static void redpic1_thermal_reset_processing_history(void)
{
    redpic1_thermal_reset_display_window_state();
    redpic1_thermal_reset_visual_filter_state();
    s_last_capture_tick_ms = 0U;
#if REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ACTIVE
    s_v4_subpage_tick_ms[0] = 0U;
    s_v4_subpage_tick_ms[1] = 0U;
    s_v4_subpage_valid[0] = 0U;
    s_v4_subpage_valid[1] = 0U;
#endif
}

#if REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ACTIVE
static uint8_t redpic1_thermal_stagev4_c1_pixel_subpage(uint16_t pixel_index)
{
    uint16_t row = (uint16_t)(pixel_index / REDPIC1_THERMAL_SRC_COLS);
    uint16_t col = (uint16_t)(pixel_index % REDPIC1_THERMAL_SRC_COLS);

    return (uint8_t)((row ^ col) & 0x01U);
}

static uint8_t redpic1_thermal_stagev4_c1_try_compose(float *frame_data,
                                                      uint8_t subpage,
                                                      uint32_t capture_tick_ms,
                                                      uint32_t *out_capture_tick_ms)
{
    uint8_t other_subpage = (uint8_t)(subpage ^ 0x01U);
    uint16_t pixel_index = 0U;

    if (frame_data == 0 || subpage > 1U)
    {
        return 0U;
    }

    memcpy(s_v4_subpage_temp_frame[subpage],
           frame_data,
           sizeof(s_v4_subpage_temp_frame[subpage]));
    s_v4_subpage_tick_ms[subpage] = capture_tick_ms;
    s_v4_subpage_valid[subpage] = 1U;

    if (s_v4_subpage_valid[other_subpage] != 0U &&
        (capture_tick_ms - s_v4_subpage_tick_ms[other_subpage]) > REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_MAX_AGE_MS)
    {
        s_v4_subpage_valid[other_subpage] = 0U;
        s_v4_subpage_tick_ms[other_subpage] = 0U;
    }

    if (s_v4_subpage_valid[other_subpage] == 0U)
    {
        return 0U;
    }

    for (pixel_index = 0U; pixel_index < REDPIC1_THERMAL_PIXEL_COUNT; ++pixel_index)
    {
        uint8_t owner_subpage = redpic1_thermal_stagev4_c1_pixel_subpage(pixel_index);
        frame_data[pixel_index] = s_v4_subpage_temp_frame[owner_subpage][pixel_index];
    }

    if (out_capture_tick_ms != 0)
    {
        *out_capture_tick_ms = (s_v4_subpage_tick_ms[subpage] >= s_v4_subpage_tick_ms[other_subpage]) ?
                               s_v4_subpage_tick_ms[subpage] :
                               s_v4_subpage_tick_ms[other_subpage];
    }

    return 1U;
}
#endif

static void redpic1_thermal_stage6l3_invalidate_history(void)
{
#if REDPIC1_THERMAL_STAGEP6_ACTIVE
    redpic1_thermal_reset_processing_history();
#endif
}

static void redpic1_thermal_stage6l2_adopt_raw_visual_history(const float *raw_frame_data)
{
#if REDPIC1_THERMAL_FILTER_PATH_ENABLED
    uint16_t i = 0U;

    if (raw_frame_data == 0)
    {
        return;
    }

    for (i = 0U; i < REDPIC1_THERMAL_PIXEL_COUNT; ++i)
    {
        s_current_visual_temp_frame[i] = raw_frame_data[i];
        s_previous_filtered_temp_frame[i] = raw_frame_data[i];
    }
    s_filter_history_valid = 1U;
#else
    (void)raw_frame_data;
#endif
}

static uint8_t redpic1_thermal_stage6l3_capture_gap_exceeded(uint32_t capture_tick_ms)
{
#if REDPIC1_THERMAL_STAGEP6_ACTIVE
    uint32_t active_period_ms = redpic1_thermal_get_active_period_ms();

    if (s_last_capture_tick_ms == 0U || active_period_ms == 0U)
    {
        return 0U;
    }

    return ((capture_tick_ms - s_last_capture_tick_ms) > (active_period_ms * 2U)) ? 1U : 0U;
#else
    (void)capture_tick_ms;
    return 0U;
#endif
}

/* 生成用于显示的视觉温度帧。
 * 固定保留 Stage6V_4 的视觉滤波策略，只改变灰度映射输入，不改原始温度统计。 */
static const float *redpic1_thermal_get_visual_frame(const float *raw_frame_data,
                                                     uint8_t *out_high_motion_frame)
{
    uint16_t i = 0U;

    if (out_high_motion_frame != 0)
    {
        *out_high_motion_frame = 0U;
    }

    if (raw_frame_data == 0)
    {
        return 0;
    }

#if REDPIC1_THERMAL_FILTER_PATH_ENABLED
    if (s_filter_history_valid == 0U)
    {
        redpic1_thermal_stage6l2_adopt_raw_visual_history(raw_frame_data);
        return s_current_visual_temp_frame;
    }

#if REDPIC1_THERMAL_STAGEP5_ACTIVE
    {
        float max_abs_delta = 0.0f;
        uint16_t hot_pixel_count = 0U;

        for (i = 0U; i < REDPIC1_THERMAL_PIXEL_COUNT; ++i)
        {
            float raw_temp = raw_frame_data[i];
            float prev_temp = s_previous_filtered_temp_frame[i];
            float delta = raw_temp - prev_temp;
            float abs_delta = delta;
            float current_weight = 1.0f;
            float filtered_temp = 0.0f;

            if (abs_delta < 0.0f)
            {
                abs_delta = -abs_delta;
            }

            if (abs_delta > max_abs_delta)
            {
                max_abs_delta = abs_delta;
            }

            if (abs_delta >= REDPIC1_THERMAL_STAGE6L_2_MOTION_HOT_DELTA_C)
            {
                hot_pixel_count++;
            }

            if (abs_delta <= 0.20f)
            {
                current_weight = 0.40f;
            }
            else if (abs_delta < 1.00f)
            {
                current_weight = 0.40f + (((abs_delta - 0.20f) / 0.80f) * 0.60f);
            }

            filtered_temp = prev_temp + ((raw_temp - prev_temp) * current_weight);
            s_current_visual_temp_frame[i] = filtered_temp;
            s_previous_filtered_temp_frame[i] = filtered_temp;
        }

        if (max_abs_delta >= REDPIC1_THERMAL_STAGE6L_2_MOTION_MAX_DELTA_C ||
            hot_pixel_count >= REDPIC1_THERMAL_STAGE6L_2_MOTION_HOT_PIXELS)
        {
            if (out_high_motion_frame != 0)
            {
                *out_high_motion_frame = 1U;
            }

            redpic1_thermal_stage6l2_adopt_raw_visual_history(raw_frame_data);
            return raw_frame_data;
        }
    }
#else
    for (i = 0U; i < REDPIC1_THERMAL_PIXEL_COUNT; ++i)
    {
        float raw_temp = raw_frame_data[i];
        float prev_temp = s_previous_filtered_temp_frame[i];
        float delta = raw_temp - prev_temp;
        float abs_delta = delta;
        float current_weight = 1.0f;
        float filtered_temp = 0.0f;

        if (abs_delta < 0.0f)
        {
            abs_delta = -abs_delta;
        }

        if (abs_delta <= 0.20f)
        {
            current_weight = 0.40f;
        }
        else if (abs_delta < 1.00f)
        {
            current_weight = 0.40f + (((abs_delta - 0.20f) / 0.80f) * 0.60f);
        }

        filtered_temp = prev_temp + ((raw_temp - prev_temp) * current_weight);
        s_current_visual_temp_frame[i] = filtered_temp;
        s_previous_filtered_temp_frame[i] = filtered_temp;
    }
#endif

    return s_current_visual_temp_frame;
#else
    return raw_frame_data;
#endif
}

/* 选择用于灰度映射的温度源帧。
 * 当前版本固定使用视觉滤波后的帧作为显示输入，统计值仍以原始采集帧为准。 */
static const float *redpic1_thermal_get_gray_source_frame(const float *raw_frame_data,
                                                          uint8_t *out_high_motion_frame)
{
    const float *visual_frame = redpic1_thermal_get_visual_frame(raw_frame_data,
                                                                 out_high_motion_frame);

    return (visual_frame != 0) ? visual_frame : raw_frame_data;
}

/* 根据温度帧生成显示灰度帧。
 * 固定保留显示窗口平滑、视觉滤波输入和 Stage6_6A 的转置写入路径，保证现有效果不变。 */
static void redpic1_thermal_prepare_gray_frame(const float *raw_frame_data,
                                               const float *display_frame_data,
                                               uint8_t high_motion_frame,
                                               uint8_t *gray_frame,
                                               float *out_min_temp,
                                               float *out_max_temp)
{
    float raw_min_temp = 300.0f;
    float raw_max_temp = -40.0f;
    float display_min_temp = 0.0f;
    float display_max_temp = 0.0f;
    float scale = 0.0f;
    uint16_t i = 0U;

    if (raw_frame_data == 0 || display_frame_data == 0 || gray_frame == 0)
    {
        return;
    }

    for (i = 0U; i < REDPIC1_THERMAL_PIXEL_COUNT; ++i)
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

    if (raw_max_temp <= raw_min_temp)
    {
        for (i = 0U; i < REDPIC1_THERMAL_PIXEL_COUNT; ++i)
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

#if REDPIC1_THERMAL_STAGEP7_ACTIVE
    redpic1_thermal_get_display_window(raw_min_temp,
                                       raw_max_temp,
                                       high_motion_frame,
                                       &display_min_temp,
                                       &display_max_temp);
#else
    if (high_motion_frame != 0U)
    {
        display_min_temp = raw_min_temp;
        display_max_temp = raw_max_temp;
    }
    else
    {
        redpic1_thermal_get_display_window(raw_min_temp,
                                           raw_max_temp,
                                           0U,
                                           &display_min_temp,
                                           &display_max_temp);
    }
#endif

    if (display_max_temp <= display_min_temp)
    {
        display_min_temp = raw_min_temp;
        display_max_temp = raw_max_temp;
    }

    scale = 255.0f / (display_max_temp - display_min_temp);

    for (uint16_t src_row = 0U; src_row < REDPIC1_THERMAL_SRC_ROWS; ++src_row)
    {
        const float *src = display_frame_data + ((uint32_t)src_row * REDPIC1_THERMAL_SRC_COLS);
        uint8_t *dst = gray_frame + src_row;

        for (uint16_t src_col = 0U; src_col < REDPIC1_THERMAL_SRC_COLS; ++src_col)
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
            dst += REDPIC1_THERMAL_SRC_ROWS;
        }
    }

    if (out_min_temp != 0)
    {
        *out_min_temp = raw_min_temp;
    }
    if (out_max_temp != 0)
    {
        *out_max_temp = raw_max_temp;
    }
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

/* 判断 backoff 截止时间是否已到。
 * 使用带符号差值比较，避免 tick 回绕时出现误判。 */
static uint8_t redpic1_thermal_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (((int32_t)(now_ms - deadline_ms)) >= 0) ? 1U : 0U;
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


/* 判断单个温度值是否落在可接受范围。 */
static uint8_t redpic1_thermal_temp_in_range(float temp)
{
    if (temp != temp)
    {
        return 0U;
    }

    if (temp < REDPIC1_THERMAL_VALID_TEMP_MIN_C)
    {
        return 0U;
    }
    if (temp > REDPIC1_THERMAL_VALID_TEMP_MAX_C)
    {
        return 0U;
    }

    return 1U;
}

/* 校验整帧原始温度数据是否都在有效范围内。 */
static uint8_t redpic1_thermal_frame_data_is_valid(const float *frame_data)
{
    uint16_t i = 0U;

    if (frame_data == 0)
    {
        return 0U;
    }

    for (i = 0U; i < REDPIC1_THERMAL_PIXEL_COUNT; ++i)
    {
        if (redpic1_thermal_temp_in_range(frame_data[i]) == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

/* 校验一帧统计值是否满足显示条件。 */
static uint8_t redpic1_thermal_frame_is_valid(float min_temp,
                                              float max_temp,
                                              float center_temp)
{
    if (redpic1_thermal_temp_in_range(min_temp) == 0U ||
        redpic1_thermal_temp_in_range(max_temp) == 0U ||
        redpic1_thermal_temp_in_range(center_temp) == 0U)
    {
        return 0U;
    }

    if (max_temp < min_temp)
    {
        return 0U;
    }

    if ((max_temp - min_temp) < REDPIC1_THERMAL_VALID_MIN_SPAN_C)
    {
        return 0U;
    }

    return 1U;
}

/* 判断灰度帧是否具备最基本的对比度。 */
static uint8_t redpic1_thermal_gray_frame_has_contrast(const uint8_t *gray_frame)
{
    uint8_t gray_min = 255U;
    uint8_t gray_max = 0U;
    uint16_t i = 0U;

    if (gray_frame == 0)
    {
        return 0U;
    }

    for (i = 0U; i < REDPIC1_THERMAL_PIXEL_COUNT; ++i)
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

/* 记录采集失败后的退避状态。
 * [6R_3 优化版]：引入分级退避策略 (Tiered Backoff)。
 * transport_related 为 1 时累计总线错误次数，并根据失败等级动态调整惩罚时间。 */
static void redpic1_thermal_note_backoff(uint8_t transport_related)
{
    uint32_t now_ms = power_manager_get_tick_ms();

    app_perf_baseline_record_thermal_capture_failure();
    app_perf_baseline_record_thermal_backoff();

    if (transport_related != 0U)
    {
        s_consecutive_transport_failures++;

#if (REDPIC1_THERMAL_STAGE6R_3_ACTIVE != 0U)
        if (s_consecutive_transport_failures == 1U)
        {
            s_backoff_until_ms = now_ms + 2U;
        }
        else if (s_consecutive_transport_failures == 2U)
        {
            s_backoff_until_ms = now_ms + 5U;
        }
        else
        {
            s_restore_bus_pending = 1U;
            s_backoff_until_ms = now_ms + REDPIC1_THERMAL_BACKOFF_MS;
        }
#else
        s_backoff_until_ms = now_ms + REDPIC1_THERMAL_BACKOFF_MS;
        if (s_consecutive_transport_failures >= REDPIC1_THERMAL_RESTORE_THRESHOLD)
        {
            s_restore_bus_pending = 1U;
        }
#endif
    }
    else
    {
        /* Non-transport failures keep a short retry and reset the transport streak. */
        s_backoff_until_ms = now_ms + 2U;
        s_consecutive_transport_failures = 0U;
    }
}

//static void redpic1_thermal_note_backoff(uint8_t transport_related)
//{
//    app_perf_baseline_record_thermal_capture_failure();
//    app_perf_baseline_record_thermal_backoff();
//    s_backoff_until_ms = power_manager_get_tick_ms() + REDPIC1_THERMAL_BACKOFF_MS;

//    if (transport_related != 0U)
//    {
//        s_consecutive_transport_failures++;
//        if (s_consecutive_transport_failures >= REDPIC1_THERMAL_RESTORE_THRESHOLD)
//        {
//            s_restore_bus_pending = 1U;
//        }
//    }
//    else
//    {
//        s_consecutive_transport_failures = 0U;
//    }
//}

/* 立即恢复 MLX90640 I2C 通路并回写当前刷新率。
 * 该操作用于 stop 唤醒或连续传输失败后的总线恢复。 */
static void redpic1_thermal_restore_bus_now(void)
{
    MLX90640_I2CInit();
    redpic1_thermal_apply_refresh_rate_internal(s_refreshRate, 1U);
    s_restore_bus_pending = 0U;
    s_consecutive_transport_failures = 0U;
    redpic1_thermal_stage6l3_invalidate_history();
}

/* 生成与 frame slot 绑定的 present token。
 * token = frame_seq + slot_index，可用于在异步回调中校验帧身份。 */
static uintptr_t redpic1_thermal_make_slot_token(uint8_t slot_index, uint32_t frame_seq)
{
    return ((((uintptr_t)frame_seq) << REDPIC1_THERMAL_TOKEN_SHIFT) |
            (uintptr_t)slot_index);
}

static uint8_t redpic1_thermal_token_slot_index(uintptr_t token)
{
    return (uint8_t)(token & REDPIC1_THERMAL_TOKEN_SLOT_MASK);
}

static uint32_t redpic1_thermal_token_frame_seq(uintptr_t token)
{
    return (uint32_t)(token >> REDPIC1_THERMAL_TOKEN_SHIFT);
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

static uint8_t redpic1_thermal_ready_frame_expired_locked(uint8_t slot_index, uint32_t now_ms)
{
    uint32_t threshold_ms = 0U;
    uint32_t capture_tick_ms = 0U;

    if (slot_index >= REDPIC1_THERMAL_SLOT_COUNT ||
        s_frame_slots[slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_READY ||
        s_frame_slots[slot_index].valid == 0U)
    {
        return 0U;
    }

    capture_tick_ms = s_frame_slots[slot_index].capture_tick_ms;
    threshold_ms = redpic1_thermal_get_expired_frame_threshold_ms();
    if (capture_tick_ms == 0U || threshold_ms == 0U)
    {
        return 0U;
    }

    return ((uint32_t)(now_ms - capture_tick_ms) > threshold_ms) ? 1U : 0U;
}

static uint8_t redpic1_thermal_cancel_ready_slot_by_token(uintptr_t token)
{
    uint8_t slot_index = redpic1_thermal_token_slot_index(token);
    uint32_t frame_seq = redpic1_thermal_token_frame_seq(token);
    uint8_t cancelled = 0U;

    redpic1_thermal_enter_critical();
    if (slot_index < REDPIC1_THERMAL_SLOT_COUNT &&
        s_ready_slot_index == slot_index &&
        s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY &&
        s_frame_slots[slot_index].frame_seq == frame_seq)
    {
        redpic1_thermal_free_slot_locked(slot_index);
        cancelled = 1U;
    }
    redpic1_thermal_exit_critical();

    if (cancelled != 0U)
    {
        app_perf_baseline_record_thermal_display_cancel();
        app_perf_baseline_record_thermal_3d_done_cancel();
    }

    return cancelled;
}

/* 重置同步等待上下文。
 * 当前固定走异步 present，这里仍保留同步等待上下文，便于兼容运行时错误回收。 */
/* 清空“最近一次已提交 token”记录。 */
static void redpic1_thermal_clear_submitted_token_locked(void)
{
    s_last_submitted_token = 0U;
    s_last_submitted_valid = 0U;
}

/* 在线程安全上下文中清空已提交 token。 */
static void redpic1_thermal_clear_submitted_token(void)
{
    redpic1_thermal_enter_critical();
    redpic1_thermal_clear_submitted_token_locked();
    redpic1_thermal_exit_critical();
}

/* 为同步等待 present done 做准备。 */
/* 通知同步等待者 present 已结束。 */
/* 释放指定槽位。
 * 该函数只能在临界区内调用，用于统一维护 front/ready/inflight 三个所有权索引。 */
static void redpic1_thermal_free_slot_locked(uint8_t slot_index)
{
    if (slot_index >= REDPIC1_THERMAL_SLOT_COUNT)
    {
        return;
    }

    s_frame_slots[slot_index].valid = 0U;
    s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FREE;

    if (s_front_slot_index == slot_index)
    {
        s_front_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
    }
    if (s_ready_slot_index == slot_index)
    {
        s_ready_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
    }
    if (s_inflight_slot_index == slot_index)
    {
        s_inflight_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
    }
    if (s_last_submitted_valid != 0U &&
        redpic1_thermal_token_slot_index(s_last_submitted_token) == slot_index)
    {
        redpic1_thermal_clear_submitted_token_locked();
    }
}

/* 申请新的写入槽位。
 * 优先使用 FREE 槽位；若没有空槽位，则覆盖最新 READY 槽位，但绝不抢占 INFLIGHT/FRONT。 */
static redpic1_thermal_frame_slot_t *redpic1_thermal_get_back_slot(void)
{
    uint8_t slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;

    redpic1_thermal_enter_critical();
    for (slot_index = 0U; slot_index < REDPIC1_THERMAL_SLOT_COUNT; ++slot_index)
    {
        if (s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_FREE)
        {
            break;
        }
    }

    if (slot_index >= REDPIC1_THERMAL_SLOT_COUNT &&
        s_ready_slot_index < REDPIC1_THERMAL_SLOT_COUNT)
    {
        slot_index = s_ready_slot_index;
        redpic1_thermal_free_slot_locked(slot_index);
        app_perf_baseline_record_thermal_ready_replace();
    }

    if (slot_index < REDPIC1_THERMAL_SLOT_COUNT)
    {
        s_frame_slots[slot_index].valid = 0U;
        s_frame_slots[slot_index].ready_tick_ms = 0U;
        s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_WRITING;
    }
    redpic1_thermal_exit_critical();

    if (slot_index >= REDPIC1_THERMAL_SLOT_COUNT)
    {
        return 0;
    }

    return &s_frame_slots[slot_index];
}

/* 释放尚未发布成功的写入槽位。 */
static void redpic1_thermal_release_back_slot(redpic1_thermal_frame_slot_t *slot)
{
    uint8_t slot_index = 0U;

    if (slot == 0)
    {
        return;
    }

    slot_index = (uint8_t)(slot - &s_frame_slots[0]);
    if (slot_index >= REDPIC1_THERMAL_SLOT_COUNT)
    {
        return;
    }

    redpic1_thermal_enter_critical();
    if (s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_WRITING)
    {
        redpic1_thermal_free_slot_locked(slot_index);
    }
    redpic1_thermal_exit_critical();
}

/* 获取当前稳定 front 槽位。
 * front 槽位表示已经完成一次异步送显，可用于 force refresh 的最近稳定帧。 */
static redpic1_thermal_frame_slot_t *redpic1_thermal_get_front_slot(void)
{
    uint8_t front_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
    redpic1_thermal_frame_slot_t *slot = 0;

    redpic1_thermal_enter_critical();
    front_index = s_front_slot_index;
    if (front_index < REDPIC1_THERMAL_SLOT_COUNT &&
        s_frame_slots[front_index].valid != 0U &&
        s_frame_slots[front_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_FRONT)
    {
        slot = &s_frame_slots[front_index];
    }
    redpic1_thermal_exit_critical();

    return slot;
}

/* 将写入完成的槽位发布为 READY。
 * 当前设计最多只保留一帧 READY，新 READY 会替换旧 READY，等待 display runtime 认领。 */
static void redpic1_thermal_publish_back_slot(redpic1_thermal_frame_slot_t *slot)
{
    uint8_t publish_index = 0U;
    uint8_t old_ready = REDPIC1_THERMAL_SLOT_INDEX_NONE;
    uint8_t note_replace = 0U;
    uint32_t ready_tick_ms = power_manager_get_tick_ms();

    if (slot == 0)
    {
        return;
    }

    publish_index = (uint8_t)(slot - &s_frame_slots[0]);
    if (publish_index >= REDPIC1_THERMAL_SLOT_COUNT)
    {
        return;
    }

    redpic1_thermal_enter_critical();
    if (s_frame_slots[publish_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_WRITING)
    {
        old_ready = s_ready_slot_index;
        if (old_ready < REDPIC1_THERMAL_SLOT_COUNT &&
            old_ready != publish_index &&
            s_frame_slots[old_ready].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY)
        {
            redpic1_thermal_free_slot_locked(old_ready);
            note_replace = 1U;
        }
        s_frame_slots[publish_index].valid = 1U;
        s_frame_slots[publish_index].ready_tick_ms = ready_tick_ms;
        s_frame_slots[publish_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_READY;
        s_ready_slot_index = publish_index;
    }
    redpic1_thermal_exit_critical();

    if (note_replace != 0U)
    {
        app_perf_baseline_record_thermal_ready_replace();
    }
}

static void redpic1_thermal_present_front_slot(void)
{
    redpic1_thermal_frame_slot_t *slot = 0;

    if (s_runEnabled == 0U || s_display_paused != 0U || s_overlayHold != 0U || s_frameReady == 0U)
    {
        return;
    }

    slot = redpic1_thermal_get_front_slot();
    if (slot == 0)
    {
        return;
    }

    (void)redpic1_thermal_present_gray_frame(slot->gray_frame);
}

static void redpic1_thermal_try_submit_latest_ready_after_done(void);
static void redpic1_thermal_try_submit_latest_ready_after_resume(void);

/* 供 display runtime 在异步送显开始前认领 READY 槽位。
 * thermal task 负责发布 READY，display runtime 通过 token 精确认领对应帧。 */
static uint8_t redpic1_thermal_try_claim_present(uintptr_t token, uint8_t **gray_frame)
{
    uint8_t slot_index = redpic1_thermal_token_slot_index(token);
    uint32_t frame_seq = redpic1_thermal_token_frame_seq(token);
    uint32_t now_ms = power_manager_get_tick_ms();
    uint8_t claimed = 0U;
    uint8_t expired = 0U;

    if (gray_frame == 0)
    {
        return 0U;
    }

    *gray_frame = 0;

    redpic1_thermal_enter_critical();
    if (s_runEnabled != 0U &&
        slot_index < REDPIC1_THERMAL_SLOT_COUNT &&
        s_ready_slot_index == slot_index &&
        s_inflight_slot_index == REDPIC1_THERMAL_SLOT_INDEX_NONE &&
        s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY &&
        s_frame_slots[slot_index].frame_seq == frame_seq)
    {
#if (REDPIC1_THERMAL_DROP_EXPIRED_FRAME_ACTIVE != 0U)
        if (redpic1_thermal_ready_frame_expired_locked(slot_index, now_ms) != 0U)
        {
            expired = 1U;
        }
        else
#endif
        {
            s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT;
            s_ready_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
            s_inflight_slot_index = slot_index;
            *gray_frame = s_frame_slots[slot_index].gray_frame;
            claimed = 1U;
        }
    }
    redpic1_thermal_exit_critical();

    if (expired != 0U)
    {
        (void)redpic1_thermal_cancel_ready_slot_by_token(token);
        return 0U;
    }

    if (claimed != 0U)
    {
        app_perf_baseline_record_thermal_3d_claim();
    }

    return claimed;
}

/* 处理 display runtime 的异步完成回调。
 * 该回调负责 READY -> INFLIGHT -> FRONT/FREE 的状态迁移，是热成像任务与显示回调的并发边界。 */
static void redpic1_thermal_handle_display_done(uintptr_t token,
                                                app_display_thermal_done_status_t status)
{
    uint8_t slot_index = redpic1_thermal_token_slot_index(token);
    uint32_t frame_seq = redpic1_thermal_token_frame_seq(token);
    uint32_t display_done_tick_ms = power_manager_get_tick_ms();
    uint32_t display_age_ms = 0U;
    uint8_t note_cancel = 0U;
    uint8_t note_display_age = 0U;
    uint8_t note_done_ok = 0U;
    uint8_t note_done_error = 0U;
    uint8_t note_done_cancel = 0U;

    redpic1_thermal_enter_critical();
    if (slot_index < REDPIC1_THERMAL_SLOT_COUNT &&
        s_frame_slots[slot_index].frame_seq == frame_seq)
    {
        switch (status)
        {
        case APP_DISPLAY_THERMAL_DONE_OK:
            if (s_inflight_slot_index == slot_index &&
                s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT)
            {
                s_inflight_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
                if (s_frame_slots[slot_index].capture_tick_ms != 0U)
                {
                    display_age_ms = (uint32_t)(display_done_tick_ms -
                                                s_frame_slots[slot_index].capture_tick_ms);
                    note_display_age = 1U;
                }
                if (s_runEnabled != 0U)
                {
                    uint8_t old_front = s_front_slot_index;

                    if (old_front < REDPIC1_THERMAL_SLOT_COUNT && old_front != slot_index)
                    {
                        redpic1_thermal_free_slot_locked(old_front);
                    }

                    s_frame_slots[slot_index].valid = 1U;
                    s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FRONT;
                    s_front_slot_index = slot_index;
                    s_frameReady = 1U;
                }
                else
                {
                    redpic1_thermal_free_slot_locked(slot_index);
                }
                note_done_ok = 1U;
            }
            break;

        case APP_DISPLAY_THERMAL_DONE_CANCELLED:
            if ((s_ready_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY) ||
                (s_inflight_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT))
            {
                redpic1_thermal_free_slot_locked(slot_index);
                note_cancel = 1U;
                note_done_cancel = 1U;
            }
            break;

        case APP_DISPLAY_THERMAL_DONE_ERROR:
        default:
            if ((s_inflight_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT) ||
                (s_ready_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY))
            {
                redpic1_thermal_free_slot_locked(slot_index);
                note_done_error = 1U;
            }
            break;
        }
    }

    if (s_front_slot_index >= REDPIC1_THERMAL_SLOT_COUNT ||
        s_frame_slots[s_front_slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_FRONT ||
        s_frame_slots[s_front_slot_index].valid == 0U)
    {
        s_frameReady = 0U;
    }
    redpic1_thermal_exit_critical();

    if (note_cancel != 0U)
    {
        app_perf_baseline_record_thermal_display_cancel();
    }

    if (note_display_age != 0U)
    {
        app_perf_baseline_record_thermal_display_age_ms(display_age_ms);
    }

    if (note_done_ok != 0U)
    {
        app_perf_baseline_record_thermal_3d_done_ok();
    }
    if (note_done_error != 0U)
    {
        app_perf_baseline_record_thermal_3d_done_error();
    }
    if (note_done_cancel != 0U)
    {
        app_perf_baseline_record_thermal_3d_done_cancel();
    }

    redpic1_thermal_enter_critical();
    if (s_last_submitted_valid != 0U && s_last_submitted_token == token)
    {
        redpic1_thermal_clear_submitted_token_locked();
    }
    redpic1_thermal_exit_critical();

    if (note_done_ok != 0U)
    {
        redpic1_thermal_try_submit_latest_ready_after_done();
    }
}

/* 提交当前最新 READY 槽位。
 * 如果 display runtime 尚未消费旧 token，则不重复提交，避免同一帧被重复排队。 */
static uint8_t redpic1_thermal_submit_latest_ready_slot(void)
{
    redpic1_thermal_frame_slot_t *slot = 0;
    uintptr_t token = 0U;
    uint8_t submit_needed = 0U;
    uint8_t ok = 0U;

    redpic1_thermal_enter_critical();
    if (s_ready_slot_index < REDPIC1_THERMAL_SLOT_COUNT &&
        s_frame_slots[s_ready_slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY)
    {
        slot = &s_frame_slots[s_ready_slot_index];
        token = redpic1_thermal_make_slot_token(s_ready_slot_index, slot->frame_seq);
        if (s_last_submitted_valid == 0U || s_last_submitted_token != token)
        {
            submit_needed = 1U;
        }
    }
    redpic1_thermal_exit_critical();

    if (slot == 0)
    {
        return 0U;
    }

    if (submit_needed == 0U)
    {
        return 1U;
    }

    ok = app_display_runtime_request_thermal_present_async(slot->gray_frame, token);
    if (ok != 0U)
    {
        redpic1_thermal_enter_critical();
        s_last_submitted_token = token;
        s_last_submitted_valid = 1U;
        redpic1_thermal_exit_critical();
    }

    return ok;
}

static uint8_t redpic1_thermal_can_gap_close_submit_locked(void)
{
    if (s_runEnabled == 0U ||
        s_display_paused != 0U ||
        s_overlayHold != 0U ||
        s_inflight_slot_index != REDPIC1_THERMAL_SLOT_INDEX_NONE)
    {
        return 0U;
    }

    if (s_ready_slot_index >= REDPIC1_THERMAL_SLOT_COUNT ||
        s_frame_slots[s_ready_slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_READY)
    {
        return 0U;
    }

    return 1U;
}

static void redpic1_thermal_try_submit_latest_ready_after_gap_close(void)
{
    uint8_t can_submit = 0U;

    redpic1_thermal_enter_critical();
    can_submit = redpic1_thermal_can_gap_close_submit_locked();
    redpic1_thermal_exit_critical();

    if (can_submit != 0U)
    {
        (void)redpic1_thermal_submit_latest_ready_slot();
    }
}

static void redpic1_thermal_try_submit_latest_ready_after_done(void)
{
    redpic1_thermal_try_submit_latest_ready_after_gap_close();
}

static void redpic1_thermal_try_submit_latest_ready_after_resume(void)
{
    redpic1_thermal_try_submit_latest_ready_after_gap_close();
}

/* 取消尚未完成的异步送显请求，并清空本地“最近一次已提交 token”记录。 */
static void redpic1_thermal_cancel_pending_present_and_clear_submit(void)
{
    app_display_runtime_cancel_thermal_present_async();
    redpic1_thermal_clear_submitted_token();
}

/* 丢弃除 INFLIGHT 以外的所有槽位状态。
 * suspend/resume 时保留可能仍在显示链路中的那一帧，其余缓存全部重建。 */
static void redpic1_thermal_drop_non_inflight_slots(void)
{
    uint8_t slot_index = 0U;

    redpic1_thermal_enter_critical();
    s_front_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
    s_ready_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;

    for (slot_index = 0U; slot_index < REDPIC1_THERMAL_SLOT_COUNT; ++slot_index)
    {
        if (slot_index == s_inflight_slot_index &&
            s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT)
        {
            continue;
        }

        s_frame_slots[slot_index].valid = 0U;
        s_frame_slots[slot_index].frame_seq = 0U;
        s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FREE;
    }

    if (s_inflight_slot_index >= REDPIC1_THERMAL_SLOT_COUNT ||
        s_frame_slots[s_inflight_slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT)
    {
        s_inflight_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
        s_frame_sequence = 0U;
    }
    redpic1_thermal_clear_submitted_token_locked();
    redpic1_thermal_exit_critical();

    s_backoff_until_ms = 0U;
    s_restore_bus_pending = 0U;
    s_consecutive_transport_failures = 0U;
    s_frameReady = 0U;
    redpic1_thermal_reset_processing_history();
}

/* 完整重置所有槽位与运行时缓存。
 * 初始化阶段调用该函数，确保 front/ready/inflight 和显示窗口/滤波历史全部回到初始状态。 */
static void redpic1_thermal_reset_slots(void)
{
    uint8_t slot_index = 0U;

    redpic1_thermal_enter_critical();
    s_front_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
    s_ready_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
    s_inflight_slot_index = REDPIC1_THERMAL_SLOT_INDEX_NONE;
    redpic1_thermal_clear_submitted_token_locked();
    for (slot_index = 0U; slot_index < REDPIC1_THERMAL_SLOT_COUNT; ++slot_index)
    {
        s_frame_slots[slot_index].valid = 0U;
        s_frame_slots[slot_index].frame_seq = 0U;
        s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FREE;
    }
    redpic1_thermal_exit_critical();

    s_frame_sequence = 0U;
    s_backoff_until_ms = 0U;
    s_restore_bus_pending = 0U;
    s_consecutive_transport_failures = 0U;
    s_frameReady = 0U;
    redpic1_thermal_reset_processing_history();
}

void redpic1_thermal_init(void)
{
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

    redpic1_thermal_reset_processing_history();
    redpic1_thermal_reset_slots();
    redpic1_thermal_build_diag_pattern();
}

void redpic1_thermal_bind_display_runtime(void)
{
    app_display_runtime_set_thermal_present_claim_callback(redpic1_thermal_try_claim_present);
    app_display_runtime_set_thermal_present_done_callback(redpic1_thermal_handle_display_done);
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
    uint32_t get_temp_start_cycle = 0U;
    uint32_t gray_start_cycle = 0U;
    float ta = 0.0f;
    float frame_min_temp = 0.0f;
    float frame_max_temp = 0.0f;
    float frame_center_temp = 0.0f;
#if (REDPIC1_THERMAL_STAGE6R_1_ACTIVE == 0U)
    uint16_t state = 0U;
#endif

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
        uint32_t now_ms = power_manager_get_tick_ms();

#if REDPIC1_THERMAL_DIAG_MODE == REDPIC1_THERMAL_DIAG_MODE_TEST_PATTERN
        redpic1_thermal_present_diag_pattern();
        app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
        return;
#endif

        if (s_backoff_until_ms != 0U &&
            redpic1_thermal_deadline_reached(now_ms, s_backoff_until_ms) == 0U)
        {
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        s_backoff_until_ms = 0U;
        if (s_restore_bus_pending != 0U)
        {
            redpic1_thermal_restore_bus_now();
        }

#if (REDPIC1_THERMAL_STAGE6R_1_ACTIVE == 0U)
        if (MLX90640_I2CRead(MLX90640_ADDR, MLX90640_REG_STATUS, 1, &state) != 0)
        {
            app_perf_baseline_record_i2c_failure();
            redpic1_thermal_note_backoff(1U);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        if ((state & MLX90640_STATUS_DATA_READY_MASK) == 0U)
        {
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }
#endif

        back_slot = redpic1_thermal_get_back_slot();
        if (back_slot == 0)
        {
            redpic1_thermal_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        get_temp_start_cycle = app_perf_baseline_cycle_now();
        
        /* ============== 6R_2 核心优化点：分离软超时与硬故障 ============== */
#if REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ACTIVE
        int temp_status = get_temp_ex(back_slot->temp_frame, &ta, &captured_subpage);
#else
        int temp_status = get_temp(back_slot->temp_frame, &ta);
#endif
        
        if (temp_status < 0)
        {
            app_perf_baseline_record_get_temp_us(app_perf_baseline_elapsed_us(get_temp_start_cycle));
            
#if (REDPIC1_THERMAL_STAGE6R_2_ACTIVE != 0U)
            if (temp_status == -9) 
            {
                /* 软超时 (MLX90640_READY_WAIT_TIMEOUT_ERROR): 
                 * 仅表示新帧物理上还没准备好，释放槽位，静默退出，不作惩罚 */
                redpic1_thermal_release_back_slot(back_slot);
                redpic1_thermal_note_backoff(0U);
            }
            else 
#endif
            {
                /* 硬故障: 真正的 I2C 总线错乱/NACK/校验失败，走原始的报错与退避逻辑 */
                app_perf_baseline_record_i2c_failure();
                redpic1_thermal_release_back_slot(back_slot);
                redpic1_thermal_stage6l3_invalidate_history();
                redpic1_thermal_note_backoff(1U);
            }
            
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }
        /* ================================================================= */

        app_perf_baseline_record_get_temp_us(app_perf_baseline_elapsed_us(get_temp_start_cycle));

        capture_tick_ms = power_manager_get_tick_ms();
#if REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ACTIVE
        if (redpic1_thermal_stagev4_c1_try_compose(back_slot->temp_frame,
                                                   captured_subpage,
                                                   capture_tick_ms,
                                                   &capture_tick_ms) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }
#endif

        if (redpic1_thermal_frame_data_is_valid(back_slot->temp_frame) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            redpic1_thermal_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        if (redpic1_thermal_stage6l3_capture_gap_exceeded(capture_tick_ms) != 0U)
        {
            redpic1_thermal_stage6l3_invalidate_history();
        }

        gray_source_frame = redpic1_thermal_get_gray_source_frame(back_slot->temp_frame,
                                                                  &high_motion_frame);

        gray_start_cycle = app_perf_baseline_cycle_now();
        redpic1_thermal_prepare_gray_frame(back_slot->temp_frame,
                                           gray_source_frame,
                                           high_motion_frame,
                                           back_slot->gray_frame,
                                           &frame_min_temp,
                                           &frame_max_temp);
        app_perf_baseline_record_gray_us(app_perf_baseline_elapsed_us(gray_start_cycle));
        frame_center_temp = redpic1_thermal_center_temp(back_slot->temp_frame);

        if (redpic1_thermal_frame_is_valid(frame_min_temp,
                                           frame_max_temp,
                                           frame_center_temp) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            redpic1_thermal_note_backoff(0U);
            app_perf_baseline_record_thermal_step_us(app_perf_baseline_elapsed_us(step_start_cycle));
            return;
        }

        if (redpic1_thermal_gray_frame_has_contrast(back_slot->gray_frame) == 0U)
        {
            redpic1_thermal_release_back_slot(back_slot);
            redpic1_thermal_note_backoff(0U);
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
        back_slot->frame_seq = ++s_frame_sequence;

        app_perf_baseline_record_thermal_capture_success(capture_tick_ms,
                                                         frame_min_temp,
                                                         frame_max_temp,
                                                         frame_center_temp);

        redpic1_thermal_publish_back_slot(back_slot);
        if (s_display_paused == 0U && s_overlayHold == 0U)
        {
            (void)redpic1_thermal_submit_latest_ready_slot();
        }
        s_consecutive_transport_failures = 0U;
        s_last_capture_tick_ms = capture_tick_ms;
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
#if (REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ACTIVE != 0U)
            if (was_display_paused == 0U &&
                redpic1_thermal_pause_send_esp_enabled() != 0U)
            {
                (void)redpic1_thermal_submit_snapshot_to_esp();
            }
#endif
        }
        redpic1_thermal_mark_bottom_bar_dirty(REDPIC1_THERMAL_OVERLAY_DIRTY_REASON_PAUSE);
        redpic1_thermal_stage6l3_invalidate_history();
        if (was_display_paused != 0U && s_display_paused == 0U)
        {
            redpic1_thermal_try_submit_latest_ready_after_resume();
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

    power_manager_acquire_lock(POWER_LOCK_THERMAL);
    power_manager_notify_activity();

    redpic1_thermal_try_submit_latest_ready_after_resume();
}

/* STOP 唤醒后恢复 MLX90640 总线。
 * 若调度器已运行，则只设置延后恢复标志，由 thermal task 在安全上下文中完成恢复。 */
void redpic1_thermal_restore_bus_after_stop(void)
{
    if (redpic1_thermal_scheduler_running() != 0U)
    {
        s_restore_bus_pending = 1U;
        s_backoff_until_ms = 0U;
        return;
    }

    MLX90640_I2CInit();
    redpic1_thermal_apply_refresh_rate_internal(s_refreshRate, 1U);
    redpic1_thermal_stage6l3_invalidate_history();
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
