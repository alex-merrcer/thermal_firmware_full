/**
 * @file    lcd_dma.c
 * @brief   LCD DMA 热成像显示驱动模块
 * @note    本模块负责将 MLX90640 输出的 24x32 热成像灰度帧，
 *          通过双线性插值放大为 240x320 像素，并经由 SPI1 + DMA2 Stream3
 *          逐条带（stripe）发送到 LCD 屏幕。
 *
 * @par 核心流程
 *      1. 水平插值：将每行 24 列扩展为 240 列（10 倍）
 *      2. 垂直边缘外推：生成顶部 5 行、底部 4 行的外推数据
 *      3. 伪彩色映射：灰度值经 LUT 转换为 RGB565 颜色
 *      4. 十字准星叠加：在画面中心绘制测量十字准星
 *      5. DMA 双缓冲发送：交替填充/发送，实现渲染与传输并行
 *
 * @par 性能优化要点
 *      - 使用 __USAT 硬件饱和指令替代分支式 clamp
 *      - 用乘法+移位替代除法（(X * 205) >> 11 等效 X / 10）
 *      - 内联函数消除热路径的函数调用开销
 *      - 双行缓冲实现渲染与 DMA 传输流水线化
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "lcd_dma.h"
#include "lcd_init.h"
#include "lcd.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>
#include "stdlib.h"
#include "math.h"
#include "sys.h"

#include "app_display_runtime.h"
#include "app_perf_baseline.h"
#include "redpic1_thermal.h"

/* =========================================================================
 *  2. 宏定义 — 渲染尺寸参数
 * ======================================================================= */

/** @defgroup LCD_DMA_DIM  渲染尺寸与源图像参数
 *  @{ */
#define THERMAL_RENDER_WIDTH    240         /**< 插值后渲染宽度（像素）        */
#define THERMAL_RENDER_HEIGHT   320         /**< 插值后渲染高度（像素）        */
#define THERMAL_OUTPUT_ROWS     (LCD_H - 20U)  /**< 实际输出到 LCD 的行数     */
#define THERMAL_SRC_WIDTH       24U         /**< 热成像源图像宽度              */
#define THERMAL_SRC_HEIGHT      32U         /**< 热成像源图像高度              */
/** @} */

/** @defgroup LCD_DMA_INTERP  插值与边缘外推参数
 *  @{ */
#define INTERP_STRIDE           10          /**< 水平插值倍率（24 -> 240）     */
#define TOP_EDGE_ROWS           5           /**< 顶部边缘外推行数              */
#define BOTTOM_EDGE_ROWS        4           /**< 底部边缘外推行数              */
#define BOTTOM_EDGE_START       (THERMAL_RENDER_HEIGHT - BOTTOM_EDGE_ROWS) /**< 底部边缘起始行 */
/** @} */

/** @defgroup LCD_DMA_BUF  缓冲区参数
 *  @{ */
#define LINE_BUF_SIZE           (LCD_W * 2)                     /**< 单行字节数（RGB565） */
#define LCD_DMA_THERMAL_STRIPE_ROWS  1U                         /**< 每次 DMA 发送的行数  */
#define LCD_DMA_THERMAL_AREA_WIDTH   LCD_W                      /**< 热成像区域宽度       */
#define LCD_DMA_THERMAL_AREA_HEIGHT  THERMAL_OUTPUT_ROWS        /**< 热成像区域高度       */
#define LCD_DMA_THERMAL_AREA_X0      0U                         /**< 热成像区域起始 X     */
#define LCD_DMA_THERMAL_AREA_Y0      0U                         /**< 热成像区域起始 Y     */
#define LCD_DMA_THERMAL_AREA_LINE_BYTES (LCD_DMA_THERMAL_AREA_WIDTH * 2U) /**< 区域单行字节数 */
#define LCD_DMA_THERMAL_STRIPE_BUF_SIZE (LINE_BUF_SIZE * LCD_DMA_THERMAL_STRIPE_ROWS) /**< 条带缓冲区大小 */
/** @} */

/* =========================================================================
 *  3. 宏定义 — 十字准星参数
 * ======================================================================= */

/** @defgroup LCD_DMA_CROSS  十字准星绘制参数
 *  @{ */
#define LCD_DMA_THERMAL_CROSS_HALF_SIZE 6U      /**< 准星半臂长度（像素）      */
#define LCD_DMA_THERMAL_CROSS_GAP_SIZE  2U      /**< 准星中心间隙（像素）      */
/** @} */

/* =========================================================================
 *  4. 宏定义 — DMA / SPI 超时参数
 * ======================================================================= */

/** @defgroup LCD_DMA_TIMEOUT  超时与等待参数
 *  @{ */
#define DMA_TRANSFER_WAIT_LOOPS        5000000UL   /**< DMA 传输完成忙等待上限   */
#define DMA_TRANSFER_WAIT_TIMEOUT_MS   20UL        /**< DMA 传输等待超时（ms）   */
#define DMA_STREAM_DISABLE_WAIT_LOOPS  100000UL    /**< DMA 流关闭等待上限       */
#define LCD_SPI_IDLE_WAIT_LOOPS        100000UL    /**< SPI 空闲等待上限         */
/** @} */

/* =========================================================================
 *  5. 宏定义 — 伪彩色 LUT 与色调映射参数
 * ======================================================================= */

/** @defgroup LCD_DMA_PALETTE  伪彩色调色板参数
 *  @{ */
#define THERMAL_ENABLE_OLD_PALETTE_MAP      1       /**< 启用旧版调色板映射路径   */
#define THERMAL_ENABLE_TONE_LUT             1       /**< 启用色调 LUT 压缩高光区  */
#define THERMAL_TONE_LUT_START_GRAY         158U    /**< 色调映射起始灰度值       */
#define THERMAL_TONE_LUT_INPUT_SPAN         100U    /**< 色调映射输入跨度         */
#define THERMAL_TONE_LUT_OUTPUT_SPAN        82U     /**< 色调映射输出跨度         */
#define THERMAL_TONE_LUT_MAX_GRAY           238U    /**< 色调映射最大输出灰度     */
#define THERMAL_VISIBLE_PALETTE_WHITE_HOT   0xFEU   /**< 白热模式伪调色板 ID      */
#define THERMAL_VISIBLE_PALETTE_BLACK_HOT   0xFFU   /**< 黑热模式伪调色板 ID      */
#define THERMAL_VISIBLE_PALETTE_COUNT       5U      /**< 可见调色板模式总数       */
/** @} */

/* =========================================================================
 *  6. 宏定义 — 工具宏
 * ======================================================================= */

/** @brief 计算数组元素个数 */
#define LCD_DMA_ARRAY_SIZE(a)   ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/** @brief 将 RGB888 分量合成为 RGB565 格式 */
#define LCD_DMA_RGB565(r, g, b) ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | \
                                            (((uint16_t)(g) & 0xFCU) << 3) | \
                                            (((uint16_t)(b) & 0xF8U) >> 3)))

/** @brief 十字准星线条颜色（淡青色） */
#define LCD_DMA_THERMAL_CROSS_COLOR_PRODUCT     LCD_DMA_RGB565(210U, 230U, 230U)

/** @brief 十字准星中心点颜色（白色） */
#define LCD_DMA_THERMAL_CROSS_CENTER_PRODUCT    LCD_DMA_RGB565(255U, 255U, 255U)

/** @brief 快速整除 10 宏：(x * 205) >> 11 ≈ x / 10，避免硬件除法 */
#define FAST_DIV_10(x)  (((x) * 205U) >> 11U)

/* =========================================================================
 *  7. 宏定义 — 横屏/竖屏方向适配
 * ======================================================================= */

/**
 * @brief 根据 USE_HORIZONTAL 宏选择源图像的宽高映射
 * @note  USE_HORIZONTAL == 2 时为横屏模式，源宽高互换
 */
#if USE_HORIZONTAL == 2
    #define LCD_DMA_ORIENTED_SRC_WIDTH  THERMAL_SRC_HEIGHT
    #define LCD_DMA_ORIENTED_SRC_HEIGHT THERMAL_SRC_WIDTH
#else
    #define LCD_DMA_ORIENTED_SRC_WIDTH  THERMAL_SRC_WIDTH
    #define LCD_DMA_ORIENTED_SRC_HEIGHT THERMAL_SRC_HEIGHT
#endif

/**
 * @brief Stage6_6B / 6C 已启用标志
 * @note  当前工程固定启用，不再依赖 redpic1_thermal 的历史阶段宏。
 */
#define LCD_DMA_STAGE6_6B_ACTIVE    1

/* =========================================================================
 *  8. 数据类型定义
 * ======================================================================= */

/**
 * @brief 伪彩色调色板控制点
 * @note  定义调色板中某一位置的 RGB 颜色分量
 */
typedef struct
{
    uint8_t pos;    /**< 灰度位置（0~255） */
    uint8_t r;      /**< 红色分量          */
    uint8_t g;      /**< 绿色分量          */
    uint8_t b;      /**< 蓝色分量          */
} thermal_palette_stop_t;

/**
 * @brief 伪彩色调色板 ID 枚举
 */
typedef enum
{
    THERMAL_PALETTE_IRON = 0,       /**< 铁红色调色板    */
    THERMAL_PALETTE_RAINBOW,        /**< 彩虹色调色板    */
    THERMAL_PALETTE_WHITE_HOT,      /**< 白热模式        */
    THERMAL_PALETTE_BLACK_HOT,      /**< 黑热模式        */
    THERMAL_PALETTE_ARCTIC,         /**< 北极蓝色调色板  */
    THERMAL_PALETTE_COUNT           /**< 调色板总数      */
} thermal_palette_id_t;

/**
 * @brief DMA 传输工作模式枚举
 */
typedef enum
{
    LCD_DMA_MODE_IDLE    = 0,   /**< 空闲态          */
    LCD_DMA_MODE_THERMAL = 1    /**< 热成像传输态    */
} lcd_dma_mode_t;

#if LCD_DMA_STAGE6_6B_ACTIVE
/**
 * @brief 插值行类型枚举
 * @note  区分边缘外推行与主体插值行，用于快速采样。
 */
typedef enum
{
    LCD_DMA_INTERP_ROW_TOP    = 0,  /**< 顶部边缘外推行    */
    LCD_DMA_INTERP_ROW_BODY   = 1,  /**< 主体插值行        */
    LCD_DMA_INTERP_ROW_BOTTOM = 2   /**< 底部边缘外推行    */
} lcd_dma_interp_row_kind_t;

/**
 * @brief 插值行元数据结构体
 * @note  记录每行的类型、基准行索引和插值比例，
 *        用于渲染阶段快速定位采样源数据。
 */
typedef struct
{
    uint8_t kind;           /**< 行类型（top/body/bottom） */
    uint8_t base_index;     /**< 基准行索引                */
    uint8_t ratio;          /**< 插值比例（0~INTERP_STRIDE-1） */
} lcd_dma_interp_row_meta_t;
#endif

/**
 * @brief 单帧行能统计结构体
 */
typedef struct
{
    uint32_t render_us;             /**< 渲染耗时（微秒）          */
    uint32_t dma_start_us;          /**< DMA 启动耗时（微秒）      */
    uint32_t dma_wait_us;           /**< DMA 等待耗时（微秒）      */
    uint32_t spi_idle_wait_us;      /**< SPI 空闲等待耗时（微秒）  */
    uint32_t overlay_us;            /**< 十字准星叠加耗时（微秒）  */
} lcd_dma_frame_perf_t;

/* =========================================================================
 *  9. 模块级静态 / 全局变量
 * ======================================================================= */

/** @defgroup LCD_DMA_VARS  模块级变量
 *  @{ */

/* ---- 双行缓冲区（必须位于系统 SRAM，DMA 可达） ---- */
__attribute__((section("dma_sram"), aligned(4)))
u8 lineBuffer[2][LCD_DMA_THERMAL_STRIPE_BUF_SIZE];     /**< 双行缓冲区            */

volatile uint8_t activeBuffer       = 0;    /**< 当前活跃缓冲区索引（0/1） */
volatile uint8_t transferComplete   = 1;    /**< DMA 传输完成标志          */

static volatile TaskHandle_t s_dma_wait_task    = 0;    /**< 等待 DMA 完成的任务句柄 */
static volatile uint8_t s_dma_last_result       = 1U;   /**< 最近一次 DMA 传输结果   */
static volatile app_perf_lcd_dma_status_t s_dma_last_status = APP_PERF_LCD_DMA_STATUS_NONE; /**< 最近一次 DMA 状态码 */

static volatile lcd_dma_mode_t s_dma_mode = LCD_DMA_MODE_IDLE; /**< 当前 DMA 工作模式 */

/* ---- 插值缓冲区（位于 CCRAM，访问速度快） ---- */
static CCMRAM uint8_t g_interpRows[THERMAL_SRC_HEIGHT][THERMAL_RENDER_WIDTH];   /**< 水平插值结果缓冲区 */
static CCMRAM uint8_t g_topEdgeRows[TOP_EDGE_ROWS][THERMAL_RENDER_WIDTH];       /**< 顶部边缘外推行     */
static CCMRAM uint8_t g_bottomEdgeRows[BOTTOM_EDGE_ROWS][THERMAL_RENDER_WIDTH]; /**< 底部边缘外推行     */

/* ---- 伪彩色查找表 ---- */
CCMRAM uint16_t GCM_Pseudo3[256];  /**< 全局伪彩色 LUT（256 灰度 -> RGB565） */

/* ---- 可见调色板模式映射表 ---- */
static const uint8_t kVisiblePaletteMap[THERMAL_VISIBLE_PALETTE_COUNT] = {
    3U,     /**< 模式 0 -> color_code mode 3 */
    4U,     /**< 模式 1 -> color_code mode 4 */
    1U,     /**< 模式 2 -> color_code mode 1 */
    2U,     /**< 模式 3 -> color_code mode 2 */
    7U      /**< 模式 4 -> color_code mode 7 */
};

/* ---- 色调映射 LUT ---- */
static uint8_t  s_thermal_tone_lut[256];        /**< 色调压缩查找表            */
static uint8_t  s_thermal_tone_lut_ready = 0U;  /**< 色调 LUT 是否已构建       */

#if LCD_DMA_STAGE6_6B_ACTIVE
/* ---- 渲染映射表（预计算，避免逐像素重复计算） ---- */
static CCMRAM lcd_dma_interp_row_meta_t g_interpRowMeta[THERMAL_RENDER_HEIGHT];  /**< 每行插值元数据       */
static CCMRAM uint16_t g_outputRowToInterpRow[LCD_H];  /**< 输出行 -> 插值行映射  */
static CCMRAM uint16_t g_outputColToInterpRow[LCD_W];  /**< 输出列 -> 插值行映射  */
static CCMRAM uint16_t g_outputRowToInterpCol[LCD_H];  /**< 输出行 -> 插值列映射  */
static CCMRAM uint16_t g_outputColToInterpCol[LCD_W];  /**< 输出列 -> 插值列映射  */
static uint8_t s_renderMappingReady = 0U;               /**< 映射表是否已初始化    */
#endif

/* ---- 帧行能统计 ---- */
static lcd_dma_frame_perf_t s_lcd_dma_frame_perf;   /**< 当前帧行能数据 */

/** @} */

/* =========================================================================
 *  10. 内部函数前向声明
 * ======================================================================= */

/* ---- 性能统计 ---- */
static void     lcd_dma_perf_reset_frame    (void);
static void     lcd_dma_perf_add_elapsed    (uint32_t *accum, uint32_t start_cycle);
static void     lcd_dma_perf_commit_frame   (void);

/* ---- 工具函数 ---- */
static uint8_t  clamp_to_u8                (int32_t value);
static uint16_t lcd_dma_active_line_bytes   (void);
static uint16_t lcd_dma_scale_axis          (uint16_t index, uint16_t output_count,
                                              uint16_t interp_count);

/* ---- 像素写入与颜色转换 ---- */
static void     lcd_dma_write_rgb565_pixel  (uint8_t *buf, uint16_t out_x, uint16_t color);
static inline uint16_t lcd_dma_gray_to_output_color(uint8_t pixel);

/* ---- 十字准星叠加 ---- */
static void     lcd_dma_overlay_crosshair_row(uint16_t out_row, uint8_t *buf);

/* ---- DMA 传输控制 ---- */
static uint8_t  lcd_dma_wait_stream_disabled    (void);
static uint8_t  lcd_dma_wait_spi_idle           (void);
static uint8_t  start_dma_line_transfer         (uint8_t *buf, uint16_t row_count);
static uint8_t  lcd_dma_scheduler_running       (void);
static uint8_t  lcd_dma_wait_busy_loop          (void);
static uint8_t  wait_for_dma_transfer_complete  (void);

/* ---- 渲染管线 ---- */
static void     build_horizontal_interp_rows    (const uint8_t *frameData);
static void     build_vertical_edge_rows        (void);
static void     lcd_dma_render_legacy_row       (uint16_t out_row, uint8_t *buf);
static void     render_output_row_to_buffer     (uint16_t outRow, uint8_t *buf);
static uint16_t lcd_dma_get_stripe_row_count    (uint16_t start_row);
static void     render_output_rows_to_buffer    (uint16_t start_row, uint16_t row_count,
                                                  uint8_t *buf);

/* ---- 色调映射与调色板 ---- */
static uint16_t lcd_dma_rgb565_from_rgb888      (uint8_t r, uint8_t g, uint8_t b);
static void     thermal_build_tone_lut          (void);
static uint8_t  thermal_apply_tone_map          (uint8_t gray);
static uint16_t thermal_visible_palette_color   (uint8_t gray, uint16_t visible_mode);

/* =========================================================================
 *  11. 工具函数实现
 * ======================================================================= */

/**
 * @brief  将任意整数裁剪到 0~255 范围
 * @param  value — 输入整数值
 * @return uint8_t — 裁剪后的值
 */
static uint8_t clamp_to_u8(int32_t value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 255)
    {
        return 255;
    }
    return (uint8_t)value;
}

/**
 * @brief  获取当前活跃的单行字节数
 * @return uint16_t — 热成像区域单行字节数（RGB565）
 */
static uint16_t lcd_dma_active_line_bytes(void)
{
    return (uint16_t)LCD_DMA_THERMAL_AREA_LINE_BYTES;
}

/**
 * @brief  向行缓冲区写入一个 RGB565 像素
 * @param  buf   — 行缓冲区指针
 * @param  out_x — 输出列坐标
 * @param  color — RGB565 颜色值
 */
static void lcd_dma_write_rgb565_pixel(uint8_t *buf, uint16_t out_x, uint16_t color)
{
    if (buf == 0 || out_x >= LCD_DMA_THERMAL_AREA_WIDTH)
    {
        return;
    }

    buf[2U * out_x]       = (uint8_t)(color >> 8);
    buf[2U * out_x + 1U]  = (uint8_t)(color & 0xFFU);
}

/**
 * @brief  将灰度值通过伪彩色 LUT 转换为 RGB565 并交换字节序
 * @param  pixel — 灰度值（0~255）
 * @return uint16_t — 大端序 RGB565 颜色值
 */
static inline uint16_t lcd_dma_gray_to_output_color(uint8_t pixel)
{
    uint16_t color = GCM_Pseudo3[pixel];
    return (uint16_t)((color >> 8) | (color << 8));
}

/**
 * @brief  获取伪彩色 LUT 中指定灰度的 RGB565 颜色（不交换字节序）
 * @param  pixel — 灰度值（0~255）
 * @return uint16_t — RGB565 颜色值
 */
uint16_t lcd_dma_palette_color_rgb565(uint8_t pixel)
{
    return GCM_Pseudo3[pixel];
}

/* =========================================================================
 *  12. 性能统计函数
 * ======================================================================= */

/**
 * @brief  重置当前帧的性能统计数据
 */
static void lcd_dma_perf_reset_frame(void)
{
    memset(&s_lcd_dma_frame_perf, 0, sizeof(s_lcd_dma_frame_perf));
}

/**
 * @brief  累加指定计数器的耗时（微秒）
 * @param  accum       — 输出：累加器指针
 * @param  start_cycle — 起始 DWT 周期计数
 */
static void lcd_dma_perf_add_elapsed(uint32_t *accum, uint32_t start_cycle)
{
    if (accum == 0)
    {
        return;
    }

    *accum += app_perf_baseline_elapsed_us(start_cycle);
}

/**
 * @brief  提交当前帧的性能统计数据到性能基准模块
 */
static void lcd_dma_perf_commit_frame(void)
{
    app_perf_baseline_record_lcd_dma_render_us(s_lcd_dma_frame_perf.render_us);
    app_perf_baseline_record_lcd_dma_start_us(s_lcd_dma_frame_perf.dma_start_us);
    app_perf_baseline_record_lcd_dma_wait_us(s_lcd_dma_frame_perf.dma_wait_us);
    app_perf_baseline_record_lcd_dma_spi_idle_us(s_lcd_dma_frame_perf.spi_idle_wait_us);
    app_perf_baseline_record_lcd_dma_overlay_us(s_lcd_dma_frame_perf.overlay_us);
}

/* =========================================================================
 *  13. 十字准星叠加
 * ======================================================================= */

/**
 * @brief  在指定输出行叠加十字准星
 * @note   十字准星由水平臂和垂直臂组成，中心留有间隙。
 *         仅在 redpic1_thermal 运行时 overlay 可见时绘制。
 * @param  out_row — 输出行号
 * @param  buf     — 行缓冲区指针
 */
static void lcd_dma_overlay_crosshair_row(uint16_t out_row, uint8_t *buf)
{
    uint16_t center_x     = (uint16_t)(LCD_DMA_THERMAL_AREA_WIDTH / 2U);
    uint16_t center_y     = (uint16_t)(LCD_DMA_THERMAL_AREA_HEIGHT / 2U);
    uint16_t left_start   = 0U;
    uint16_t left_end     = 0U;
    uint16_t right_start  = 0U;
    uint16_t right_end    = 0U;
    uint16_t top_start    = 0U;
    uint16_t top_end      = 0U;
    uint16_t bottom_start = (uint16_t)(center_y + LCD_DMA_THERMAL_CROSS_GAP_SIZE);
    uint16_t bottom_end   = (uint16_t)(center_y + LCD_DMA_THERMAL_CROSS_HALF_SIZE);

    if (buf == 0 || out_row >= LCD_DMA_THERMAL_AREA_HEIGHT ||
        redpic1_thermal_runtime_overlay_visible() == 0U)
    {
        return;
    }

    /* 计算水平臂的左右段起止列 */
    left_start = (center_x > LCD_DMA_THERMAL_CROSS_HALF_SIZE) ?
                 (uint16_t)(center_x - LCD_DMA_THERMAL_CROSS_HALF_SIZE) : 0U;
    left_end   = (center_x > LCD_DMA_THERMAL_CROSS_GAP_SIZE) ?
                 (uint16_t)(center_x - LCD_DMA_THERMAL_CROSS_GAP_SIZE) : 0U;
    right_start = (uint16_t)(center_x + LCD_DMA_THERMAL_CROSS_GAP_SIZE);
    right_end   = (uint16_t)(center_x + LCD_DMA_THERMAL_CROSS_HALF_SIZE);

    /* 计算垂直臂的上下段起止行 */
    top_start = (center_y > LCD_DMA_THERMAL_CROSS_HALF_SIZE) ?
                (uint16_t)(center_y - LCD_DMA_THERMAL_CROSS_HALF_SIZE) : 0U;
    top_end   = (center_y > LCD_DMA_THERMAL_CROSS_GAP_SIZE) ?
                (uint16_t)(center_y - LCD_DMA_THERMAL_CROSS_GAP_SIZE) : 0U;

    /* ---- 水平臂：在中心行绘制左右两段 + 中心点 ---- */
    if (out_row == center_y)
    {
        uint32_t overlay_start_cycle = app_perf_baseline_cycle_now();
        uint16_t out_x = 0U;

        /* 左段 */
        for (out_x = left_start; out_x <= left_end && out_x < LCD_DMA_THERMAL_AREA_WIDTH; ++out_x)
        {
            lcd_dma_write_rgb565_pixel(buf, out_x, LCD_DMA_THERMAL_CROSS_COLOR_PRODUCT);
        }
        /* 右段 */
        for (out_x = right_start; out_x <= right_end && out_x < LCD_DMA_THERMAL_AREA_WIDTH; ++out_x)
        {
            lcd_dma_write_rgb565_pixel(buf, out_x, LCD_DMA_THERMAL_CROSS_COLOR_PRODUCT);
        }
        /* 中心点 */
        lcd_dma_write_rgb565_pixel(buf, center_x, LCD_DMA_THERMAL_CROSS_CENTER_PRODUCT);
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.overlay_us, overlay_start_cycle);
        return;
    }

    /* ---- 垂直臂：在上下段范围内绘制中心列 ---- */
    if ((out_row >= top_start && out_row <= top_end) ||
        (out_row >= bottom_start && out_row <= bottom_end))
    {
        uint32_t overlay_start_cycle = app_perf_baseline_cycle_now();
        lcd_dma_write_rgb565_pixel(buf, center_x, LCD_DMA_THERMAL_CROSS_COLOR_PRODUCT);
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.overlay_us, overlay_start_cycle);
    }
}

/* =========================================================================
 *  14. 坐标缩放与渲染映射初始化
 * ======================================================================= */

/**
 * @brief  均匀缩放坐标轴映射
 * @note   将输出坐标线性映射到插值坐标，使用四舍五入减少误差。
 * @param  index        — 输出坐标
 * @param  output_count — 输出轴总长度
 * @param  interp_count — 插值轴总长度
 * @return uint16_t — 对应的插值坐标
 */
static uint16_t lcd_dma_scale_axis(uint16_t index, uint16_t output_count, uint16_t interp_count)
{
    uint32_t numerator   = 0U;
    uint32_t denominator = 0U;

    if (output_count <= 1U || interp_count <= 1U)
    {
        return 0U;
    }

    denominator = (uint32_t)(output_count - 1U);
    numerator   = ((uint32_t)index * (uint32_t)(interp_count - 1U)) + (denominator / 2U);
    return (uint16_t)(numerator / denominator);
}

#if LCD_DMA_STAGE6_6B_ACTIVE
/**
 * @brief  初始化渲染映射表
 * @note   预计算所有输出行/列到插值行/列的映射关系，
 *         避免逐帧逐像素重复计算，显著降低渲染阶段 CPU 开销。
 *         仅在首次调用时执行，后续调用直接返回。
 */
static void lcd_dma_init_render_mappings(void)
{
    uint16_t out_row = 0U;
    uint16_t out_col = 0U;

    if (s_renderMappingReady != 0U)
    {
        return;
    }

    /* ---- 1. 构建插值行元数据（区分顶部边缘/主体/底部边缘） ---- */
    for (uint16_t interp_row = 0U; interp_row < THERMAL_RENDER_HEIGHT; ++interp_row)
    {
        lcd_dma_interp_row_meta_t *meta = &g_interpRowMeta[interp_row];

        if (interp_row < TOP_EDGE_ROWS)
        {
            /* 顶部边缘外推行 */
            meta->kind       = LCD_DMA_INTERP_ROW_TOP;
            meta->base_index = (uint8_t)interp_row;
            meta->ratio      = 0U;
        }
        else if (interp_row >= BOTTOM_EDGE_START)
        {
            /* 底部边缘外推行 */
            meta->kind       = LCD_DMA_INTERP_ROW_BOTTOM;
            meta->base_index = (uint8_t)(interp_row - BOTTOM_EDGE_START);
            meta->ratio      = 0U;
        }
        else
        {
            /* 主体插值行：计算基准行索引和插值比例 */
            uint16_t offset  = (uint16_t)(interp_row - TOP_EDGE_ROWS);
            meta->kind       = LCD_DMA_INTERP_ROW_BODY;
            meta->base_index = (uint8_t)(offset / INTERP_STRIDE);
            meta->ratio      = (uint8_t)(offset % INTERP_STRIDE);
        }
    }

    /* ---- 2. 构建输出坐标到插值坐标的映射表 ---- */
#if USE_HORIZONTAL == 0 || USE_HORIZONTAL == 1
    /* 竖屏模式：行->行反转映射，列->列反转映射 */
    for (out_row = 0U; out_row < LCD_H; ++out_row)
    {
        if (out_row < LCD_DMA_THERMAL_AREA_HEIGHT)
        {
            uint16_t mapped_row = lcd_dma_scale_axis(out_row,
                                                     LCD_DMA_THERMAL_AREA_HEIGHT,
                                                     THERMAL_RENDER_HEIGHT);
            g_outputRowToInterpRow[out_row] = (uint16_t)(THERMAL_RENDER_HEIGHT - 1U - mapped_row);
        }
        else
        {
            g_outputRowToInterpRow[out_row] = 0U;
        }
    }
    for (out_col = 0U; out_col < LCD_W; ++out_col)
    {
        if (out_col < LCD_DMA_THERMAL_AREA_WIDTH)
        {
            uint16_t mapped_col = lcd_dma_scale_axis(out_col,
                                                     LCD_DMA_THERMAL_AREA_WIDTH,
                                                     THERMAL_RENDER_WIDTH);
            g_outputColToInterpCol[out_col] = (uint16_t)(THERMAL_RENDER_WIDTH - 1U - mapped_col);
        }
        else
        {
            g_outputColToInterpCol[out_col] = 0U;
        }
    }
#elif USE_HORIZONTAL == 2
    /* 横屏模式：行映射到列，列映射到行 */
    for (out_row = 0U; out_row < LCD_H; ++out_row)
    {
        if (out_row < LCD_DMA_THERMAL_AREA_HEIGHT)
        {
            uint16_t mapped_col = lcd_dma_scale_axis(out_row,
                                                     LCD_DMA_THERMAL_AREA_HEIGHT,
                                                     THERMAL_RENDER_WIDTH);
            g_outputRowToInterpCol[out_row] = (uint16_t)(THERMAL_RENDER_WIDTH - 1U - mapped_col);
        }
        else
        {
            g_outputRowToInterpCol[out_row] = 0U;
        }
    }
    for (out_col = 0U; out_col < LCD_W; ++out_col)
    {
        if (out_col < LCD_DMA_THERMAL_AREA_WIDTH)
        {
            g_outputColToInterpRow[out_col] = lcd_dma_scale_axis(out_col,
                                                                  LCD_DMA_THERMAL_AREA_WIDTH,
                                                                  THERMAL_RENDER_HEIGHT);
        }
        else
        {
            g_outputColToInterpRow[out_col] = 0U;
        }
    }
#endif

    s_renderMappingReady = 1U;
}

/**
 * @brief  从插值缓冲区采样指定位置的灰度值
 * @note   热路径函数，使用 inline 强制内联消除函数调用开销。
 *         根据行类型（边缘/主体）选择不同的采样策略：
 *         - 边缘行：直接查表
 *         - 主体行：双线性插值（ratio=0 时退化为直接查表）
 * @param  meta — 插值行元数据指针
 * @param  col  — 插值列坐标
 * @return uint8_t — 采样灰度值
 */
static inline uint8_t lcd_dma_sample_interp_row(const lcd_dma_interp_row_meta_t *meta, uint16_t col)
{
    uint8_t  kind = meta->kind;
    uint16_t base = meta->base_index;

    if (kind == LCD_DMA_INTERP_ROW_TOP)
    {
        /* 顶部边缘外推行：直接查表 */
        return g_topEdgeRows[base][col];
    }
    else if (kind == LCD_DMA_INTERP_ROW_BOTTOM)
    {
        /* 底部边缘外推行：直接查表 */
        return g_bottomEdgeRows[base][col];
    }
    else
    {
        /* 主体插值行：加权平均 */
        uint8_t  ratio = meta->ratio;
        uint8_t  p1    = g_interpRows[base][col];

        if (ratio == 0U)
        {
            return p1;
        }

        uint8_t  p2  = g_interpRows[base + 1U][col];
        uint32_t val = (uint32_t)(p1 * (INTERP_STRIDE - ratio) + p2 * ratio);

        /* 使用乘法+移位替代除法：(val * 205) >> 11 ≈ val / 10 */
#if INTERP_STRIDE == 10
        return (uint8_t)FAST_DIV_10(val);
#else
        return (uint8_t)(val / INTERP_STRIDE);
#endif
    }
}
#endif /* LCD_DMA_STAGE6_6B_ACTIVE */

/* =========================================================================
 *  15. 插值管线：水平插值与垂直边缘外推
 * ======================================================================= */

/**
 * @brief  水平方向插值：将每行 24 列扩展为 240 列
 * @note   对每对相邻像素进行 10 等分线性插值，
 *         左右各 5 个边缘像素使用 __USAT 硬件饱和指令进行外推。
 * @param  frameData — 输入 24x32 灰度帧数据
 */
static void build_horizontal_interp_rows(const uint8_t *frameData)
{
    for (int row = 0; row < THERMAL_SRC_HEIGHT; row++)
    {
        uint8_t       *dst     = g_interpRows[row];
        const uint8_t *src     = &frameData[row * THERMAL_SRC_WIDTH];
        uint8_t       *dst_ptr = dst + TOP_EDGE_ROWS;

        /* 主体插值：每对相邻像素插值 10 个点（含左端点） */
        for (int i = 0; i < (THERMAL_SRC_WIDTH - 1); i++)
        {
            uint32_t p0 = src[i];
            uint32_t p1 = src[i + 1];

            /* 后索引寻址 *dst_ptr++ 触发 ARM STRB Rx, [Ry], #1 指令 */
            *dst_ptr++ = (uint8_t)p0;
            *dst_ptr++ = (uint8_t)(((p0 * 9 + p1)     * 205U) >> 11U);
            *dst_ptr++ = (uint8_t)(((p0 * 8 + p1 * 2) * 205U) >> 11U);
            *dst_ptr++ = (uint8_t)(((p0 * 7 + p1 * 3) * 205U) >> 11U);
            *dst_ptr++ = (uint8_t)(((p0 * 6 + p1 * 4) * 205U) >> 11U);
            *dst_ptr++ = (uint8_t)(((p0 * 5 + p1 * 5) * 205U) >> 11U);
            *dst_ptr++ = (uint8_t)(((p0 * 4 + p1 * 6) * 205U) >> 11U);
            *dst_ptr++ = (uint8_t)(((p0 * 3 + p1 * 7) * 205U) >> 11U);
            *dst_ptr++ = (uint8_t)(((p0 * 2 + p1 * 8) * 205U) >> 11U);
            *dst_ptr++ = (uint8_t)(((p0     + p1 * 9) * 205U) >> 11U);
        }

        /* 最后一个源像素直接复制 */
        *dst_ptr = src[THERMAL_SRC_WIDTH - 1];

        /* 左边缘外推：使用 __USAT 硬件饱和指令替代 clamp_to_u8 */
        for (int i = TOP_EDGE_ROWS - 1; i >= 0; i--)
        {
            dst[i] = (uint8_t)__USAT((2 * (int32_t)dst[i + 1] - (int32_t)dst[i + 2]), 8);
        }

        /* 右边缘外推 */
        for (int i = THERMAL_RENDER_WIDTH - TOP_EDGE_ROWS + 1; i < THERMAL_RENDER_WIDTH; i++)
        {
            dst[i] = (uint8_t)__USAT((2 * (int32_t)dst[i - 1] - (int32_t)dst[i - 2]), 8);
        }
    }
}

/**
 * @brief  计算顶部与底部边缘外推行
 * @note   基于插值结果缓冲区的首/末行，通过线性外推生成
 *         顶部 5 行和底部 4 行的虚拟数据，用于完整覆盖渲染区域。
 */
static void build_vertical_edge_rows(void)
{
    /* ---- 顶部边缘外推 ---- */

    /* 最靠近主体的顶部边缘行（第 4 行）：基于插值第 0 行外推 */
    for (int x = 0; x < THERMAL_RENDER_WIDTH; x++)
    {
        uint8_t row5 = g_interpRows[0][x];
        uint8_t row6 = (uint8_t)((g_interpRows[0][x] * 9 + g_interpRows[1][x]) / INTERP_STRIDE);
        g_topEdgeRows[TOP_EDGE_ROWS - 1][x] = clamp_to_u8(2 * row5 - row6);
    }

    /* 向外逐行外推（第 3、2、1、0 行） */
    for (int row = TOP_EDGE_ROWS - 2; row >= 0; row--)
    {
        const uint8_t *next1 = g_topEdgeRows[row + 1];
        const uint8_t *next2 = (row == TOP_EDGE_ROWS - 2) ? g_interpRows[0] : g_topEdgeRows[row + 2];

        for (int x = 0; x < THERMAL_RENDER_WIDTH; x++)
        {
            g_topEdgeRows[row][x] = clamp_to_u8(2 * next1[x] - next2[x]);
        }
    }

    /* ---- 底部边缘外推 ---- */

    /* 最靠近主体的底部边缘行（第 0 行）：基于插值末行外推 */
    for (int x = 0; x < THERMAL_RENDER_WIDTH; x++)
    {
        uint8_t row314 = (uint8_t)((g_interpRows[30][x] + g_interpRows[31][x] * 9) / INTERP_STRIDE);
        uint8_t row315 = g_interpRows[31][x];
        g_bottomEdgeRows[0][x] = clamp_to_u8(2 * row315 - row314);
    }

    /* 向外逐行外推（第 1、2、3 行） */
    for (int row = 1; row < BOTTOM_EDGE_ROWS; row++)
    {
        const uint8_t *prev1 = g_bottomEdgeRows[row - 1];
        const uint8_t *prev2 = (row == 1) ? g_interpRows[31] : g_bottomEdgeRows[row - 2];

        for (int x = 0; x < THERMAL_RENDER_WIDTH; x++)
        {
            g_bottomEdgeRows[row][x] = clamp_to_u8(2 * prev1[x] - prev2[x]);
        }
    }
}

/* =========================================================================
 *  16. 行渲染与缓冲区填充
 * ======================================================================= */

/**
 * @brief  渲染单行输出数据到缓冲区
 * @note   根据 USE_HORIZONTAL 选择不同的映射路径，
 *         通过预计算的映射表快速采样并转换为 RGB565。
 * @param  out_row — 输出行号
 * @param  buf     — 行缓冲区指针
 */
static void lcd_dma_render_legacy_row(uint16_t out_row, uint8_t *buf)
{
    uint16_t *buf16 = (uint16_t *)buf;

    if (buf16 == 0 || out_row >= LCD_DMA_THERMAL_AREA_HEIGHT)
    {
        return;
    }

#if LCD_DMA_STAGE6_6B_ACTIVE && (USE_HORIZONTAL == 0 || USE_HORIZONTAL == 1)
    {
        /* 竖屏模式：行映射 + 列映射 */
        const lcd_dma_interp_row_meta_t *row_meta =
            &g_interpRowMeta[g_outputRowToInterpRow[out_row]];

        for (uint16_t out_x = 0U; out_x < LCD_DMA_THERMAL_AREA_WIDTH; ++out_x)
        {
            buf16[out_x] = lcd_dma_gray_to_output_color(
                lcd_dma_sample_interp_row(row_meta, g_outputColToInterpCol[out_x]));
        }
    }
#elif LCD_DMA_STAGE6_6B_ACTIVE
    {
        /* 横屏模式：行映射到列，列映射到行 */
        uint16_t interp_col = g_outputRowToInterpCol[out_row];

        for (uint16_t out_x = 0U; out_x < LCD_DMA_THERMAL_AREA_WIDTH; ++out_x)
        {
            const lcd_dma_interp_row_meta_t *row_meta =
                &g_interpRowMeta[g_outputColToInterpRow[out_x]];
            buf16[out_x] = lcd_dma_gray_to_output_color(
                lcd_dma_sample_interp_row(row_meta, interp_col));
        }
    }
#endif
}

/**
 * @brief  生成指定输出行的 RGB565 数据（含十字准星叠加）
 * @param  outRow — 输出行号
 * @param  buf    — 行缓冲区指针
 */
static void render_output_row_to_buffer(uint16_t outRow, uint8_t *buf)
{
    if (buf == 0)
    {
        return;
    }

    lcd_dma_render_legacy_row(outRow, buf);
    lcd_dma_overlay_crosshair_row(outRow, buf);
}

/**
 * @brief  计算指定起始行的条带行数
 * @note   取剩余行数与条带行数的较小值。
 * @param  start_row — 起始行号
 * @return uint16_t — 本次条带的行数
 */
static uint16_t lcd_dma_get_stripe_row_count(uint16_t start_row)
{
    uint16_t remaining_rows = 0U;

    if (start_row >= LCD_DMA_THERMAL_AREA_HEIGHT)
    {
        return 0U;
    }

    remaining_rows = (uint16_t)(LCD_DMA_THERMAL_AREA_HEIGHT - start_row);
    if (remaining_rows > LCD_DMA_THERMAL_STRIPE_ROWS)
    {
        remaining_rows = LCD_DMA_THERMAL_STRIPE_ROWS;
    }

    return remaining_rows;
}

/**
 * @brief  渲染多行输出数据到缓冲区
 * @param  start_row — 起始行号
 * @param  row_count — 行数
 * @param  buf       — 缓冲区指针
 */
static void render_output_rows_to_buffer(uint16_t start_row, uint16_t row_count, uint8_t *buf)
{
    uint32_t render_start_cycle = app_perf_baseline_cycle_now();
    uint16_t stripe_row  = 0U;
    uint16_t row_stride  = lcd_dma_active_line_bytes();

    if (buf == 0)
    {
        return;
    }

    for (stripe_row = 0U; stripe_row < row_count; ++stripe_row)
    {
        render_output_row_to_buffer((uint16_t)(start_row + stripe_row),
                                    &buf[(uint32_t)stripe_row * row_stride]);
    }

    lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.render_us, render_start_cycle);
}

/* =========================================================================
 *  17. DMA 传输控制
 * ======================================================================= */

/**
 * @brief  等待 DMA Stream 关闭
 * @note   若 DMA 流仍在运行则主动关闭，超时后标记错误状态。
 * @retval 1 — DMA 流已关闭；0 — 超时
 */
static uint8_t lcd_dma_wait_stream_disabled(void)
{
    uint32_t timeout = DMA_STREAM_DISABLE_WAIT_LOOPS;

    if (DMA_GetCmdStatus(DMA2_Stream3) == DISABLE)
    {
        return 1U;
    }

    DMA_Cmd(DMA2_Stream3, DISABLE);
    while (DMA_GetCmdStatus(DMA2_Stream3) != DISABLE)
    {
        if (timeout-- == 0U)
        {
            s_dma_mode        = LCD_DMA_MODE_IDLE;
            transferComplete  = 1U;
            s_dma_last_result = 0U;
            s_dma_last_status = APP_PERF_LCD_DMA_STATUS_ERROR;
            return 0U;
        }
    }

    return 1U;
}

/**
 * @brief  等待 SPI 总线空闲
 * @note   依次检查 TXE（发送缓冲区空）和 BSY（总线忙）标志，
 *         确保上一次 SPI 传输完全结束后再进行下一次 DMA 配置。
 * @retval 1 — SPI 空闲；0 — 超时
 */
static uint8_t lcd_dma_wait_spi_idle(void)
{
    uint32_t wait_start_cycle = app_perf_baseline_cycle_now();
    uint32_t timeout = LCD_SPI_IDLE_WAIT_LOOPS;

    /* 等待发送缓冲区空 */
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
    {
        if (timeout-- == 0U)
        {
            s_dma_mode        = LCD_DMA_MODE_IDLE;
            s_dma_last_result = 0U;
            s_dma_last_status = APP_PERF_LCD_DMA_STATUS_ERROR;
            lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.spi_idle_wait_us, wait_start_cycle);
            return 0U;
        }
    }

    /* 等待总线不忙 */
    timeout = LCD_SPI_IDLE_WAIT_LOOPS;
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)
    {
        if (timeout-- == 0U)
        {
            s_dma_mode        = LCD_DMA_MODE_IDLE;
            s_dma_last_result = 0U;
            s_dma_last_status = APP_PERF_LCD_DMA_STATUS_ERROR;
            lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.spi_idle_wait_us, wait_start_cycle);
            return 0U;
        }
    }

    lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.spi_idle_wait_us, wait_start_cycle);
    return 1U;
}

/**
 * @brief  启动一次 DMA 行发送
 * @note   配置 DMA2 Stream3 源地址和传输长度后使能传输。
 * @param  buf       — 发送缓冲区指针
 * @param  row_count — 本次发送的行数
 * @retval 1 — 启动成功；0 — 失败
 */
static uint8_t start_dma_line_transfer(uint8_t *buf, uint16_t row_count)
{
    uint32_t start_cycle   = app_perf_baseline_cycle_now();
    uint16_t transfer_size = 0U;

    if (buf == 0 || row_count == 0U || lcd_dma_wait_stream_disabled() == 0U)
    {
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_start_us, start_cycle);
        return 0U;
    }

    transfer_size = (uint16_t)(lcd_dma_active_line_bytes() * row_count);

    /* 配置 DMA 传输参数 */
    s_dma_mode        = LCD_DMA_MODE_THERMAL;
    transferComplete  = 0U;
    s_dma_last_result = 0U;
    s_dma_last_status = APP_PERF_LCD_DMA_STATUS_TIMEOUT;

    /* 清除所有 DMA 中断标志 */
    DMA_ClearFlag(DMA2_Stream3,
                  DMA_FLAG_FEIF3 | DMA_FLAG_DMEIF3 |
                  DMA_FLAG_TEIF3 | DMA_FLAG_HTIF3 | DMA_FLAG_TCIF3);

    DMA2_Stream3->M0AR = (uint32_t)buf;
    DMA_SetCurrDataCounter(DMA2_Stream3, transfer_size);
    DMA_Cmd(DMA2_Stream3, ENABLE);

    lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_start_us, start_cycle);
    return 1U;
}

/**
 * @brief  判断 FreeRTOS 调度器是否正在运行
 * @retval 1 — 运行中；0 — 未启动
 */
static uint8_t lcd_dma_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

/**
 * @brief  忙等待 DMA 传输完成（调度器未运行时使用）
 * @note   超时后主动终止 DMA 并清除中断标志。
 * @retval 1 — 传输成功；0 — 超时或失败
 */
static uint8_t lcd_dma_wait_busy_loop(void)
{
    uint32_t wait_start_cycle = app_perf_baseline_cycle_now();
    uint32_t timeout = DMA_TRANSFER_WAIT_LOOPS;

    while (!transferComplete)
    {
        if (timeout-- == 0U)
        {
            /* 超时：强制关闭 DMA 并清除标志 */
            DMA_Cmd(DMA2_Stream3, DISABLE);
            DMA_ClearFlag(DMA2_Stream3,
                          DMA_FLAG_FEIF3 | DMA_FLAG_DMEIF3 |
                          DMA_FLAG_TEIF3 | DMA_FLAG_HTIF3 | DMA_FLAG_TCIF3);
            s_dma_mode        = LCD_DMA_MODE_IDLE;
            transferComplete  = 1U;
            s_dma_last_result = 0U;
            s_dma_last_status = APP_PERF_LCD_DMA_STATUS_TIMEOUT;
            lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
            return 0U;
        }
    }

    if (s_dma_last_result != 0U)
    {
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
        return lcd_dma_wait_spi_idle();
    }

    lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
    return 0U;
}

/**
 * @brief  等待 DMA 传输完成
 * @note   调度器运行时使用 TaskNotify 机制阻塞等待（低功耗），
 *         调度器未运行时退化为忙等待。
 *         超时后主动终止 DMA，避免主循环长期卡死。
 * @retval 1 — 传输成功；0 — 超时或失败
 */
static uint8_t wait_for_dma_transfer_complete(void)
{
    uint32_t     wait_start_cycle = app_perf_baseline_cycle_now();
    TickType_t   wait_ticks       = pdMS_TO_TICKS(DMA_TRANSFER_WAIT_TIMEOUT_MS);
    TaskHandle_t current_task     = 0;

    /* 调度器未运行：退化为忙等待 */
    if (lcd_dma_scheduler_running() == 0U)
    {
        return lcd_dma_wait_busy_loop();
    }

    /* 传输已完成：直接检查结果 */
    if (transferComplete != 0U)
    {
        if (s_dma_last_result != 0U)
        {
            lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
            return lcd_dma_wait_spi_idle();
        }
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
        return 0U;
    }

    /* 注册当前任务为等待者 */
    current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0U);

    taskENTER_CRITICAL();
    if (transferComplete == 0U)
    {
        s_dma_wait_task = current_task;
    }
    taskEXIT_CRITICAL();

    /* 再次检查（ISR 可能在临界区外已完成传输） */
    if (transferComplete != 0U)
    {
        taskENTER_CRITICAL();
        if (s_dma_wait_task == current_task)
        {
            s_dma_wait_task = 0;
        }
        taskEXIT_CRITICAL();

        if (s_dma_last_result != 0U)
        {
            lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
            return lcd_dma_wait_spi_idle();
        }
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
        return 0U;
    }

    /* 阻塞等待 DMA 完成通知或超时 */
    if (ulTaskNotifyTake(pdTRUE, wait_ticks) == 0U)
    {
        /* 超时：清除等待者注册，强制关闭 DMA */
        taskENTER_CRITICAL();
        if (s_dma_wait_task == current_task)
        {
            s_dma_wait_task = 0;
        }
        taskEXIT_CRITICAL();

        DMA_Cmd(DMA2_Stream3, DISABLE);
        DMA_ClearFlag(DMA2_Stream3,
                      DMA_FLAG_FEIF3 | DMA_FLAG_DMEIF3 |
                      DMA_FLAG_TEIF3 | DMA_FLAG_HTIF3 | DMA_FLAG_TCIF3);
        s_dma_mode        = LCD_DMA_MODE_IDLE;
        transferComplete  = 1U;
        s_dma_last_result = 0U;
        s_dma_last_status = APP_PERF_LCD_DMA_STATUS_TIMEOUT;
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
        return 0U;
    }

    /* 收到通知：记录并检查结果 */
    app_perf_baseline_record_dma_wait_take();
    if (s_dma_last_result != 0U)
    {
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
        return lcd_dma_wait_spi_idle();
    }

    lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
    return 0U;
}

/* =========================================================================
 *  18. DMA 中断服务程序
 * ======================================================================= */

/**
 * @brief  DMA2 Stream3 中断处理函数
 * @note   处理传输完成（TC）和传输错误（TE）两种中断：
 *         - TC：标记传输成功，唤醒等待任务
 *         - TE：强制关闭 DMA，拉高 CS，标记错误，唤醒等待任务
 */
void DMA2_Stream3_IRQHandler(void)
{
    BaseType_t   higher_priority_task_woken = pdFALSE;
    TaskHandle_t waiting_task               = 0;

    /* ---- 传输完成中断 ---- */
    if (DMA_GetITStatus(DMA2_Stream3, DMA_IT_TCIF3) != RESET)
    {
        app_perf_baseline_record_dma_irq_tc();
        DMA_ClearITPendingBit(DMA2_Stream3, DMA_IT_TCIF3);

        transferComplete  = 1U;
        s_dma_last_result = 1U;
        s_dma_last_status = APP_PERF_LCD_DMA_STATUS_OK;

        waiting_task    = (TaskHandle_t)s_dma_wait_task;
        s_dma_wait_task = 0;
    }

    /* ---- 传输错误中断 ---- */
    if (DMA_GetITStatus(DMA2_Stream3, DMA_IT_TEIF3) != RESET)
    {
        app_perf_baseline_record_dma_irq_te();

        /* 清除所有 DMA 标志并关闭流 */
        DMA_ClearFlag(DMA2_Stream3,
                      DMA_FLAG_FEIF3 | DMA_FLAG_DMEIF3 |
                      DMA_FLAG_TEIF3 | DMA_FLAG_HTIF3 | DMA_FLAG_TCIF3);
        DMA_Cmd(DMA2_Stream3, DISABLE);

        transferComplete  = 1U;
        s_dma_last_result = 0U;
        s_dma_last_status = APP_PERF_LCD_DMA_STATUS_ERROR;
        s_dma_mode        = LCD_DMA_MODE_IDLE;

        /* 拉高 CS 释放 SPI 总线 */
        LCD_CS_Set();

        waiting_task    = (TaskHandle_t)s_dma_wait_task;
        s_dma_wait_task = 0;
    }

    /* 唤醒等待中的任务 */
    if (waiting_task != 0)
    {
        vTaskNotifyGiveFromISR(waiting_task, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

/* =========================================================================
 *  19. RGB 颜色转换
 * ======================================================================= */

/**
 * @brief  将 RGB888 分量合成为 RGB565
 * @param  COLOR_R — 红色分量（0~255）
 * @param  COLOR_G — 绿色分量（0~255）
 * @param  COLOR_B — 蓝色分量（0~255）
 * @return uint16_t — RGB565 颜色值
 */
uint16_t rgb_565(uint16_t COLOR_R, uint16_t COLOR_G, uint16_t COLOR_B)
{
    uint16_t RGB565 = 0;
    RGB565 = ((COLOR_R & 0XF8) << 8) + ((COLOR_G & 0XFC) << 3) + ((COLOR_B & 0XF8) >> 3);
    return RGB565;
}

/* =========================================================================
 *  20. 伪彩色调色板生成
 * ======================================================================= */

/**
 * @brief  将灰度值按指定模式映射为伪彩色 RGB565
 * @note   支持 9 种调色板模式（mode 0~8），每种模式定义了
 *         不同的灰度->RGB 映射曲线。
 * @param  grayValue — 灰度值（0~255）
 * @param  mode      — 调色板模式（0~8）
 * @return uint16_t — RGB565 颜色值
 */
uint16_t color_code(uint16_t grayValue, uint16_t mode)
{
    uint16_t colorR, colorG, colorB;
    colorR = 0;
    colorG = 0;
    colorB = 0;

    /* ---- mode 0：冷暖色调（蓝-绿-红） ---- */
    if (mode == 0)
    {
        colorR = abs(0 - grayValue);
        colorG = abs(127 - grayValue);
        colorB = abs(255 - grayValue);
    }
    /* ---- mode 1：彩虹色调（蓝->青->绿->黄->红） ---- */
    else if (mode == 1)
    {
        if ((grayValue > 0) && (grayValue <= 63))
        {
            colorR = 0;
            colorG = 0;
            colorB = round(grayValue / 64.0 * 255.0);
        }
        else if ((grayValue >= 64) && (grayValue <= 127))
        {
            colorR = 0;
            colorG = round((grayValue - 64) / 64.0 * 255.0);
            colorB = round((127 - grayValue) / 64.0 * 255.0);
        }
        else if ((grayValue >= 128) && (grayValue <= 191))
        {
            colorR = round((grayValue - 128) / 64.0 * 255.0);
            colorG = 255;
            colorB = 0;
        }
        else if ((grayValue >= 192) && (grayValue <= 255))
        {
            colorR = 255;
            colorG = round((255 - grayValue) / 64.0 * 255.0);
            colorB = 0;
        }
    }
    /* ---- mode 2：增强彩虹（含白色过渡段） ---- */
    else if (mode == 2)
    {
        if ((grayValue > 0) && (grayValue <= 63))
        {
            colorR = 0;
            colorG = 0;
            colorB = round(grayValue / 64.0 * 255.0);
        }
        else if ((grayValue >= 64) && (grayValue <= 95))
        {
            colorR = round((grayValue - 63) / 32.0 * 127.0);
            colorG = round((grayValue - 63) / 32.0 * 127.0);
            colorB = 255;
        }
        else if ((grayValue >= 96) && (grayValue <= 127))
        {
            colorR = round((grayValue - 95) / 32.0 * 127.0) + 128;
            colorG = round((grayValue - 95) / 32.0 * 127.0) + 128;
            colorB = round((127 - grayValue) / 32.0 * 255.0);
        }
        else if ((grayValue >= 128) && (grayValue <= 191))
        {
            colorR = 255;
            colorG = 255;
            colorB = 0;
        }
        else if ((grayValue >= 192) && (grayValue <= 255))
        {
            colorR = 255;
            colorG = 255;
            colorB = round((grayValue - 192) / 64 * 255.0);
        }
    }
    /* ---- mode 3：铁红色调（黑->暗红->亮红->黄->白） ---- */
    else if (mode == 3)
    {
        colorR = 0;
        colorG = 0;
        colorB = 0;

        if ((grayValue > 0) && (grayValue <= 16))
        {
            colorR = 0;
        }
        else if ((grayValue >= 17) && (grayValue <= 140))
        {
            colorR = round((grayValue - 16) / 124.0 * 255.0);
        }
        else if ((grayValue >= 141) && (grayValue <= 255))
        {
            colorR = 255;
        }

        if ((grayValue > 0) && (grayValue <= 101))
        {
            colorG = 0;
        }
        else if ((grayValue >= 102) && (grayValue <= 218))
        {
            colorG = round((grayValue - 101) / 117.0 * 255.0);
        }
        else if ((grayValue >= 219) && (grayValue <= 255))
        {
            colorG = 255;
        }

        if ((grayValue > 0) && (grayValue <= 91))
        {
            colorB = 28 + round((grayValue - 0) / 91.0 * 100.0);
        }
        else if ((grayValue >= 92) && (grayValue <= 120))
        {
            colorB = round((120 - grayValue) / 29.0 * 128.0);
        }
        else if ((grayValue >= 129) && (grayValue <= 214))
        {
            colorB = 0;
        }
        else if ((grayValue >= 215) && (grayValue <= 255))
        {
            colorB = round((grayValue - 214) / 41.0 * 255.0);
        }
    }
    /* ---- mode 4：蓝-青-绿-黄-红-白 渐变 ---- */
    else if (mode == 4)
    {
        if ((grayValue > 0) && (grayValue <= 31))
        {
            colorR = 0;
            colorG = 0;
            colorB = round(grayValue / 32.0 * 255.0);
        }
        else if ((grayValue >= 32) && (grayValue <= 63))
        {
            colorR = 0;
            colorG = round((grayValue - 32) / 32.0 * 255.0);
            colorB = 255;
        }
        else if ((grayValue >= 64) && (grayValue <= 95))
        {
            colorR = 0;
            colorG = 255;
            colorB = round((95 - grayValue) / 32.0 * 255.0);
        }
        else if ((grayValue >= 96) && (grayValue <= 127))
        {
            colorR = round((grayValue - 96) / 32.0 * 255.0);
            colorG = 255;
            colorB = 0;
        }
        else if ((grayValue >= 128) && (grayValue <= 191))
        {
            colorR = 255;
            colorG = round((191 - grayValue) / 64.0 * 255.0);
            colorB = 0;
        }
        else if ((grayValue >= 192) && (grayValue <= 255))
        {
            colorR = 255;
            colorG = round((grayValue - 192) / 64.0 * 255.0);
            colorB = round((grayValue - 192) / 64.0 * 255.0);
        }
    }
    /* ---- mode 5：冷色调变体 ---- */
    else if (mode == 5)
    {
        if ((grayValue > 0) && (grayValue <= 63))
        {
            colorR = 0;
            colorG = round((grayValue - 0) / 64.0 * 255.0);
            colorB = 255;
        }
        else if ((grayValue >= 64) && (grayValue <= 95))
        {
            colorR = 0;
            colorG = 255;
            colorB = round((95 - grayValue) / 32.0 * 255.0);
        }
        else if ((grayValue >= 96) && (grayValue <= 127))
        {
            colorR = round((grayValue - 96) / 32.0 * 255.0);
            colorG = 255;
            colorB = 0;
        }
        else if ((grayValue >= 128) && (grayValue <= 191))
        {
            colorR = 255;
            colorG = round((191 - grayValue) / 64.0 * 255.0);
            colorB = 0;
        }
        else if ((grayValue >= 192) && (grayValue <= 255))
        {
            colorR = 255;
            colorG = round((grayValue - 192) / 64.0 * 255.0);
            colorB = round((grayValue - 192) / 64.0 * 255.0);
        }
    }
    /* ---- mode 6：蓝-青-绿-黄-红 平滑渐变 ---- */
    else if (mode == 6)
    {
        if ((grayValue > 0) && (grayValue <= 51))
        {
            colorR = 0;
            colorG = grayValue * 5;
            colorB = 255;
        }
        else if ((grayValue >= 52) && (grayValue <= 102))
        {
            colorR = 0;
            colorG = 255;
            colorB = 255 - (grayValue - 51) * 5;
        }
        else if ((grayValue >= 103) && (grayValue <= 153))
        {
            colorR = (grayValue - 102) * 5;
            colorG = 255;
            colorB = 0;
        }
        else if ((grayValue >= 154) && (grayValue <= 204))
        {
            colorR = 255;
            colorG = round(255.0 - 128.0 * (grayValue - 153.0) / 51.0);
            colorB = 0;
        }
        else if ((grayValue >= 205) && (grayValue <= 255))
        {
            colorR = 255;
            colorG = round(127.0 - 127.0 * (grayValue - 204.0) / 51.0);
            colorB = 0;
        }
    }
    /* ---- mode 7：反向彩虹（蓝->青->绿->黄->红） ---- */
    else if (mode == 7)
    {
        if ((grayValue > 0) && (grayValue <= 63))
        {
            colorR = 0;
            colorG = round((64 - grayValue) / 64.0 * 255.0);
            colorB = 255;
        }
        else if ((grayValue >= 64) && (grayValue <= 127))
        {
            colorR = 0;
            colorG = round((grayValue - 64) / 64.0 * 255.0);
            colorB = round((127 - grayValue) / 64.0 * 255.0);
        }
        else if ((grayValue >= 128) && (grayValue <= 191))
        {
            colorR = round((grayValue - 128) / 64.0 * 255.0);
            colorG = 255;
            colorB = 0;
        }
        else if ((grayValue >= 192) && (grayValue <= 255))
        {
            colorR = 255;
            colorG = round((255 - grayValue) / 64.0 * 255.0);
            colorB = 0;
        }
    }
    /* ---- mode 8：高对比度彩虹（整数运算优化） ---- */
    else if (mode == 8)
    {
        if ((grayValue > 0) && (grayValue <= 63))
        {
            colorR = 0;
            colorG = 254 - 4 * grayValue;
            colorB = 255;
        }
        else if ((grayValue >= 64) && (grayValue <= 127))
        {
            colorR = 0;
            colorG = 4 * grayValue - 254;
            colorB = 510 - 4 * grayValue;
        }
        else if ((grayValue >= 128) && (grayValue <= 191))
        {
            colorR = 4 * grayValue - 510;
            colorG = 255;
            colorB = 0;
        }
        else if ((grayValue >= 192) && (grayValue <= 255))
        {
            colorR = 255;
            colorG = 1022 - 4 * grayValue;
            colorB = 0;
        }
    }
    /* ---- 默认：灰度直通 ---- */
    else
    {
        colorR = grayValue;
        colorG = grayValue;
        colorB = grayValue;
    }

    return rgb_565(colorR, colorG, colorB);
}

/* =========================================================================
 *  21. 色调映射与调色板管理
 * ======================================================================= */

/**
 * @brief  将 RGB888 转换为 RGB565
 */
static uint16_t lcd_dma_rgb565_from_rgb888(uint8_t r, uint8_t g, uint8_t b)
{
    return LCD_DMA_RGB565(r, g, b);
}

/**
 * @brief  构建色调压缩查找表
 * @note   对高灰度区域进行非线性压缩，将 THERMAL_TONE_LUT_START_GRAY 以上的
 *         灰度值映射到更窄的输出范围，避免高光区过曝。
 */
static void thermal_build_tone_lut(void)
{
    uint16_t i = 0U;

    for (i = 0U; i < 256U; ++i)
    {
#if THERMAL_ENABLE_TONE_LUT
        if (i <= THERMAL_TONE_LUT_START_GRAY)
        {
            /* 低灰度区：线性直通 */
            s_thermal_tone_lut[i] = (uint8_t)i;
        }
        else
        {
            /* 高灰度区：压缩映射 */
            uint16_t delta = (uint16_t)(i - THERMAL_TONE_LUT_START_GRAY);
            uint16_t mapped = (uint16_t)(THERMAL_TONE_LUT_START_GRAY +
                                         ((delta * THERMAL_TONE_LUT_OUTPUT_SPAN) /
                                          THERMAL_TONE_LUT_INPUT_SPAN));

            if (mapped > THERMAL_TONE_LUT_MAX_GRAY)
            {
                mapped = THERMAL_TONE_LUT_MAX_GRAY;
            }

            s_thermal_tone_lut[i] = (uint8_t)mapped;
        }
#else
        s_thermal_tone_lut[i] = (uint8_t)i;
#endif
    }

    s_thermal_tone_lut_ready = 1U;
}

/**
 * @brief  对灰度值应用色调映射
 * @param  gray — 输入灰度值
 * @return uint8_t — 映射后的灰度值
 */
static uint8_t thermal_apply_tone_map(uint8_t gray)
{
    if (s_thermal_tone_lut_ready == 0U)
    {
        thermal_build_tone_lut();
    }

    return s_thermal_tone_lut[gray];
}

/**
 * @brief  根据可见调色板模式获取 RGB565 颜色
 * @note   先应用色调映射，再根据模式选择白热/黑热或伪彩色调色板。
 * @param  gray         — 灰度值（0~255）
 * @param  visible_mode — 可见调色板模式索引
 * @return uint16_t — RGB565 颜色值
 */
static uint16_t thermal_visible_palette_color(uint8_t gray, uint16_t visible_mode)
{
    uint8_t mapped_gray  = thermal_apply_tone_map(gray);
    uint8_t real_palette = kVisiblePaletteMap[visible_mode % THERMAL_VISIBLE_PALETTE_COUNT];

#if THERMAL_ENABLE_OLD_PALETTE_MAP
    /* 白热模式：灰度直通 */
    if (real_palette == THERMAL_VISIBLE_PALETTE_WHITE_HOT)
    {
        return lcd_dma_rgb565_from_rgb888(mapped_gray, mapped_gray, mapped_gray);
    }

    /* 黑热模式：灰度反转 */
    if (real_palette == THERMAL_VISIBLE_PALETTE_BLACK_HOT)
    {
        uint8_t inverted = (uint8_t)(255U - mapped_gray);
        return lcd_dma_rgb565_from_rgb888(inverted, inverted, inverted);
    }

    /* 其他模式：查 color_code 表 */
    return color_code(mapped_gray, real_palette);
#else
    (void)real_palette;
    return GCM_Pseudo3[mapped_gray];
#endif
}

/**
 * @brief  生成整套 256 色伪彩色查找表
 * @param  color_list — 输出：256 元素的 RGB565 颜色数组
 * @param  mode       — 调色板模式
 */
void color_listcode(uint16_t *color_list, uint16_t mode)
{
    uint16_t gray = 0U;

    if (color_list == 0)
    {
        return;
    }

#if THERMAL_ENABLE_OLD_PALETTE_MAP
    for (gray = 0U; gray < 256U; ++gray)
    {
        color_list[gray] = thermal_visible_palette_color((uint8_t)gray, mode);
    }
#else
    thermal_palette_id_t palette_id = (thermal_palette_id_t)(mode % THERMAL_PALETTE_COUNT);

    switch (palette_id)
    {
    case THERMAL_PALETTE_IRON:
        thermal_palette_build_lut(kPaletteIron, LCD_DMA_ARRAY_SIZE(kPaletteIron), color_list);
        break;

    case THERMAL_PALETTE_RAINBOW:
        thermal_palette_build_lut(kPaletteRainbow, LCD_DMA_ARRAY_SIZE(kPaletteRainbow), color_list);
        break;

    case THERMAL_PALETTE_WHITE_HOT:
        thermal_palette_build_lut(kPaletteWhiteHot, LCD_DMA_ARRAY_SIZE(kPaletteWhiteHot), color_list);
        break;

    case THERMAL_PALETTE_BLACK_HOT:
        thermal_palette_build_lut(kPaletteBlackHot, LCD_DMA_ARRAY_SIZE(kPaletteBlackHot), color_list);
        break;

    case THERMAL_PALETTE_ARCTIC:
    default:
        thermal_palette_build_lut(kPaletteArctic, LCD_DMA_ARRAY_SIZE(kPaletteArctic), color_list);
        break;
    }
#endif
}

/**
 * @brief  设置伪彩色模式并重建全局 LUT
 * @param  mode — 调色板模式索引
 */
void set_color_mode(uint16_t mode)
{
    color_listcode(GCM_Pseudo3, (uint16_t)(mode % THERMAL_VISIBLE_PALETTE_COUNT));
}

/* =========================================================================
 *  22. DMA 硬件初始化
 * ======================================================================= */

/**
 * @brief  初始化 SPI1 -> DMA2 Stream3 发送链路
 * @note   配置 DMA 通道、传输方向、数据宽度、FIFO 模式、
 *         中断优先级等参数，并使能 DMA 传输完成/错误中断。
 */
void MYDMA_Config(void)
{
    /* 使能 DMA2 时钟 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, ENABLE);

    /* DMA 基本配置 */
    DMA_InitTypeDef DMA_InitStructure;
    DMA_DeInit(DMA2_Stream3);

    DMA_InitStructure.DMA_Channel             = DMA_Channel_3;
    DMA_InitStructure.DMA_Memory0BaseAddr     = (u32)lineBuffer[0];
    DMA_InitStructure.DMA_PeripheralBaseAddr  = (u32)&SPI1->DR;
    DMA_InitStructure.DMA_DIR                 = DMA_DIR_MemoryToPeripheral;
    DMA_InitStructure.DMA_BufferSize          = LCD_DMA_THERMAL_STRIPE_BUF_SIZE;
    DMA_InitStructure.DMA_PeripheralInc       = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc           = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize  = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize      = DMA_MemoryDataSize_Byte;
    DMA_InitStructure.DMA_Mode                = DMA_Mode_Normal;
    DMA_InitStructure.DMA_Priority            = DMA_Priority_VeryHigh;
    DMA_InitStructure.DMA_FIFOMode            = DMA_FIFOMode_Enable;
    DMA_InitStructure.DMA_FIFOThreshold       = DMA_FIFOThreshold_HalfFull;
    DMA_InitStructure.DMA_MemoryBurst         = DMA_MemoryBurst_INC4;
    DMA_InitStructure.DMA_PeripheralBurst     = DMA_PeripheralBurst_Single;
    DMA_Init(DMA2_Stream3, &DMA_InitStructure);

    /* 使能 SPI1 TX DMA 请求 */
    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);

    /* NVIC 中断配置 */
    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel                   = DMA2_Stream3_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    /* 使能 DMA 传输完成和传输错误中断 */
    DMA_ITConfig(DMA2_Stream3, DMA_IT_TC | DMA_IT_TE, ENABLE);

    /* 清除所有 DMA 中断标志 */
    DMA_ClearFlag(DMA2_Stream3,
                  DMA_FLAG_FEIF3 | DMA_FLAG_DMEIF3 |
                  DMA_FLAG_TEIF3 | DMA_FLAG_HTIF3 | DMA_FLAG_TCIF3);

    /* 初始化模块状态 */
    transferComplete  = 1;
    s_dma_wait_task   = 0;
    s_dma_last_result = 1U;
    s_dma_last_status = APP_PERF_LCD_DMA_STATUS_NONE;
    activeBuffer      = 0;
    s_dma_mode        = LCD_DMA_MODE_IDLE;

#if LCD_DMA_STAGE6_6B_ACTIVE
    s_renderMappingReady = 0U;
    lcd_dma_init_render_mappings();
#endif
}

/* =========================================================================
 *  23. 热成像显示主入口
 * ======================================================================= */

/**
 * @brief  热成像显示主入口函数
 * @note   输入 24x32 灰度帧，经插值后逐条带 DMA 输出到 LCD。
 *         使用双缓冲机制实现渲染与传输的流水线化：
 *         - 缓冲区 A 发送的同时，缓冲区 B 填充下一帧数据
 *         - 交替切换，最大化 CPU 与 DMA 并行度
 *
 * @param  data24x32 — 输入 24x32 灰度帧数据（768 字节）
 * @retval 1 — 显示成功；0 — 传输失败或参数无效
 */
uint8_t LCD_Disp_Thermal_Interpolated_DMA(uint8_t *data24x32)
{
    uint32_t start_cycle       = app_perf_baseline_cycle_now();
    uint8_t  tx_buffer_index   = 0U;
    uint8_t  fill_buffer_index = 1U;
    uint16_t next_start_row    = 0U;
    uint16_t transfer_row_count = 0U;

    if (data24x32 == 0)
    {
        return 0U;
    }

    /* ---- 1. 初始化帧性能统计与渲染映射 ---- */
    lcd_dma_perf_reset_frame();
    app_perf_baseline_record_lcd_dma_enter();
#if LCD_DMA_STAGE6_6B_ACTIVE
    lcd_dma_init_render_mappings();
#endif

    /* ---- 2. 执行插值管线 ---- */
    build_horizontal_interp_rows(data24x32);
    build_vertical_edge_rows();

    /* ---- 3. 设置 LCD 绘图窗口 ---- */
    LCD_Address_Set((u16)LCD_DMA_THERMAL_AREA_X0,
                    (u16)LCD_DMA_THERMAL_AREA_Y0,
                    (u16)(LCD_DMA_THERMAL_AREA_X0 + LCD_DMA_THERMAL_AREA_WIDTH - 1U),
                    (u16)(LCD_DMA_THERMAL_AREA_Y0 + LCD_DMA_THERMAL_AREA_HEIGHT - 1U));
    LCD_DC_Set();
    LCD_CS_Clr();

    /* ---- 4. 渲染并发送第一个条带 ---- */
    transfer_row_count = lcd_dma_get_stripe_row_count(0U);
    activeBuffer = tx_buffer_index;
    render_output_rows_to_buffer(0U, transfer_row_count, lineBuffer[tx_buffer_index]);

    if (start_dma_line_transfer(lineBuffer[tx_buffer_index], transfer_row_count) == 0U)
    {
        LCD_CS_Set();
        s_dma_mode = LCD_DMA_MODE_IDLE;
        lcd_dma_perf_commit_frame();
        app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                                s_dma_last_status);
        return 0U;
    }

    /* ---- 5. 双缓冲流水线：等待上一条带完成 -> 发送当前条带 -> 填充下一条带 ---- */
    next_start_row = transfer_row_count;
    while (next_start_row < LCD_DMA_THERMAL_AREA_HEIGHT)
    {
        transfer_row_count = lcd_dma_get_stripe_row_count(next_start_row);

        /* 在填充缓冲区中渲染下一条带 */
        render_output_rows_to_buffer(next_start_row,
                                     transfer_row_count,
                                     lineBuffer[fill_buffer_index]);

        /* 等待上一条带 DMA 发送完成 */
        if (wait_for_dma_transfer_complete() == 0U)
        {
            LCD_CS_Set();
            s_dma_mode = LCD_DMA_MODE_IDLE;
            lcd_dma_perf_commit_frame();
            app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                                    s_dma_last_status);
            return 0U;
        }

        /* 切换缓冲区并启动 DMA 发送 */
        tx_buffer_index   = fill_buffer_index;
        fill_buffer_index ^= 1U;
        activeBuffer      = tx_buffer_index;

        if (start_dma_line_transfer(lineBuffer[tx_buffer_index], transfer_row_count) == 0U)
        {
            LCD_CS_Set();
            s_dma_mode = LCD_DMA_MODE_IDLE;
            lcd_dma_perf_commit_frame();
            app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                                    s_dma_last_status);
            return 0U;
        }

        next_start_row = (uint16_t)(next_start_row + transfer_row_count);
    }

    /* ---- 6. 等待最后一个条带发送完成 ---- */
    if (wait_for_dma_transfer_complete() == 0U)
    {
        LCD_CS_Set();
        s_dma_mode = LCD_DMA_MODE_IDLE;
        lcd_dma_perf_commit_frame();
        app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                                s_dma_last_status);
        return 0U;
    }

    /* ---- 7. 帧发送完成，拉高 CS 释放 SPI 总线 ---- */
    LCD_CS_Set();
    s_dma_mode = LCD_DMA_MODE_IDLE;
    lcd_dma_perf_commit_frame();
    app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                            APP_PERF_LCD_DMA_STATUS_OK);
    return 1U;
}
