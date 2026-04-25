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

/* LCD DMA 显示模块
 * 将 24x32 热成像灰度帧插值为 240x320，并通过 DMA 输出到 LCD。 */

#define THERMAL_RENDER_WIDTH  240
#define THERMAL_RENDER_HEIGHT 320
#define THERMAL_OUTPUT_ROWS   (LCD_H - 20U)
#define THERMAL_SRC_WIDTH 24
#define THERMAL_SRC_HEIGHT 32
#define INTERP_STRIDE 10
#define TOP_EDGE_ROWS 5
#define BOTTOM_EDGE_ROWS 4
#define BOTTOM_EDGE_START (THERMAL_RENDER_HEIGHT - BOTTOM_EDGE_ROWS)
#define LINE_BUF_SIZE  (LCD_W * 2)
#define LCD_DMA_THERMAL_CROSS_HALF_SIZE 6U
#define LCD_DMA_THERMAL_CROSS_GAP_SIZE  2U
#define DMA_TRANSFER_WAIT_LOOPS 5000000UL
#define DMA_TRANSFER_WAIT_TIMEOUT_MS 20UL
#define DMA_STREAM_DISABLE_WAIT_LOOPS 100000UL
#define LCD_SPI_IDLE_WAIT_LOOPS 100000UL

/* 当前工程固定保留 Stage6_6B / 6C 已启用实现。
 * 这里不再依赖 redpic1_thermal 的历史阶段宏，避免模块间继续耦合旧回滚开关。 */
#define LCD_DMA_STAGE6_6B_ACTIVE 1
#define LCD_DMA_STAGE6_6C_ACTIVE 1

#if (REDPIC1_THERMAL_STAGE6R_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_1_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_2_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_3_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_D1_ENABLE != 0U)
    #define LCD_DMA_STAGE6R_D1_ACTIVE 1U
#else
    #define LCD_DMA_STAGE6R_D1_ACTIVE 0U
#endif

#if (LCD_DMA_STAGE6R_D1_ACTIVE != 0U)
    #define LCD_DMA_THERMAL_STRIPE_ROWS REDPIC1_THERMAL_STAGE6R_D1_STRIPE_ROWS
#else
    #define LCD_DMA_THERMAL_STRIPE_ROWS 1U
#endif

#if (REDPIC1_THERMAL_LOW_COST_RENDER_TEST_ENABLE != 0U)
    #define LCD_DMA_LOW_COST_RENDER_TEST_ACTIVE 1U
#else
    #define LCD_DMA_LOW_COST_RENDER_TEST_ACTIVE 0U
#endif

#define LCD_DMA_THERMAL_STRIPE_BUF_SIZE (LINE_BUF_SIZE * LCD_DMA_THERMAL_STRIPE_ROWS)

/* 双行缓存必须留在系统 SRAM，避免被链接到 DMA 不可达的 CCRAM。 */
__attribute__((section("dma_sram"), aligned(4))) u8 lineBuffer[2][LCD_DMA_THERMAL_STRIPE_BUF_SIZE];

volatile uint8_t activeBuffer = 0;
volatile uint8_t transferComplete = 1;
static volatile TaskHandle_t s_dma_wait_task = 0;
static volatile uint8_t s_dma_last_result = 1U;
static volatile app_perf_lcd_dma_status_t s_dma_last_status = APP_PERF_LCD_DMA_STATUS_NONE;
typedef enum {
    LCD_DMA_MODE_IDLE = 0,
    LCD_DMA_MODE_THERMAL = 1
} lcd_dma_mode_t;

static volatile lcd_dma_mode_t s_dma_mode = LCD_DMA_MODE_IDLE;
static CCMRAM uint8_t g_interpRows[THERMAL_SRC_HEIGHT][THERMAL_RENDER_WIDTH];
static CCMRAM uint8_t g_topEdgeRows[TOP_EDGE_ROWS][THERMAL_RENDER_WIDTH];
static CCMRAM uint8_t g_bottomEdgeRows[BOTTOM_EDGE_ROWS][THERMAL_RENDER_WIDTH];
/* 伪彩色 LUT。 */
CCMRAM uint16_t GCM_Pseudo3[256];

#if LCD_DMA_STAGE6_6C_ACTIVE
static CCMRAM uint8_t g_colorHighByteLut[256];
static CCMRAM uint8_t g_colorLowByteLut[256];
#endif

#if LCD_DMA_STAGE6_6B_ACTIVE
typedef enum {
    LCD_DMA_INTERP_ROW_TOP = 0,
    LCD_DMA_INTERP_ROW_BODY = 1,
    LCD_DMA_INTERP_ROW_BOTTOM = 2
} lcd_dma_interp_row_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t base_index;
    uint8_t ratio;
} lcd_dma_interp_row_meta_t;

static CCMRAM lcd_dma_interp_row_meta_t g_interpRowMeta[THERMAL_RENDER_HEIGHT];
static CCMRAM uint16_t g_outputRowToInterpRow[LCD_H];
static CCMRAM uint16_t g_outputColToInterpRow[LCD_W];
static CCMRAM uint16_t g_outputRowToInterpCol[LCD_H];
static CCMRAM uint16_t g_outputColToInterpCol[LCD_W];
static uint8_t s_renderMappingReady = 0U;
#endif

typedef struct {
    uint32_t render_us;
    uint32_t dma_start_us;
    uint32_t dma_wait_us;
    uint32_t spi_idle_wait_us;
    uint32_t overlay_us;
} lcd_dma_frame_perf_t;

static lcd_dma_frame_perf_t s_lcd_dma_frame_perf;
static const uint8_t *s_lcd_dma_source_frame = 0;
static void lcd_dma_perf_reset_frame(void);
static void lcd_dma_perf_add_elapsed(uint32_t *accum, uint32_t start_cycle);
static void lcd_dma_perf_commit_frame(void);

/* 将任意整数裁剪到 0~255。 */
static uint8_t clamp_to_u8(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static void lcd_dma_write_rgb565_pixel(uint8_t *buf, uint16_t out_x, uint16_t color)
{
    if (buf == 0 || out_x >= LCD_W)
    {
        return;
    }

    buf[2U * out_x] = (uint8_t)(color >> 8);
    buf[2U * out_x + 1U] = (uint8_t)(color & 0xFFU);
}

static void lcd_dma_overlay_crosshair_row(uint16_t out_row, uint8_t *buf)
{
    uint16_t center_x = (uint16_t)(LCD_W / 2U);
    uint16_t center_y = (uint16_t)(THERMAL_OUTPUT_ROWS / 2U);
    uint16_t left_start = 0U;
    uint16_t left_end = 0U;
    uint16_t right_start = 0U;
    uint16_t right_end = 0U;
    uint16_t top_start = 0U;
    uint16_t top_end = 0U;
    uint16_t bottom_start = (uint16_t)(center_y + LCD_DMA_THERMAL_CROSS_GAP_SIZE);
    uint16_t bottom_end = (uint16_t)(center_y + LCD_DMA_THERMAL_CROSS_HALF_SIZE);

    if (buf == 0 || out_row >= THERMAL_OUTPUT_ROWS ||
        redpic1_thermal_runtime_overlay_visible() == 0U)
    {
        return;
    }

    left_start = (center_x > LCD_DMA_THERMAL_CROSS_HALF_SIZE) ?
                 (uint16_t)(center_x - LCD_DMA_THERMAL_CROSS_HALF_SIZE) :
                 0U;
    left_end = (center_x > LCD_DMA_THERMAL_CROSS_GAP_SIZE) ?
               (uint16_t)(center_x - LCD_DMA_THERMAL_CROSS_GAP_SIZE) :
               0U;
    right_start = (uint16_t)(center_x + LCD_DMA_THERMAL_CROSS_GAP_SIZE);
    right_end = (uint16_t)(center_x + LCD_DMA_THERMAL_CROSS_HALF_SIZE);
    top_start = (center_y > LCD_DMA_THERMAL_CROSS_HALF_SIZE) ?
                (uint16_t)(center_y - LCD_DMA_THERMAL_CROSS_HALF_SIZE) :
                0U;
    top_end = (center_y > LCD_DMA_THERMAL_CROSS_GAP_SIZE) ?
              (uint16_t)(center_y - LCD_DMA_THERMAL_CROSS_GAP_SIZE) :
              0U;

    if (out_row == center_y)
    {
        uint32_t overlay_start_cycle = app_perf_baseline_cycle_now();
        uint16_t out_x = 0U;

        for (out_x = left_start; out_x <= left_end && out_x < LCD_W; ++out_x)
        {
            lcd_dma_write_rgb565_pixel(buf, out_x, WHITE);
        }
        for (out_x = right_start; out_x <= right_end && out_x < LCD_W; ++out_x)
        {
            lcd_dma_write_rgb565_pixel(buf, out_x, WHITE);
        }
        lcd_dma_write_rgb565_pixel(buf, center_x, RED);
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.overlay_us, overlay_start_cycle);
        return;
    }

    if ((out_row >= top_start && out_row <= top_end) ||
        (out_row >= bottom_start && out_row <= bottom_end))
    {
        uint32_t overlay_start_cycle = app_perf_baseline_cycle_now();
        lcd_dma_write_rgb565_pixel(buf, center_x, WHITE);
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.overlay_us, overlay_start_cycle);
    }
}

static uint16_t lcd_dma_scale_axis(uint16_t index, uint16_t output_count, uint16_t interp_count)
{
    uint32_t numerator = 0U;
    uint32_t denominator = 0U;

    if (output_count <= 1U || interp_count <= 1U)
    {
        return 0U;
    }

    denominator = (uint32_t)(output_count - 1U);
    numerator = ((uint32_t)index * (uint32_t)(interp_count - 1U)) + (denominator / 2U);
    return (uint16_t)(numerator / denominator);
}

/* 启动一次 DMA 行发送。 */
static void lcd_dma_perf_reset_frame(void)
{
    memset(&s_lcd_dma_frame_perf, 0, sizeof(s_lcd_dma_frame_perf));
}

static void lcd_dma_perf_add_elapsed(uint32_t *accum, uint32_t start_cycle)
{
    if (accum == 0)
    {
        return;
    }

    *accum += app_perf_baseline_elapsed_us(start_cycle);
}

static void lcd_dma_perf_commit_frame(void)
{
    app_perf_baseline_record_lcd_dma_render_us(s_lcd_dma_frame_perf.render_us);
    app_perf_baseline_record_lcd_dma_start_us(s_lcd_dma_frame_perf.dma_start_us);
    app_perf_baseline_record_lcd_dma_wait_us(s_lcd_dma_frame_perf.dma_wait_us);
    app_perf_baseline_record_lcd_dma_spi_idle_us(s_lcd_dma_frame_perf.spi_idle_wait_us);
    app_perf_baseline_record_lcd_dma_overlay_us(s_lcd_dma_frame_perf.overlay_us);
}

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
            s_dma_mode = LCD_DMA_MODE_IDLE;
            transferComplete = 1U;
            s_dma_last_result = 0U;
            s_dma_last_status = APP_PERF_LCD_DMA_STATUS_ERROR;
            return 0U;
        }
    }

    return 1U;
}

static uint8_t lcd_dma_wait_spi_idle(void)
{
    uint32_t wait_start_cycle = app_perf_baseline_cycle_now();
    uint32_t timeout = LCD_SPI_IDLE_WAIT_LOOPS;

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
    {
        if (timeout-- == 0U)
        {
            s_dma_mode = LCD_DMA_MODE_IDLE;
            s_dma_last_result = 0U;
            s_dma_last_status = APP_PERF_LCD_DMA_STATUS_ERROR;
            lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.spi_idle_wait_us, wait_start_cycle);
            return 0U;
        }
    }

    timeout = LCD_SPI_IDLE_WAIT_LOOPS;
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)
    {
        if (timeout-- == 0U)
        {
            s_dma_mode = LCD_DMA_MODE_IDLE;
            s_dma_last_result = 0U;
            s_dma_last_status = APP_PERF_LCD_DMA_STATUS_ERROR;
            lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.spi_idle_wait_us, wait_start_cycle);
            return 0U;
        }
    }

    lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.spi_idle_wait_us, wait_start_cycle);
    return 1U;
}

static uint8_t start_dma_line_transfer(uint8_t *buf, uint16_t row_count)
{
    uint32_t start_cycle = app_perf_baseline_cycle_now();
    uint16_t transfer_size = 0U;

    if (buf == 0 || row_count == 0U || lcd_dma_wait_stream_disabled() == 0U)
    {
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_start_us, start_cycle);
        return 0U;
    }

    transfer_size = (uint16_t)(LINE_BUF_SIZE * row_count);

    s_dma_mode = LCD_DMA_MODE_THERMAL;
    transferComplete = 0U;
    s_dma_last_result = 0U;
    s_dma_last_status = APP_PERF_LCD_DMA_STATUS_TIMEOUT;
    DMA_ClearFlag(DMA2_Stream3,
                  DMA_FLAG_FEIF3 |
                  DMA_FLAG_DMEIF3 |
                  DMA_FLAG_TEIF3 |
                  DMA_FLAG_HTIF3 |
                  DMA_FLAG_TCIF3);
    DMA2_Stream3->M0AR = (uint32_t)buf;
    DMA_SetCurrDataCounter(DMA2_Stream3, transfer_size);
    DMA_Cmd(DMA2_Stream3, ENABLE);
    lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_start_us, start_cycle);
    return 1U;
}

static uint8_t lcd_dma_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

static uint8_t lcd_dma_wait_busy_loop(void)
{
    uint32_t wait_start_cycle = app_perf_baseline_cycle_now();
    uint32_t timeout = DMA_TRANSFER_WAIT_LOOPS;

    while (!transferComplete)
    {
        if (timeout-- == 0U)
        {
            DMA_Cmd(DMA2_Stream3, DISABLE);
            DMA_ClearFlag(DMA2_Stream3,
                          DMA_FLAG_FEIF3 |
                          DMA_FLAG_DMEIF3 |
                          DMA_FLAG_TEIF3 |
                          DMA_FLAG_HTIF3 |
                          DMA_FLAG_TCIF3);
            s_dma_mode = LCD_DMA_MODE_IDLE;
            transferComplete = 1U;
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

/* 等待 DMA 发送完成。
 * 超时后主动终止，避免主循环长期卡死。 */
static uint8_t wait_for_dma_transfer_complete(void)
{
#if APP_DISPLAY_STAGE3_ENABLE
    uint32_t wait_start_cycle = app_perf_baseline_cycle_now();
    TickType_t wait_ticks = pdMS_TO_TICKS(DMA_TRANSFER_WAIT_TIMEOUT_MS);
    TaskHandle_t current_task = 0;

    if (lcd_dma_scheduler_running() == 0U)
    {
        return lcd_dma_wait_busy_loop();
    }

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

    current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0U);

    taskENTER_CRITICAL();
    if (transferComplete == 0U)
    {
        s_dma_wait_task = current_task;
    }
    taskEXIT_CRITICAL();

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

    if (ulTaskNotifyTake(pdTRUE, wait_ticks) == 0U)
    {
        taskENTER_CRITICAL();
        if (s_dma_wait_task == current_task)
        {
            s_dma_wait_task = 0;
        }
        taskEXIT_CRITICAL();

        DMA_Cmd(DMA2_Stream3, DISABLE);
        DMA_ClearFlag(DMA2_Stream3,
                      DMA_FLAG_FEIF3 |
                      DMA_FLAG_DMEIF3 |
                      DMA_FLAG_TEIF3 |
                      DMA_FLAG_HTIF3 |
                      DMA_FLAG_TCIF3);
        s_dma_mode = LCD_DMA_MODE_IDLE;
        transferComplete = 1U;
        s_dma_last_result = 0U;
        s_dma_last_status = APP_PERF_LCD_DMA_STATUS_TIMEOUT;
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
        return 0U;
    }

    app_perf_baseline_record_dma_wait_take();
    if (s_dma_last_result != 0U)
    {
        lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
        return lcd_dma_wait_spi_idle();
    }

    lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.dma_wait_us, wait_start_cycle);
    return 0U;
#else
    return lcd_dma_wait_busy_loop();
#endif
}

#if LCD_DMA_STAGE6_6B_ACTIVE
static void lcd_dma_init_render_mappings(void)
{
    uint16_t out_row = 0U;
    uint16_t out_col = 0U;

    if (s_renderMappingReady != 0U)
    {
        return;
    }

    for (uint16_t interp_row = 0U; interp_row < THERMAL_RENDER_HEIGHT; ++interp_row)
    {
        lcd_dma_interp_row_meta_t *meta = &g_interpRowMeta[interp_row];

        if (interp_row < TOP_EDGE_ROWS)
        {
            meta->kind = LCD_DMA_INTERP_ROW_TOP;
            meta->base_index = (uint8_t)interp_row;
            meta->ratio = 0U;
        }
        else if (interp_row >= BOTTOM_EDGE_START)
        {
            meta->kind = LCD_DMA_INTERP_ROW_BOTTOM;
            meta->base_index = (uint8_t)(interp_row - BOTTOM_EDGE_START);
            meta->ratio = 0U;
        }
        else
        {
            uint16_t offset = (uint16_t)(interp_row - TOP_EDGE_ROWS);
            meta->kind = LCD_DMA_INTERP_ROW_BODY;
            meta->base_index = (uint8_t)(offset / INTERP_STRIDE);
            meta->ratio = (uint8_t)(offset % INTERP_STRIDE);
        }
    }

#if USE_HORIZONTAL==0 || USE_HORIZONTAL==1
    for (out_row = 0U; out_row < LCD_H; ++out_row)
    {
        if (out_row < THERMAL_OUTPUT_ROWS)
        {
            uint16_t mapped_row = lcd_dma_scale_axis(out_row,
                                                     THERMAL_OUTPUT_ROWS,
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
        g_outputColToInterpCol[out_col] = (uint16_t)(THERMAL_RENDER_WIDTH - 1U - out_col);
    }
#elif USE_HORIZONTAL==2
    for (out_row = 0U; out_row < LCD_H; ++out_row)
    {
        if (out_row < THERMAL_OUTPUT_ROWS)
        {
            uint16_t mapped_col = lcd_dma_scale_axis(out_row,
                                                     THERMAL_OUTPUT_ROWS,
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
        g_outputColToInterpRow[out_col] = out_col;
    }

#endif

    s_renderMappingReady = 1U;
}


// 原理： X / 10 近似等于 (X * 205) / 2048
#define FAST_DIV_10(x) (((x) * 205U) >> 11U)

/* * 优化点1：必须添加 static inline 强制内联，彻底消除 76800 次函数调用的入栈/出栈开销
 */
static inline uint8_t lcd_dma_sample_interp_row(const lcd_dma_interp_row_meta_t *meta, uint16_t col)
{
    /* * 优化点2：去掉 if(meta == 0) 的防呆检查。
     * 这种底层热路径函数不该承担参数校验的责任，外层逻辑保证不传 NULL 即可。
     */

    // 优化点3：提取到局部变量，避免 CPU 反复进行结构体指针偏移读取
    uint8_t kind = meta->kind;
    uint16_t base = meta->base_index;

    // 优化点4：使用 if - else if 结构，明确互斥关系，极大提升 CPU 的分支预测成功率
    if (kind == LCD_DMA_INTERP_ROW_TOP)
    {
        return g_topEdgeRows[base][col];
    }
    else if (kind == LCD_DMA_INTERP_ROW_BOTTOM)
    {
        return g_bottomEdgeRows[base][col];
    }
    else 
    {
        // 进入正常插值路径
        uint8_t ratio = meta->ratio;
        uint8_t p1 = g_interpRows[base][col];
        
        if (ratio == 0U)
        {
            return p1;
        }
        
        uint8_t p2 = g_interpRows[base + 1U][col];
        
        // 提取公共计算部分
        uint32_t val = (uint32_t)(p1 * (INTERP_STRIDE - ratio) + p2 * ratio);
        
        /* * 优化点5：消灭除法器！
         * Cortex-M4 的除法指令非常慢（需要十几个周期），而乘法+移位只需 2 个周期。
         */
#if INTERP_STRIDE == 10
        return (uint8_t)FAST_DIV_10(val);
#else
        return (uint8_t)(val / INTERP_STRIDE);
#endif
    }
}
#endif

/* 水平方向插值：将每行 24 列扩展为 240 列。 */
/* 极限优化版本的水平方向插值 */
static void build_horizontal_interp_rows(const uint8_t *frameData)
{
    for (int row = 0; row < THERMAL_SRC_HEIGHT; row++) {
        uint8_t *dst = g_interpRows[row];
        const uint8_t *src = &frameData[row * THERMAL_SRC_WIDTH];

        // 优化点2：提前将基准指针偏移好，避免每次循环计算 base
        uint8_t *dst_ptr = dst + TOP_EDGE_ROWS;

        for (int i = 0; i < (THERMAL_SRC_WIDTH - 1); i++) {
            // 优化点3：使用 uint32_t 提取，防止编译器做多余的 8位零扩展 (UXTB)
            uint32_t p0 = src[i];
            uint32_t p1 = src[i + 1];

            // 优化点1 & 2：
            // *dst_ptr++ 触发 ARM 汇编后索引寻址 (STRB Rx, [Ry], #1)，省去大量的地址加法
            // (X * 205) >> 11 完美等效于 X / 10，彻底消灭硬件除法！
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

        *dst_ptr = src[THERMAL_SRC_WIDTH - 1];

        // 优化点4：边缘外推使用单周期硬件饱和指令 __USAT 替代带有分支的 clamp_to_u8
        for (int i = TOP_EDGE_ROWS - 1; i >= 0; i--) {
            dst[i] = (uint8_t)__USAT((2 * (int32_t)dst[i + 1] - (int32_t)dst[i + 2]), 8);
        }

        for (int i = THERMAL_RENDER_WIDTH - TOP_EDGE_ROWS + 1; i < THERMAL_RENDER_WIDTH; i++) {
            dst[i] = (uint8_t)__USAT((2 * (int32_t)dst[i - 1] - (int32_t)dst[i - 2]), 8);
        }
    }
}


/* 计算顶部与底部边缘外推行。 */
static void build_vertical_edge_rows(void)
{
    for (int x = 0; x < THERMAL_RENDER_WIDTH; x++) {
        uint8_t row5 = g_interpRows[0][x];
        uint8_t row6 = (uint8_t)((g_interpRows[0][x] * 9 + g_interpRows[1][x]) / INTERP_STRIDE);
        g_topEdgeRows[TOP_EDGE_ROWS - 1][x] = clamp_to_u8(2 * row5 - row6);
    }

    for (int row = TOP_EDGE_ROWS - 2; row >= 0; row--) {
        const uint8_t *next1 = g_topEdgeRows[row + 1];
        const uint8_t *next2 = (row == TOP_EDGE_ROWS - 2) ? g_interpRows[0] : g_topEdgeRows[row + 2];

        for (int x = 0; x < THERMAL_RENDER_WIDTH; x++) {
            g_topEdgeRows[row][x] = clamp_to_u8(2 * next1[x] - next2[x]);
        }
    }

    for (int x = 0; x < THERMAL_RENDER_WIDTH; x++) {
        uint8_t row314 = (uint8_t)((g_interpRows[30][x] + g_interpRows[31][x] * 9) / INTERP_STRIDE);
        uint8_t row315 = g_interpRows[31][x];
        g_bottomEdgeRows[0][x] = clamp_to_u8(2 * row315 - row314);
    }

    for (int row = 1; row < BOTTOM_EDGE_ROWS; row++) {
        const uint8_t *prev1 = g_bottomEdgeRows[row - 1];
        const uint8_t *prev2 = (row == 1) ? g_interpRows[31] : g_bottomEdgeRows[row - 2];

        for (int x = 0; x < THERMAL_RENDER_WIDTH; x++) {
            g_bottomEdgeRows[row][x] = clamp_to_u8(2 * prev1[x] - prev2[x]);
        }
    }
}

#if LCD_DMA_LOW_COST_RENDER_TEST_ACTIVE
static uint16_t lcd_dma_interp_coord_to_src_index(uint16_t interp_coord,
                                                  uint16_t first_anchor,
                                                  uint16_t max_src_index)
{
    uint16_t last_anchor = (uint16_t)(first_anchor + (max_src_index * INTERP_STRIDE));

    if (interp_coord <= first_anchor)
    {
        return 0U;
    }

    if (interp_coord >= last_anchor)
    {
        return max_src_index;
    }

    return (uint16_t)(((interp_coord - first_anchor) + (INTERP_STRIDE / 2U)) / INTERP_STRIDE);
}

static uint8_t lcd_dma_sample_low_cost_gray(uint16_t out_x, uint16_t out_row)
{
    uint16_t interp_row = 0U;
    uint16_t interp_col = 0U;
    uint16_t src_row = 0U;
    uint16_t src_col = 0U;

    if (s_lcd_dma_source_frame == 0 ||
        out_x >= LCD_W ||
        out_row >= THERMAL_OUTPUT_ROWS)
    {
        return 0U;
    }

#if LCD_DMA_STAGE6_6B_ACTIVE && (USE_HORIZONTAL==0 || USE_HORIZONTAL==1)
    interp_row = g_outputRowToInterpRow[out_row];
    interp_col = g_outputColToInterpCol[out_x];
#elif LCD_DMA_STAGE6_6B_ACTIVE
    interp_row = g_outputColToInterpRow[out_x];
    interp_col = g_outputRowToInterpCol[out_row];
#else
    interp_row = lcd_dma_scale_axis(out_row, THERMAL_OUTPUT_ROWS, THERMAL_RENDER_HEIGHT);
    interp_col = lcd_dma_scale_axis(out_x, LCD_W, THERMAL_RENDER_WIDTH);
#endif

    src_row = lcd_dma_interp_coord_to_src_index(interp_row,
                                                TOP_EDGE_ROWS,
                                                (uint16_t)(THERMAL_SRC_HEIGHT - 1U));
    src_col = lcd_dma_interp_coord_to_src_index(interp_col,
                                                TOP_EDGE_ROWS,
                                                (uint16_t)(THERMAL_SRC_WIDTH - 1U));

    return s_lcd_dma_source_frame[((uint32_t)src_row * THERMAL_SRC_WIDTH) + src_col];
}
#endif

/* 生成指定输出行的 RGB565 数据。 */
static void render_output_row_to_buffer(uint16_t outRow, uint8_t *buf)
{
    if (buf == 0)
    {
        return;
    }

    if (outRow >= THERMAL_OUTPUT_ROWS)
    {
        /* 保留 32位指针 极速清屏优化 */
        uint32_t *buf32 = (uint32_t *)buf;
        for (int i = 0; i < (LINE_BUF_SIZE / 4); i++) {
            buf32[i] = 0UL;
        }
        return;
    }

    /* 保留 16位指针写入，榨干总线性能 */
    uint16_t *buf16 = (uint16_t *)buf;

#if LCD_DMA_LOW_COST_RENDER_TEST_ACTIVE
    for (uint16_t outX = 0U; outX < LCD_W; outX++) {
        uint8_t pixel = lcd_dma_sample_low_cost_gray(outX, outRow);
#if LCD_DMA_STAGE6_6C_ACTIVE
        buf16[outX] = (uint16_t)(g_colorHighByteLut[pixel] | (g_colorLowByteLut[pixel] << 8));
#else
        uint16_t color = GCM_Pseudo3[pixel];
        buf16[outX] = (uint16_t)((color >> 8) | (color << 8));
#endif
    }
#elif LCD_DMA_STAGE6_6B_ACTIVE && (USE_HORIZONTAL==0 || USE_HORIZONTAL==1)
    const lcd_dma_interp_row_meta_t *row_meta = &g_interpRowMeta[g_outputRowToInterpRow[outRow]];

    for (uint16_t outX = 0U; outX < LCD_W; outX++) {
        uint8_t pixel = lcd_dma_sample_interp_row(row_meta, g_outputColToInterpCol[outX]);
#if LCD_DMA_STAGE6_6C_ACTIVE
        /* 修复：将高字节排在低地址位，生成会被翻译为 REV16 的交换逻辑 */
        buf16[outX] = (uint16_t)(g_colorHighByteLut[pixel] | (g_colorLowByteLut[pixel] << 8));
#else
        uint16_t color = GCM_Pseudo3[pixel];
        /* 修复：大小端硬件翻转 (将高低8位对调，匹配 LCD 的大端接收时序) */
        buf16[outX] = (uint16_t)((color >> 8) | (color << 8));
#endif
    }
#elif LCD_DMA_STAGE6_6B_ACTIVE
    uint16_t interp_col = g_outputRowToInterpCol[outRow];

    for (uint16_t outX = 0U; outX < LCD_W; outX++) {
        const lcd_dma_interp_row_meta_t *row_meta = &g_interpRowMeta[g_outputColToInterpRow[outX]];
        uint8_t pixel = lcd_dma_sample_interp_row(row_meta, interp_col);
#if LCD_DMA_STAGE6_6C_ACTIVE
        buf16[outX] = (uint16_t)(g_colorHighByteLut[pixel] | (g_colorLowByteLut[pixel] << 8));
#else
        uint16_t color = GCM_Pseudo3[pixel];
        buf16[outX] = (uint16_t)((color >> 8) | (color << 8));
#endif
    }
#elif LCD_DMA_STAGE6_6C_ACTIVE
    for (uint16_t outX = 0U; outX < LCD_W; outX++) {
        uint16_t portraitX = 0U;
        uint16_t portraitY = 0U;
        map_output_to_legacy_portrait(outX, outRow, &portraitX, &portraitY);

        uint8_t pixel = sample_legacy_portrait_gray(portraitX, portraitY);
        buf16[outX] = (uint16_t)(g_colorHighByteLut[pixel] | (g_colorLowByteLut[pixel] << 8));
    }
#else
    for (uint16_t outX = 0U; outX < LCD_W; outX++) {
        uint16_t portraitX = 0U;
        uint16_t portraitY = 0U;
        map_output_to_legacy_portrait(outX, outRow, &portraitX, &portraitY);

        uint8_t pixel = sample_legacy_portrait_gray(portraitX, portraitY);
        uint16_t color = GCM_Pseudo3[pixel];
        buf16[outX] = (uint16_t)((color >> 8) | (color << 8));
    }
#endif

    lcd_dma_overlay_crosshair_row(outRow, buf);
}



/* 初始化 SPI1 -> DMA2_Stream3 发送链路。 */
static uint16_t lcd_dma_get_stripe_row_count(uint16_t start_row)
{
    uint16_t remaining_rows = 0U;

    if (start_row >= THERMAL_OUTPUT_ROWS)
    {
        return 0U;
    }

    remaining_rows = (uint16_t)(THERMAL_OUTPUT_ROWS - start_row);
    if (remaining_rows > LCD_DMA_THERMAL_STRIPE_ROWS)
    {
        remaining_rows = LCD_DMA_THERMAL_STRIPE_ROWS;
    }

    return remaining_rows;
}

static void render_output_rows_to_buffer(uint16_t start_row, uint16_t row_count, uint8_t *buf)
{
    uint32_t render_start_cycle = app_perf_baseline_cycle_now();
    uint16_t stripe_row = 0U;

    if (buf == 0)
    {
        return;
    }

    for (stripe_row = 0U; stripe_row < row_count; ++stripe_row)
    {
        render_output_row_to_buffer((uint16_t)(start_row + stripe_row),
                                    &buf[(uint32_t)stripe_row * LINE_BUF_SIZE]);
    }

    lcd_dma_perf_add_elapsed(&s_lcd_dma_frame_perf.render_us, render_start_cycle);
}

void MYDMA_Config(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, ENABLE);
    
    DMA_InitTypeDef DMA_InitStructure;
    DMA_DeInit(DMA2_Stream3);
    
    DMA_InitStructure.DMA_Channel = DMA_Channel_3;
    DMA_InitStructure.DMA_Memory0BaseAddr = (u32)lineBuffer[0];
    DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)&SPI1->DR;
    DMA_InitStructure.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    DMA_InitStructure.DMA_BufferSize = LCD_DMA_THERMAL_STRIPE_BUF_SIZE;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Normal; // 单次传输模式
    DMA_InitStructure.DMA_Priority = DMA_Priority_VeryHigh;
    DMA_InitStructure.DMA_FIFOMode = DMA_FIFOMode_Enable;
    DMA_InitStructure.DMA_FIFOThreshold = DMA_FIFOThreshold_HalfFull;
    DMA_InitStructure.DMA_MemoryBurst = DMA_MemoryBurst_INC4;
    DMA_InitStructure.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    DMA_Init(DMA2_Stream3, &DMA_InitStructure);
    
    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);

    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = DMA2_Stream3_IRQn;
#if APP_DISPLAY_STAGE3_ENABLE
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;
#else
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
#endif
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    
    DMA_ITConfig(DMA2_Stream3, DMA_IT_TC | DMA_IT_TE, ENABLE);

    DMA_ClearFlag(DMA2_Stream3,
                  DMA_FLAG_FEIF3 |
                  DMA_FLAG_DMEIF3 |
                  DMA_FLAG_TEIF3 |
                  DMA_FLAG_HTIF3 |
                  DMA_FLAG_TCIF3);

    transferComplete = 1;
    s_dma_wait_task = 0;
    s_dma_last_result = 1U;
    s_dma_last_status = APP_PERF_LCD_DMA_STATUS_NONE;
    activeBuffer = 0;
    s_dma_mode = LCD_DMA_MODE_IDLE;
#if LCD_DMA_STAGE6_6B_ACTIVE
    s_renderMappingReady = 0U;
    lcd_dma_init_render_mappings();
#endif
}
/* DMA2 Stream3 中断处理：
 * 处理热成像逐行发送。 */
void DMA2_Stream3_IRQHandler(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    TaskHandle_t waiting_task = 0;

    if (DMA_GetITStatus(DMA2_Stream3, DMA_IT_TCIF3) != RESET)
    {
        app_perf_baseline_record_dma_irq_tc();
        DMA_ClearITPendingBit(DMA2_Stream3, DMA_IT_TCIF3);
        transferComplete = 1U;
        s_dma_last_result = 1U;
        s_dma_last_status = APP_PERF_LCD_DMA_STATUS_OK;
        waiting_task = (TaskHandle_t)s_dma_wait_task;
        s_dma_wait_task = 0;
    }

    if (DMA_GetITStatus(DMA2_Stream3, DMA_IT_TEIF3) != RESET)
    {
        app_perf_baseline_record_dma_irq_te();
        DMA_ClearFlag(DMA2_Stream3,
                      DMA_FLAG_FEIF3 |
                      DMA_FLAG_DMEIF3 |
                      DMA_FLAG_TEIF3 |
                      DMA_FLAG_HTIF3 |
                      DMA_FLAG_TCIF3);
        DMA_Cmd(DMA2_Stream3, DISABLE);
        transferComplete = 1U;
        s_dma_last_result = 0U;
        s_dma_last_status = APP_PERF_LCD_DMA_STATUS_ERROR;
        s_dma_mode = LCD_DMA_MODE_IDLE;
        LCD_CS_Set();
        waiting_task = (TaskHandle_t)s_dma_wait_task;
        s_dma_wait_task = 0;
    }

#if APP_DISPLAY_STAGE3_ENABLE
    if (waiting_task != 0)
    {
        vTaskNotifyGiveFromISR(waiting_task, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
#endif
}

/* 8 位 RGB 合成为 RGB565。 */
uint16_t rgb_565(uint16_t COLOR_R,uint16_t COLOR_G,uint16_t COLOR_B){
	uint16_t RGB565=0;
	RGB565=((COLOR_R&0XF8)<<8)+((COLOR_G&0XFC)<<3)+((COLOR_B&0XF8)>>3);
	return RGB565;
}


/* 将灰度值按指定模式映射为伪彩色 RGB565。 */
uint16_t color_code(uint16_t grayValue,uint16_t mode){
	uint16_t colorR,colorG,colorB;
    colorR=0;
    colorG=0;
    colorB=0;
    if (mode==0){
        colorR=abs(0-grayValue);
        colorG=abs(127-grayValue);
        colorB=abs(255-grayValue);
		}
    else if (mode==1){
        if ((grayValue>0) && (grayValue<=63)){
            colorR=0;
            colorG=0;
            colorB=round(grayValue/64.0*255.0);
			}
        else if ((grayValue>=64) && (grayValue<=127)){
            colorR=0;
            colorG=round((grayValue-64)/64.0*255.0);
            colorB=round((127-grayValue)/64.0*255.0);
			}
        else if ((grayValue>=128) && (grayValue<=191)){
            colorR=round((grayValue-128)/64.0*255.0);
            colorG=255;
            colorB=0;
			}
        else if ((grayValue>=192) && (grayValue<=255)){
            colorR=255;
            colorG=round((255-grayValue)/64.0*255.0);
            colorB=0;
			}
		}
    else if (mode==2){ 
        if ((grayValue>0) && (grayValue<=63)){
            colorR=0; 
            colorG=0; 
            colorB=round(grayValue/64.0*255.0); 
			}
        else if ((grayValue>=64) && (grayValue<=95)){
        
            colorR=round((grayValue-63)/32.0*127.0); 
            colorG=round((grayValue-63)/32.0*127.0); 
            colorB=255; 
			}
        else if ((grayValue>=96) && (grayValue<=127)){
        
            colorR=round((grayValue-95)/32.0*127.0)+128; 
            colorG=round((grayValue-95)/32.0*127.0)+128; 
            colorB=round((127-grayValue)/32.0*255.0); 
			}
        else if ((grayValue>=128) && (grayValue<=191)){
            colorR=255; 
            colorG=255; 
            colorB=0;
			}
        else if ((grayValue>=192) && (grayValue<=255)){
        
            colorR=255; 
            colorG=255; 
            colorB=round((grayValue-192)/64*255.0);
			}
		}
    else if (mode==3){  
        colorR=0; 
        colorG=0;
        colorB=0;
        if ((grayValue>0) && (grayValue<=16)){
            colorR=0;} 
        else if ((grayValue>=17) && (grayValue<=140)){ 
            colorR=round((grayValue-16)/124.0*255.0);
			}
        else if ((grayValue>=141) && (grayValue<=255)){  
            colorR=255; 
			}
		
        if ((grayValue>0) && (grayValue<=101)){
            colorG=0;
			}
        else if ((grayValue>=102) && (grayValue<=218)){
            colorG=round((grayValue-101)/117.0*255.0);
			}
        else if ((grayValue>=219) && (grayValue<=255)){  
            colorG=255; 
			}
        if ((grayValue>0) && (grayValue<=91)){
            colorB=28+round((grayValue-0)/91.0*100.0);
			}
        else if ((grayValue>=92) && (grayValue<=120)){
            colorB=round((120-grayValue)/29.0*128.0);
			}
        else if ((grayValue>=129) && (grayValue<=214)){
            colorB=0;
			}			
        else if ((grayValue>=215) && (grayValue<=255)){
            colorB=round((grayValue-214)/41.0*255.0);
			}
		}
    else if (mode==4){ 
        if ((grayValue>0) && (grayValue<=31)){
            colorR=0; 
            colorG=0; 
            colorB=round(grayValue/32.0*255.0);
			}			
        else if ((grayValue>=32) && (grayValue<=63)){
            colorR=0; 
            colorG=round((grayValue-32)/32.0*255.0); 
            colorB=255;
			}			
        else if ((grayValue>=64) && (grayValue<=95)){
            colorR=0; 
            colorG=255; 
            colorB=round((95-grayValue)/32.0*255.0);
			}
        else if ((grayValue>=96) && (grayValue<=127)){
            colorR=round((grayValue-96)/32.0*255.0); 
            colorG=255;
            colorB=0;
			}
        else if ((grayValue>=128) && (grayValue<=191)){
            colorR=255; 
            colorG=round((191-grayValue)/64.0*255.0); 
            colorB=0;
			}
        else if ((grayValue>=192) && (grayValue<=255)){
            colorR=255;
            colorG=round((grayValue-192)/64.0*255.0);
            colorB=round((grayValue-192)/64.0*255.0); 
			}
		}
    else if (mode==5){
        if ((grayValue>0) && (grayValue<=63)){
            colorR=0;
            colorG=round((grayValue-0)/64.0*255.0);
            colorB=255;
			}
        else if ((grayValue>=64) && (grayValue<=95)){
            colorR=0; 
            colorG=255; 
            colorB=round((95-grayValue)/32.0*255.0);
			}
        else if ((grayValue>=96) && (grayValue<=127)){
            colorR=round((grayValue-96)/32.0*255.0); 
            colorG=255; 
            colorB=0;
			}
        else if ((grayValue>=128) && (grayValue<=191)){
            colorR=255 ;
            colorG=round((191-grayValue)/64.0*255.0); 
            colorB=0;
			}
        else if ((grayValue>=192) && (grayValue<=255)){
            colorR=255;
            colorG=round((grayValue-192)/64.0*255.0);
            colorB=round((grayValue-192)/64.0*255.0);
			}
		}
    else if (mode==6){
        if ((grayValue>0) && (grayValue<=51)){
            colorR=0;
            colorG=grayValue*5;
            colorB=255;
			}
        else if ((grayValue>=52) && (grayValue<=102)){
            colorR=0;
            colorG=255;
            colorB=255-(grayValue-51)*5;
			}
        else if ((grayValue>=103) && (grayValue<=153)){
            colorR=(grayValue-102)*5;
            colorG=255;
            colorB=0;
			}
        else if ((grayValue>=154) && (grayValue<=204)){
            colorR=255;
            colorG=round(255.0-128.0*(grayValue-153.0)/51.0);
            colorB=0;
			}
        else if ((grayValue>=205) && (grayValue<=255)){
            colorR=255;
            colorG=round(127.0-127.0*(grayValue-204.0)/51.0);
            colorB=0;
			}
		}
    else if (mode==7){
        if ((grayValue>0) && (grayValue<=63)){
            colorR=0;
            colorG=round((64-grayValue)/64.0*255.0);
            colorB=255;
			}
        else if ((grayValue>=64) && (grayValue<=127)){
            colorR=0 ;
            colorG=round((grayValue-64)/64.0*255.0);
            colorB=round((127-grayValue)/64.0*255.0);
			}
        else if ((grayValue>=128) && (grayValue<=191)){
            colorR=round((grayValue-128)/64.0*255.0);
            colorG=255;
            colorB=0;
			}
        else if ((grayValue>=192) && (grayValue<=255)){
            colorR=255;
            colorG=round((255-grayValue)/64.0*255.0);
            colorB=0;
			}
		}
    else if (mode==8){
        if ((grayValue>0) && (grayValue<=63)){
            colorR=0;
            colorG=254-4*grayValue;
            colorB=255;
			}
        else if ((grayValue>=64) && (grayValue<=127)){
            colorR=0;
            colorG=4*grayValue-254;
            colorB=510-4*grayValue;
			}
        else if ((grayValue>=128) && (grayValue<=191)){
            colorR=4*grayValue-510;
            colorG=255;
            colorB=0;
			}
        else if ((grayValue>=192) && (grayValue<=255)){
            colorR=255;
            colorG=1022-4*grayValue;
            colorB=0;
			}
		}
    else{
        colorR=grayValue;
        colorG=grayValue; 
        colorB=grayValue;
	}
	return rgb_565(colorR,colorG,colorB);
}

/* 生成整套 256 色伪彩色查找表。 */
void color_listcode(uint16_t *color_list,uint16_t mode ){
	uint16_t i;
	for (i=0;i<256;i++){
        uint16_t color = color_code(i,mode);
		color_list[i]=color;
#if LCD_DMA_STAGE6_6C_ACTIVE
        g_colorHighByteLut[i] = (uint8_t)(color >> 8);
        g_colorLowByteLut[i] = (uint8_t)(color & 0xFFU);
#endif
	}
}
/* 设置伪彩色模式并重建 LUT。 */
void set_color_mode(uint16_t mode) {
    color_listcode(GCM_Pseudo3, mode); // 重建 LUT
}

/* 热成像显示主入口：
 * 输入 24x32 灰度帧，插值后逐行 DMA 输出到 LCD。 */
uint8_t LCD_Disp_Thermal_Interpolated_DMA(uint8_t *data24x32)
{
    uint32_t start_cycle = app_perf_baseline_cycle_now();
    uint8_t tx_buffer_index = 0U;
    uint8_t fill_buffer_index = 1U;
    uint16_t next_start_row = 0U;
    uint16_t transfer_row_count = 0U;

    if (data24x32 == 0)
    {
        return 0U;
    }

    lcd_dma_perf_reset_frame();
    app_perf_baseline_record_lcd_dma_enter();
#if LCD_DMA_STAGE6_6B_ACTIVE
    lcd_dma_init_render_mappings();
#endif
    s_lcd_dma_source_frame = data24x32;
#if (LCD_DMA_LOW_COST_RENDER_TEST_ACTIVE == 0U)
    build_horizontal_interp_rows(data24x32);
    build_vertical_edge_rows();
#endif

    LCD_Address_Set(0, 0, (u16)(LCD_W - 1), (u16)(THERMAL_OUTPUT_ROWS - 1U));
    LCD_DC_Set();
    LCD_CS_Clr();

    transfer_row_count = lcd_dma_get_stripe_row_count(0U);
    activeBuffer = tx_buffer_index;
    render_output_rows_to_buffer(0U, transfer_row_count, lineBuffer[tx_buffer_index]);
    if (start_dma_line_transfer(lineBuffer[tx_buffer_index], transfer_row_count) == 0U) {
        LCD_CS_Set();
        s_dma_mode = LCD_DMA_MODE_IDLE;
        lcd_dma_perf_commit_frame();
        s_lcd_dma_source_frame = 0;
        app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                                s_dma_last_status);
        return 0U;
    }

    next_start_row = transfer_row_count;
    while (next_start_row < THERMAL_OUTPUT_ROWS) {
        transfer_row_count = lcd_dma_get_stripe_row_count(next_start_row);
        render_output_rows_to_buffer(next_start_row,
                                     transfer_row_count,
                                     lineBuffer[fill_buffer_index]);

        if (wait_for_dma_transfer_complete() == 0U) {
            LCD_CS_Set();
            s_dma_mode = LCD_DMA_MODE_IDLE;
            lcd_dma_perf_commit_frame();
            s_lcd_dma_source_frame = 0;
            app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                                    s_dma_last_status);
            return 0U;
        }

        tx_buffer_index = fill_buffer_index;
        fill_buffer_index ^= 1U;
        activeBuffer = tx_buffer_index;
        if (start_dma_line_transfer(lineBuffer[tx_buffer_index], transfer_row_count) == 0U) {
            LCD_CS_Set();
            s_dma_mode = LCD_DMA_MODE_IDLE;
            lcd_dma_perf_commit_frame();
            s_lcd_dma_source_frame = 0;
            app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                                    s_dma_last_status);
            return 0U;
        }

        next_start_row = (uint16_t)(next_start_row + transfer_row_count);
    }

    if (wait_for_dma_transfer_complete() == 0U) {
        LCD_CS_Set();
        s_dma_mode = LCD_DMA_MODE_IDLE;
        lcd_dma_perf_commit_frame();
        s_lcd_dma_source_frame = 0;
        app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                                s_dma_last_status);
        return 0U;
    }

    LCD_CS_Set();
    s_dma_mode = LCD_DMA_MODE_IDLE;
    lcd_dma_perf_commit_frame();
    s_lcd_dma_source_frame = 0;
    app_perf_baseline_record_lcd_dma_result(app_perf_baseline_elapsed_us(start_cycle),
                                            APP_PERF_LCD_DMA_STATUS_OK);
    return 1U;
}
