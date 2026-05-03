/**
 * @file    app_perf_baseline.c
 * @brief   性能基线诊断模块 —— 实现
 * @note    本模块通过 volatile 计数器和统计累加器采集系统运行时性能数据。
 *          所有 record 函数均受 APP_PERF_BASELINE_ENABLE 编译宏控制，
 *          关闭后编译为空操作（参数仅做 void 转换，消除未使用警告）。
 *
 * @par 设计要点
 *      - volatile 保证多任务/中断环境下计数器的可见性
 *      - 统计累加器（app_perf_stat_accum_t）跟踪 min/max/count/total
 *      - get_snapshot 在临界区内批量拷贝，保证快照原子性
 *      - DWT 周期计数器提供亚微秒级时间测量
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "app_perf_baseline.h"

#include <string.h>

#include "sys.h"

/* =========================================================================
 *  2. 内部数据类型定义
 * ======================================================================= */

/**
 * @brief 统计累加器结构体
 * @note  跟踪一组采样值的最近值、最小值、最大值、采样数和总和，
 *        用于计算运行平均值。
 */
typedef struct
{
    uint32_t last;      /**< 最近一次采样值              */
    uint32_t min;       /**< 历史最小值                  */
    uint32_t max;       /**< 历史最大值                  */
    uint32_t count;     /**< 采样计数                    */
    uint64_t total;     /**< 采样值累加和（用于求平均）  */
} app_perf_stat_accum_t;

/* =========================================================================
 *  3. 热成像帧率与捕获计数器
 * ======================================================================= */

static volatile uint32_t s_thermal_capture_frames       = 0U;   /**< 热成像捕获帧总数          */
static volatile uint32_t s_thermal_display_frames       = 0U;   /**< 热成像显示帧总数          */
static volatile uint32_t s_thermal_capture_failures     = 0U;   /**< 热成像捕获失败次数        */
static volatile uint32_t s_thermal_fps                  = 0U;   /**< 热成像采集帧率            */
static volatile uint32_t s_thermal_display_fps          = 0U;   /**< 热成像显示帧率            */
static volatile uint32_t s_lcd_present_fps              = 0U;   /**< LCD 呈现帧率              */
static volatile uint32_t s_last_capture_tick_ms         = 0U;   /**< 最近捕获 tick（ms）       */
static volatile uint32_t s_fps_window_start_ms          = 0U;   /**< 采集 FPS 窗口起始时间     */
static volatile uint32_t s_fps_window_count             = 0U;   /**< 采集 FPS 窗口内帧数       */
static volatile uint32_t s_display_fps_window_start_ms  = 0U;   /**< 显示 FPS 窗口起始时间     */
static volatile uint32_t s_display_fps_window_count     = 0U;   /**< 显示 FPS 窗口内帧数       */

/* =========================================================================
 *  4. 最新温度数据
 * ======================================================================= */

static volatile float s_latest_min_temp     = 0.0f;     /**< 最近一次最低温度（℃）    */
static volatile float s_latest_max_temp     = 0.0f;     /**< 最近一次最高温度（℃）    */
static volatile float s_latest_center_temp  = 0.0f;     /**< 最近一次中心温度（℃）    */

/* =========================================================================
 *  5. 队列溢出计数器
 * ======================================================================= */

static volatile uint32_t s_key_queue_drop_count         = 0U;   /**< 按键队列溢出次数          */
static volatile uint32_t s_ui_msg_drop_count            = 0U;   /**< UI 消息队列溢出次数       */
static volatile uint32_t s_service_queue_fail_count     = 0U;   /**< 服务队列入队失败次数      */
static volatile uint32_t s_display_queue_fail_count     = 0U;   /**< 显示队列入队失败次数      */

/* =========================================================================
 *  6. TaskNotify 计数器
 * ======================================================================= */

static volatile uint32_t s_input_notify_count           = 0U;   /**< 输入任务 Notify 次数      */
static volatile uint32_t s_ui_notify_count              = 0U;   /**< UI 任务 Notify 次数       */
static volatile uint32_t s_service_notify_count         = 0U;   /**< 服务任务 Notify 次数      */
static volatile uint32_t s_display_notify_count         = 0U;   /**< 显示任务 Notify 次数      */

/* =========================================================================
 *  7. UART 错误计数器
 * ======================================================================= */

static volatile uint32_t s_uart_error_count             = 0U;   /**< UART 错误总次数           */
static volatile uint32_t s_last_uart_error_flags        = 0U;   /**< 最近一次 UART 错误标志    */

/* =========================================================================
 *  8. I2C 错误分类计数器
 * ======================================================================= */

static volatile uint32_t s_i2c_failure_count            = 0U;   /**< I2C 失败总次数            */
static volatile uint32_t s_i2c_af_count                 = 0U;   /**< 应答失败次数              */
static volatile uint32_t s_i2c_berr_count               = 0U;   /**< 总线错误次数              */
static volatile uint32_t s_i2c_arlo_count               = 0U;   /**< 仲裁丢失次数              */
static volatile uint32_t s_i2c_ovr_count                = 0U;   /**< 溢出错误次数              */
static volatile uint32_t s_i2c_timeout_count            = 0U;   /**< I2C 超时次数              */
static volatile uint32_t s_i2c_busy_stuck_count         = 0U;   /**< 总线忙卡死次数            */
static volatile uint32_t s_i2c_dma_err_count            = 0U;   /**< I2C DMA 错误次数          */

/* =========================================================================
 *  9. I2C DMA 快照计数器（超时时 / 传输完成时）
 * ======================================================================= */

static volatile uint32_t s_i2c_dma_timeout_ndtr         = 0U;   /**< 超时时 DMA NDTR           */
static volatile uint32_t s_i2c_dma_timeout_state        = 0U;   /**< 超时时 I2C 状态机         */
static volatile uint32_t s_i2c_dma_timeout_sr1          = 0U;   /**< 超时时 SR1                */
static volatile uint32_t s_i2c_dma_timeout_sr2          = 0U;   /**< 超时时 SR2                */
static volatile uint32_t s_i2c_dma_tc_ndtr              = 0U;   /**< 完成时 DMA NDTR           */
static volatile uint32_t s_i2c_dma_tc_state             = 0U;   /**< 完成时 I2C 状态机         */
static volatile uint32_t s_i2c_dma_tc_sr1               = 0U;   /**< 完成时 SR1                */
static volatile uint32_t s_i2c_dma_tc_sr2               = 0U;   /**< 完成时 SR2                */

/* =========================================================================
 *  10. I2C DMA 中断与超时计数器
 * ======================================================================= */

static volatile uint32_t s_i2c_dma_ev_irq_count         = 0U;   /**< I2C 事件中断次数          */
static volatile uint32_t s_i2c_dma_tc_irq_count         = 0U;   /**< DMA 传输完成中断次数      */
static volatile uint32_t s_i2c_dma_wait_timeout_count   = 0U;   /**< DMA 等待超时次数          */

/* =========================================================================
 *  11. I2C 轮询超时详情计数器
 * ======================================================================= */

static volatile uint32_t s_i2c_poll_event_timeout_count = 0U;   /**< 轮询事件超时次数          */
static volatile uint32_t s_i2c_poll_busy_timeout_count  = 0U;   /**< 轮询总线忙超时次数        */
static volatile uint32_t s_i2c_er_timeout_count         = 0U;   /**< 错误中断超时次数          */
static volatile uint32_t s_i2c_poll_timeout_path        = 0U;   /**< 最近超时的轮询路径        */
static volatile uint32_t s_i2c_poll_timeout_phase       = 0U;   /**< 最近超时的轮询阶段        */
static volatile uint32_t s_i2c_poll_timeout_event       = 0U;   /**< 最近超时的事件标志        */
static volatile uint32_t s_i2c_poll_timeout_sr1         = 0U;   /**< 最近超时时 SR1            */
static volatile uint32_t s_i2c_poll_timeout_sr2         = 0U;   /**< 最近超时时 SR2            */
static volatile uint32_t s_i2c_poll_timeout_start_addr  = 0U;   /**< 最近超时的起始地址        */
static volatile uint32_t s_i2c_poll_timeout_word_count  = 0U;   /**< 最近超时的字数            */

/* =========================================================================
 *  12. I2C 轮询超时分类计数器
 * ======================================================================= */

static volatile uint32_t s_i2c_poll_timeout_read_count      = 0U;   /**< 读路径超时次数        */
static volatile uint32_t s_i2c_poll_timeout_write_count     = 0U;   /**< 写路径超时次数        */
static volatile uint32_t s_i2c_poll_timeout_verify_count    = 0U;   /**< 校验读路径超时次数    */
static volatile uint32_t s_i2c_addrw_timeout_read_count     = 0U;   /**< 读-ADDR_W 超时次数    */
static volatile uint32_t s_i2c_addrw_timeout_write_count    = 0U;   /**< 写-ADDR_W 超时次数    */
static volatile uint32_t s_i2c_addr_8000_timeout_read_count = 0U;   /**< 读 0x8000 超时次数    */
static volatile uint32_t s_i2c_addr_8000_timeout_write_count= 0U;   /**< 写 0x8000 超时次数    */
static volatile uint32_t s_i2c_addr_800d_timeout_read_count = 0U;   /**< 读 0x800D 超时次数    */
static volatile uint32_t s_i2c_addr_800d_timeout_write_count= 0U;   /**< 写 0x800D 超时次数    */
static volatile uint32_t s_i2c_r8000_addrw_timeout_count    = 0U;   /**< 读 0x8000+ADDR_W 超时 */
static volatile uint32_t s_i2c_w8000_addrw_timeout_count    = 0U;   /**< 写 0x8000+ADDR_W 超时 */
static volatile uint32_t s_i2c_r800d_addrw_timeout_count    = 0U;   /**< 读 0x800D+ADDR_W 超时 */
static volatile uint32_t s_i2c_r800d_rx_timeout_count       = 0U;   /**< 读 0x800D+RX 超时     */

/* =========================================================================
 *  13. I2C 总线忙超时分类计数器
 * ======================================================================= */

static volatile uint32_t s_i2c_busy_timeout_read_count      = 0U;   /**< 读-总线忙超时次数     */
static volatile uint32_t s_i2c_busy_timeout_write_count     = 0U;   /**< 写-总线忙超时次数     */
static volatile uint32_t s_i2c_busy_timeout_verify_count    = 0U;   /**< 校验读-总线忙超时次数 */

/* =========================================================================
 *  14. I2C 总线恢复（Bus Clear）计数器
 * ======================================================================= */

static volatile uint32_t s_i2c_bus_clear_count              = 0U;   /**< Bus Clear 总次数      */
static volatile uint32_t s_i2c_stop_release_timeout_count   = 0U;   /**< STOP 释放超时次数     */
static volatile uint32_t s_i2c_bus_clear_read_count         = 0U;   /**< 读触发 Bus Clear      */
static volatile uint32_t s_i2c_bus_clear_write_count        = 0U;   /**< 写触发 Bus Clear      */
static volatile uint32_t s_i2c_bus_clear_dma_count          = 0U;   /**< DMA 触发 Bus Clear    */
static volatile uint32_t s_i2c_bus_clear_busy_timeout_count = 0U;   /**< 忙超时触发 Bus Clear  */

/* =========================================================================
 *  15. DMA 与热成像退避计数器
 * ======================================================================= */

static volatile uint32_t s_dma_timeout_count                = 0U;   /**< DMA 超时总次数        */
static volatile uint32_t s_thermal_backoff_count            = 0U;   /**< 热成像退避次数        */

/* =========================================================================
 *  16. 热成像子页配对计数器
 * ======================================================================= */

static volatile uint32_t s_thermal_pair_timeout_count               = 0U;       /**< 配对超时次数      */
static volatile uint32_t s_thermal_pair_grace_ok_count              = 0U;       /**< 宽限期成功次数    */
static volatile uint32_t s_thermal_pair_compose_ok_count            = 0U;       /**< 正常合成成功次数  */
static volatile uint32_t s_thermal_pair_wait_other_count            = 0U;       /**< 等待另一子页次数  */
static volatile uint32_t s_thermal_pair_last_result                 = APP_PERF_THERMAL_PAIR_RESULT_NONE; /**< 最近配对结果 */
static volatile uint32_t s_thermal_pair_last_subpage               = 0xFFU;    /**< 最近子页编号      */
static volatile uint32_t s_thermal_pair_last_missing_subpage       = 0xFFU;    /**< 最近缺失子页      */
static volatile uint32_t s_thermal_pair_last_gap_ms                = 0U;       /**< 最近配对间隔      */
static volatile uint32_t s_thermal_pair_timeout_gap_last_ms        = 0U;       /**< 超时间隔-最近     */
static volatile uint32_t s_thermal_pair_timeout_gap_max_ms         = 0U;       /**< 超时间隔-最大     */
static volatile uint32_t s_thermal_pair_timeout_gap_80_120_count   = 0U;       /**< 超时间隔 80~120   */
static volatile uint32_t s_thermal_pair_timeout_gap_120_160_count  = 0U;       /**< 超时间隔 120~160  */
static volatile uint32_t s_thermal_pair_timeout_gap_160_240_count  = 0U;       /**< 超时间隔 160~240  */
static volatile uint32_t s_thermal_pair_timeout_gap_240_plus_count = 0U;       /**< 超时间隔 >240     */
static volatile uint32_t s_thermal_pair_compose_gap_last_ms        = 0U;       /**< 合成间隔-最近     */
static volatile uint32_t s_thermal_pair_compose_gap_max_ms         = 0U;       /**< 合成间隔-最大     */
static volatile uint32_t s_thermal_pair_same_subpage_streak_last   = 0U;       /**< 连续同子页-最近   */
static volatile uint32_t s_thermal_pair_same_subpage_streak_max    = 0U;       /**< 连续同子页-最大   */
static volatile uint32_t s_thermal_pair_timeout_get_temp_last_us   = 0U;       /**< 超时时 GetTemp    */
static volatile uint32_t s_thermal_pair_timeout_step_last_us       = 0U;       /**< 超时时步进耗时    */
static volatile uint32_t s_thermal_pair_soft_timeout_count         = 0U;       /**< 软超时次数        */
static volatile uint32_t s_thermal_pair_back_slot_null_count       = 0U;       /**< 后备槽为空次数    */
static volatile uint32_t s_thermal_ready_replace_count             = 0U;       /**< 就绪替换次数      */
static volatile uint32_t s_thermal_display_cancel_count            = 0U;       /**< 显示取消次数      */

/* =========================================================================
 *  17. 热成像 3D 同步计数器
 * ======================================================================= */

static volatile uint32_t s_thermal_3d_sync_present_attempt_count   = 0U;   /**< 3D 同步呈现尝试   */
static volatile uint32_t s_thermal_3d_sync_present_ok_count        = 0U;   /**< 3D 同步呈现成功   */
static volatile uint32_t s_thermal_3d_sync_present_fail_count      = 0U;   /**< 3D 同步呈现失败   */
static volatile uint32_t s_thermal_3d_claim_count                  = 0U;   /**< 3D 帧认领次数     */
static volatile uint32_t s_thermal_3d_done_ok_count                = 0U;   /**< 3D 完成-成功      */
static volatile uint32_t s_thermal_3d_done_error_count             = 0U;   /**< 3D 完成-错误      */
static volatile uint32_t s_thermal_3d_done_cancel_count            = 0U;   /**< 3D 完成-取消      */
static volatile uint32_t s_thermal_3d_wait_timeout_count           = 0U;   /**< 3D 等待超时       */

/* =========================================================================
 *  18. LCD DMA 事件与状态计数器
 * ======================================================================= */

static volatile uint32_t s_lcd_dma_enter_count          = 0U;   /**< LCD DMA 函数入口次数      */
static volatile uint32_t s_dma_irq_tc_count             = 0U;   /**< DMA 传输完成中断次数      */
static volatile uint32_t s_dma_irq_te_count             = 0U;   /**< DMA 传输错误中断次数      */
static volatile uint32_t s_dma_wait_take_count          = 0U;   /**< DMA 信号量获取次数        */
static volatile uint8_t  s_last_dma_ok                  = 0U;   /**< 最近一次 DMA 是否成功     */
static volatile uint8_t  s_last_dma_status              = APP_PERF_LCD_DMA_STATUS_NONE; /**< 最近 DMA 状态 */

/* =========================================================================
 *  19. 看门狗状态计数器
 * ======================================================================= */

static volatile uint32_t s_watchdog_missing_progress_mask = 0U; /**< 缺失进展的任务掩码        */
static volatile uint32_t s_watchdog_fault_flags           = 0U; /**< 看门狗故障标志            */

/* =========================================================================
 *  20. 运行时状态
 * ======================================================================= */

static volatile uint8_t          s_thermal_active   = 0U;               /**< 热成像是否活跃        */
static volatile uint8_t          s_screen_off       = 0U;               /**< 屏幕是否关闭          */
static volatile power_state_t    s_power_state      = POWER_STATE_ACTIVE_UI;  /**< 当前电源状态    */
static volatile clock_profile_t  s_clock_profile    = CLOCK_PROFILE_HIGH;     /**< 当前时钟配置    */

/* =========================================================================
 *  21. 任务栈水位
 * ======================================================================= */

static volatile UBaseType_t s_input_stack_words     = 0U;   /**< 输入任务栈剩余字数        */
static volatile UBaseType_t s_service_stack_words   = 0U;   /**< 服务任务栈剩余字数        */
static volatile UBaseType_t s_ui_stack_words        = 0U;   /**< UI 任务栈剩余字数         */
static volatile UBaseType_t s_display_stack_words   = 0U;   /**< 显示任务栈剩余字数        */
static volatile UBaseType_t s_thermal_stack_words   = 0U;   /**< 热成像任务栈剩余字数      */
static volatile UBaseType_t s_power_stack_words     = 0U;   /**< 电源任务栈剩余字数        */

/* =========================================================================
 *  22. 统计累加器实例
 * ======================================================================= */

static app_perf_stat_accum_t s_frame_period_stats;          /**< 帧周期统计累加器          */
static app_perf_stat_accum_t s_thermal_display_age_stats;   /**< 显示延迟统计累加器        */
static app_perf_stat_accum_t s_get_temp_stats;              /**< GetTemp 耗时统计累加器    */
static app_perf_stat_accum_t s_gray_stats;                  /**< 灰度转换耗时统计累加器    */
static app_perf_stat_accum_t s_thermal_step_stats;          /**< 热成像步进耗时统计累加器  */
static app_perf_stat_accum_t s_lcd_dma_stats;               /**< DMA 总耗时统计累加器      */
static app_perf_stat_accum_t s_lcd_dma_render_stats;        /**< DMA 渲染阶段统计累加器    */
static app_perf_stat_accum_t s_lcd_dma_start_stats;         /**< DMA 启动阶段统计累加器    */
static app_perf_stat_accum_t s_lcd_dma_wait_stats;          /**< DMA 等待阶段统计累加器    */
static app_perf_stat_accum_t s_lcd_dma_spi_idle_stats;      /**< SPI 空闲等待统计累加器    */
static app_perf_stat_accum_t s_lcd_dma_overlay_stats;       /**< 十字光标叠加统计累加器    */

/* =========================================================================
 *  23. 内部工具函数
 * ======================================================================= */

/**
 * @brief  判断 FreeRTOS 调度器是否正在运行
 * @retval 1 — 调度器运行中；0 — 尚未启动或已挂起
 */
static uint8_t app_perf_baseline_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

/**
 * @brief  重置统计累加器
 * @param  stat — 累加器指针
 */
static void app_perf_stat_reset(app_perf_stat_accum_t *stat)
{
    if (stat == 0)
    {
        return;
    }

    memset(stat, 0, sizeof(*stat));
}

/**
 * @brief  向统计累加器添加一个采样值
 * @note   更新最近值、最小值、最大值、采样数和累加和。
 * @param  stat  — 累加器指针
 * @param  value — 采样值
 */
static void app_perf_stat_add(app_perf_stat_accum_t *stat, uint32_t value)
{
    if (stat == 0)
    {
        return;
    }

    stat->last = value;

    if (stat->count == 0U || value < stat->min)
    {
        stat->min = value;
    }

    if (value > stat->max)
    {
        stat->max = value;
    }

    stat->count++;
    stat->total += value;
}

/**
 * @brief  计算统计累加器的平均值
 * @param  stat — 累加器指针
 * @return 平均值（采样数为 0 时返回 0）
 */
static uint32_t app_perf_stat_avg(const app_perf_stat_accum_t *stat)
{
    if (stat == 0 || stat->count == 0U)
    {
        return 0U;
    }

    return (uint32_t)(stat->total / stat->count);
}

/**
 * @brief  按路径和地址分类记录 I2C 超时
 * @note   根据路径（读/写/校验读）、阶段和起始地址，
 *        累加对应的细粒度分类计数器。
 * @param  path       — 轮询路径
 * @param  phase      — 轮询阶段
 * @param  start_addr — 起始寄存器地址
 */
static void app_perf_baseline_record_i2c_timeout_classified(app_perf_i2c_poll_path_t path,
                                                            app_perf_i2c_poll_phase_t phase,
                                                            uint16_t start_addr)
{
    switch (path)
    {
    case APP_PERF_I2C_POLL_PATH_READ:
        s_i2c_poll_timeout_read_count++;

        if (phase == APP_PERF_I2C_POLL_PHASE_ADDR_W)
        {
            s_i2c_addrw_timeout_read_count++;
        }

        if (start_addr == 0x8000U)
        {
            s_i2c_addr_8000_timeout_read_count++;

            if (phase == APP_PERF_I2C_POLL_PHASE_ADDR_W)
            {
                s_i2c_r8000_addrw_timeout_count++;
            }
        }
        else if (start_addr == 0x800DU)
        {
            s_i2c_addr_800d_timeout_read_count++;

            if (phase == APP_PERF_I2C_POLL_PHASE_ADDR_W)
            {
                s_i2c_r800d_addrw_timeout_count++;
            }
            else if (phase == APP_PERF_I2C_POLL_PHASE_BYTE_RECEIVED)
            {
                s_i2c_r800d_rx_timeout_count++;
            }
        }
        break;

    case APP_PERF_I2C_POLL_PATH_WRITE:
        s_i2c_poll_timeout_write_count++;

        if (phase == APP_PERF_I2C_POLL_PHASE_ADDR_W)
        {
            s_i2c_addrw_timeout_write_count++;
        }

        if (start_addr == 0x8000U)
        {
            s_i2c_addr_8000_timeout_write_count++;

            if (phase == APP_PERF_I2C_POLL_PHASE_ADDR_W)
            {
                s_i2c_w8000_addrw_timeout_count++;
            }
        }
        else if (start_addr == 0x800DU)
        {
            s_i2c_addr_800d_timeout_write_count++;
        }
        break;

    case APP_PERF_I2C_POLL_PATH_VERIFY_READ:
        s_i2c_poll_timeout_verify_count++;
        break;

    case APP_PERF_I2C_POLL_PATH_NONE:
    default:
        break;
    }
}

/**
 * @brief  将 DWT 周期数转换为微秒
 * @param  cycles — DWT 周期数
 * @return 对应的微秒数
 */
static uint32_t app_perf_cycles_to_us(uint32_t cycles)
{
    uint32_t cycles_per_us = SystemCoreClock / 1000000UL;

    if (cycles_per_us == 0U)
    {
        return 0U;
    }

    return cycles / cycles_per_us;
}

/* =========================================================================
 *  24. 初始化与控制接口实现
 * ======================================================================= */

/**
 * @brief  初始化性能基线模块
 * @note   重置所有计数器并启用 DWT 周期计数器（需 CoreDebug 支持）。
 */
void app_perf_baseline_init(void)
{
    app_perf_baseline_reset();

#if APP_PERF_BASELINE_ENABLE
    /* 启用 DWT 跟踪 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
#endif
}

/**
 * @brief  重置所有性能计数器与统计量
 * @note   在临界区内批量清零，保证多任务环境下的原子性。
 */
void app_perf_baseline_reset(void)
{
    if (app_perf_baseline_scheduler_running() != 0U)
    {
        taskENTER_CRITICAL();
    }

    /* ---- 热成像帧率与捕获 ---- */
    s_thermal_capture_frames        = 0U;
    s_thermal_display_frames        = 0U;
    s_thermal_capture_failures      = 0U;
    s_thermal_fps                   = 0U;
    s_thermal_display_fps           = 0U;
    s_lcd_present_fps               = 0U;
    s_last_capture_tick_ms          = 0U;
    s_fps_window_start_ms           = 0U;
    s_fps_window_count              = 0U;
    s_display_fps_window_start_ms   = 0U;
    s_display_fps_window_count      = 0U;

    /* ---- 最新温度 ---- */
    s_latest_min_temp       = 0.0f;
    s_latest_max_temp       = 0.0f;
    s_latest_center_temp    = 0.0f;

    /* ---- 队列溢出 ---- */
    s_key_queue_drop_count      = 0U;
    s_ui_msg_drop_count         = 0U;
    s_service_queue_fail_count  = 0U;
    s_display_queue_fail_count  = 0U;

    /* ---- TaskNotify ---- */
    s_input_notify_count    = 0U;
    s_ui_notify_count       = 0U;
    s_service_notify_count  = 0U;
    s_display_notify_count  = 0U;

    /* ---- UART 错误 ---- */
    s_uart_error_count          = 0U;
    s_last_uart_error_flags     = 0U;

    /* ---- I2C 错误分类 ---- */
    s_i2c_failure_count     = 0U;
    s_i2c_af_count          = 0U;
    s_i2c_berr_count        = 0U;
    s_i2c_arlo_count        = 0U;
    s_i2c_ovr_count         = 0U;
    s_i2c_timeout_count     = 0U;
    s_i2c_busy_stuck_count  = 0U;
    s_i2c_dma_err_count     = 0U;

    /* ---- I2C DMA 快照 ---- */
    s_i2c_dma_timeout_ndtr  = 0U;
    s_i2c_dma_timeout_state = 0U;
    s_i2c_dma_timeout_sr1   = 0U;
    s_i2c_dma_timeout_sr2   = 0U;
    s_i2c_dma_tc_ndtr       = 0U;
    s_i2c_dma_tc_state      = 0U;
    s_i2c_dma_tc_sr1        = 0U;
    s_i2c_dma_tc_sr2        = 0U;

    /* ---- I2C DMA 中断与超时 ---- */
    s_i2c_dma_ev_irq_count      = 0U;
    s_i2c_dma_tc_irq_count      = 0U;
    s_i2c_dma_wait_timeout_count = 0U;

    /* ---- I2C 轮询超时详情 ---- */
    s_i2c_poll_event_timeout_count  = 0U;
    s_i2c_poll_busy_timeout_count   = 0U;
    s_i2c_er_timeout_count          = 0U;
    s_i2c_poll_timeout_path         = 0U;
    s_i2c_poll_timeout_phase        = 0U;
    s_i2c_poll_timeout_event        = 0U;
    s_i2c_poll_timeout_sr1          = 0U;
    s_i2c_poll_timeout_sr2          = 0U;
    s_i2c_poll_timeout_start_addr   = 0U;
    s_i2c_poll_timeout_word_count   = 0U;

    /* ---- I2C 轮询超时分类 ---- */
    s_i2c_poll_timeout_read_count       = 0U;
    s_i2c_poll_timeout_write_count      = 0U;
    s_i2c_poll_timeout_verify_count     = 0U;
    s_i2c_addrw_timeout_read_count      = 0U;
    s_i2c_addrw_timeout_write_count     = 0U;
    s_i2c_addr_8000_timeout_read_count  = 0U;
    s_i2c_addr_8000_timeout_write_count = 0U;
    s_i2c_addr_800d_timeout_read_count  = 0U;
    s_i2c_addr_800d_timeout_write_count = 0U;
    s_i2c_r8000_addrw_timeout_count     = 0U;
    s_i2c_w8000_addrw_timeout_count     = 0U;
    s_i2c_r800d_addrw_timeout_count     = 0U;
    s_i2c_r800d_rx_timeout_count        = 0U;

    /* ---- I2C 总线忙超时分类 ---- */
    s_i2c_busy_timeout_read_count   = 0U;
    s_i2c_busy_timeout_write_count  = 0U;
    s_i2c_busy_timeout_verify_count = 0U;

    /* ---- I2C 总线恢复 ---- */
    s_i2c_bus_clear_count               = 0U;
    s_i2c_stop_release_timeout_count    = 0U;
    s_i2c_bus_clear_read_count          = 0U;
    s_i2c_bus_clear_write_count         = 0U;
    s_i2c_bus_clear_dma_count           = 0U;
    s_i2c_bus_clear_busy_timeout_count  = 0U;

    /* ---- DMA 与热成像退避 ---- */
    s_dma_timeout_count     = 0U;
    s_thermal_backoff_count = 0U;

    /* ---- 热成像子页配对 ---- */
    s_thermal_pair_timeout_count                = 0U;
    s_thermal_pair_grace_ok_count               = 0U;
    s_thermal_pair_compose_ok_count             = 0U;
    s_thermal_pair_wait_other_count             = 0U;
    s_thermal_pair_last_result                  = APP_PERF_THERMAL_PAIR_RESULT_NONE;
    s_thermal_pair_last_subpage                 = 0xFFU;
    s_thermal_pair_last_missing_subpage         = 0xFFU;
    s_thermal_pair_last_gap_ms                  = 0U;
    s_thermal_pair_timeout_gap_last_ms          = 0U;
    s_thermal_pair_timeout_gap_max_ms           = 0U;
    s_thermal_pair_timeout_gap_80_120_count     = 0U;
    s_thermal_pair_timeout_gap_120_160_count    = 0U;
    s_thermal_pair_timeout_gap_160_240_count    = 0U;
    s_thermal_pair_timeout_gap_240_plus_count   = 0U;
    s_thermal_pair_compose_gap_last_ms          = 0U;
    s_thermal_pair_compose_gap_max_ms           = 0U;
    s_thermal_pair_same_subpage_streak_last     = 0U;
    s_thermal_pair_same_subpage_streak_max      = 0U;
    s_thermal_pair_timeout_get_temp_last_us     = 0U;
    s_thermal_pair_timeout_step_last_us         = 0U;
    s_thermal_pair_soft_timeout_count           = 0U;
    s_thermal_pair_back_slot_null_count         = 0U;
    s_thermal_ready_replace_count               = 0U;
    s_thermal_display_cancel_count              = 0U;

    /* ---- 热成像 3D 同步 ---- */
    s_thermal_3d_sync_present_attempt_count = 0U;
    s_thermal_3d_sync_present_ok_count      = 0U;
    s_thermal_3d_sync_present_fail_count    = 0U;
    s_thermal_3d_claim_count                = 0U;
    s_thermal_3d_done_ok_count              = 0U;
    s_thermal_3d_done_error_count           = 0U;
    s_thermal_3d_done_cancel_count          = 0U;
    s_thermal_3d_wait_timeout_count         = 0U;

    /* ---- LCD DMA 事件 ---- */
    s_lcd_dma_enter_count   = 0U;
    s_dma_irq_tc_count      = 0U;
    s_dma_irq_te_count      = 0U;
    s_dma_wait_take_count   = 0U;
    s_last_dma_ok           = 0U;
    s_last_dma_status       = APP_PERF_LCD_DMA_STATUS_NONE;

    /* ---- 看门狗 ---- */
    s_watchdog_missing_progress_mask = 0U;
    s_watchdog_fault_flags           = 0U;

    /* ---- 运行时状态 ---- */
    s_thermal_active    = 0U;
    s_screen_off        = 0U;
    s_power_state       = POWER_STATE_ACTIVE_UI;
    s_clock_profile     = CLOCK_PROFILE_HIGH;

    /* ---- 任务栈水位 ---- */
    s_input_stack_words     = 0U;
    s_service_stack_words   = 0U;
    s_ui_stack_words        = 0U;
    s_display_stack_words   = 0U;
    s_thermal_stack_words   = 0U;
    s_power_stack_words     = 0U;

    /* ---- 统计累加器 ---- */
    app_perf_stat_reset(&s_frame_period_stats);
    app_perf_stat_reset(&s_thermal_display_age_stats);
    app_perf_stat_reset(&s_get_temp_stats);
    app_perf_stat_reset(&s_gray_stats);
    app_perf_stat_reset(&s_thermal_step_stats);
    app_perf_stat_reset(&s_lcd_dma_stats);
    app_perf_stat_reset(&s_lcd_dma_render_stats);
    app_perf_stat_reset(&s_lcd_dma_start_stats);
    app_perf_stat_reset(&s_lcd_dma_wait_stats);
    app_perf_stat_reset(&s_lcd_dma_spi_idle_stats);
    app_perf_stat_reset(&s_lcd_dma_overlay_stats);

    if (app_perf_baseline_scheduler_running() != 0U)
    {
        taskEXIT_CRITICAL();
    }
}

/**
 * @brief  查询性能采集是否启用
 * @retval 1 — 已启用；0 — 已关闭
 */
uint8_t app_perf_baseline_is_enabled(void)
{
#if APP_PERF_BASELINE_ENABLE
    return 1U;
#else
    return 0U;
#endif
}

/* =========================================================================
 *  25. 时间测量工具接口实现
 * ======================================================================= */

/**
 * @brief  获取当前 DWT 周期计数值
 * @return 当前周期计数（32 位，溢出自动回绕）
 */
uint32_t app_perf_baseline_cycle_now(void)
{
#if APP_PERF_BASELINE_ENABLE
    return DWT->CYCCNT;
#else
    return 0U;
#endif
}

/**
 * @brief  计算从 start_cycle 到现在的经过时间（微秒）
 * @param  start_cycle — 起始周期计数值
 * @return 经过的微秒数
 */
uint32_t app_perf_baseline_elapsed_us(uint32_t start_cycle)
{
#if APP_PERF_BASELINE_ENABLE
    return app_perf_cycles_to_us(app_perf_baseline_cycle_now() - start_cycle);
#else
    (void)start_cycle;
    return 0U;
#endif
}

/* =========================================================================
 *  26. 热成像帧采集记录接口实现
 * ======================================================================= */

/**
 * @brief  记录一次成功的热成像捕获
 * @note   更新帧计数、帧周期统计、温度数据和 FPS 滑动窗口。
 * @param  capture_tick_ms — 捕获时的系统 tick（ms）
 * @param  min_temp        — 最低温度
 * @param  max_temp        — 最高温度
 * @param  center_temp     — 中心温度
 */
void app_perf_baseline_record_thermal_capture_success(uint32_t capture_tick_ms,
                                                      float min_temp,
                                                      float max_temp,
                                                      float center_temp)
{
#if APP_PERF_BASELINE_ENABLE
    uint32_t elapsed_ms = 0U;

    s_thermal_capture_frames++;

    /* 计算帧周期并更新统计 */
    if (s_last_capture_tick_ms != 0U && capture_tick_ms >= s_last_capture_tick_ms)
    {
        elapsed_ms = capture_tick_ms - s_last_capture_tick_ms;
        app_perf_stat_add(&s_frame_period_stats, elapsed_ms);
    }
    s_last_capture_tick_ms = capture_tick_ms;

    /* 更新最新温度 */
    s_latest_min_temp       = min_temp;
    s_latest_max_temp       = max_temp;
    s_latest_center_temp    = center_temp;

    /* FPS 滑动窗口计算（1 秒窗口） */
    if (s_fps_window_start_ms == 0U)
    {
        s_fps_window_start_ms = capture_tick_ms;
        s_fps_window_count    = 1U;
    }
    else
    {
        s_fps_window_count++;

        if ((capture_tick_ms - s_fps_window_start_ms) >= 1000UL)
        {
            uint32_t window_ms = capture_tick_ms - s_fps_window_start_ms;

            if (window_ms != 0U)
            {
                s_thermal_fps = (s_fps_window_count * 1000UL) / window_ms;
            }

            s_fps_window_start_ms = capture_tick_ms;
            s_fps_window_count    = 0U;
        }
    }
#else
    (void)capture_tick_ms;
    (void)min_temp;
    (void)max_temp;
    (void)center_temp;
#endif
}

/**
 * @brief  记录一次热成像捕获失败
 */
void app_perf_baseline_record_thermal_capture_failure(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_capture_failures++;
#endif
}

/**
 * @brief  记录热成像显示延迟
 * @param  elapsed_ms — 从捕获到显示的延迟（ms）
 */
void app_perf_baseline_record_thermal_display_age_ms(uint32_t elapsed_ms)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_thermal_display_age_stats, elapsed_ms);
#else
    (void)elapsed_ms;
#endif
}

/* =========================================================================
 *  27. 耗时测量记录接口实现
 * ======================================================================= */

/**
 * @brief  记录 GetTemp 调用耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_get_temp_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_get_temp_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

/**
 * @brief  记录灰度转换耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_gray_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_gray_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

/**
 * @brief  记录热成像步进耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_thermal_step_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_thermal_step_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

/* =========================================================================
 *  28. LCD DMA 传输记录接口实现
 * ======================================================================= */

/**
 * @brief  记录 LCD DMA 传输结果
 * @note   更新 DMA 统计、帧计数和 FPS 滑动窗口。
 * @param  elapsed_us — 传输总耗时（微秒）
 * @param  status     — 传输状态
 */
void app_perf_baseline_record_lcd_dma_result(uint32_t elapsed_us,
                                             app_perf_lcd_dma_status_t status)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_stats, elapsed_us);
    s_last_dma_status = (uint8_t)status;
    s_last_dma_ok     = (status == APP_PERF_LCD_DMA_STATUS_OK) ? 1U : 0U;

    if (status == APP_PERF_LCD_DMA_STATUS_OK)
    {
        uint32_t display_tick_ms = power_manager_get_tick_ms();

        s_thermal_display_frames++;

        /* 显示 FPS 滑动窗口计算（1 秒窗口） */
        if (s_display_fps_window_start_ms == 0U)
        {
            s_display_fps_window_start_ms = display_tick_ms;
            s_display_fps_window_count    = 1U;
        }
        else
        {
            s_display_fps_window_count++;

            if ((display_tick_ms - s_display_fps_window_start_ms) >= 1000UL)
            {
                uint32_t window_ms = display_tick_ms - s_display_fps_window_start_ms;

                if (window_ms != 0U)
                {
                    s_thermal_display_fps = (s_display_fps_window_count * 1000UL) / window_ms;
                    s_lcd_present_fps     = s_thermal_display_fps;
                }

                s_display_fps_window_start_ms = display_tick_ms;
                s_display_fps_window_count    = 0U;
            }
        }
    }
    else
    {
        s_dma_timeout_count++;
    }
#else
    (void)elapsed_us;
    (void)status;
#endif
}

/**
 * @brief  记录 LCD DMA 渲染阶段耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_render_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_render_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

/**
 * @brief  记录 LCD DMA 启动阶段耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_start_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_start_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

/**
 * @brief  记录 LCD DMA 等待阶段耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_wait_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_wait_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

/**
 * @brief  记录 SPI 总线空闲等待耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_spi_idle_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_spi_idle_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

/**
 * @brief  记录十字光标叠加耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_overlay_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_overlay_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

/* =========================================================================
 *  29. TaskNotify 与队列记录接口实现
 * ======================================================================= */

/**
 * @brief  记录一次 TaskNotify 调用
 * @param  target — 通知目标任务
 */
void app_perf_baseline_record_task_notify(app_perf_notify_target_t target)
{
#if APP_PERF_BASELINE_ENABLE
    switch (target)
    {
    case APP_PERF_NOTIFY_INPUT:
        s_input_notify_count++;
        break;

    case APP_PERF_NOTIFY_DISPLAY:
        s_display_notify_count++;
        break;

    case APP_PERF_NOTIFY_SERVICE:
        s_service_notify_count++;
        break;

    case APP_PERF_NOTIFY_UI:
    default:
        s_ui_notify_count++;
        break;
    }
#else
    (void)target;
#endif
}

/**
 * @brief  记录按键队列溢出
 */
void app_perf_baseline_record_key_queue_drop(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_key_queue_drop_count++;
#endif
}

/**
 * @brief  记录 UI 消息队列溢出
 */
void app_perf_baseline_record_ui_msg_drop(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_ui_msg_drop_count++;
#endif
}

/**
 * @brief  记录服务队列入队失败
 */
void app_perf_baseline_record_service_queue_fail(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_service_queue_fail_count++;
#endif
}

/**
 * @brief  记录显示队列入队失败
 */
void app_perf_baseline_record_display_queue_fail(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_display_queue_fail_count++;
#endif
}

/* =========================================================================
 *  30. UART 错误记录接口实现
 * ======================================================================= */

/**
 * @brief  记录 UART 错误
 * @param  flags — UART 错误标志位
 */
void app_perf_baseline_record_uart_errors(uint32_t flags)
{
#if APP_PERF_BASELINE_ENABLE
    if (flags != 0U)
    {
        s_uart_error_count++;
        s_last_uart_error_flags = flags;
    }
#else
    (void)flags;
#endif
}

/* =========================================================================
 *  31. I2C 错误记录接口实现
 * ======================================================================= */

/**
 * @brief  记录一次 I2C 失败（通用）
 */
void app_perf_baseline_record_i2c_failure(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_failure_count++;
#endif
}

/**
 * @brief  记录一次 I2C 传输错误（分类）
 * @param  error_kind — 错误类型
 */
void app_perf_baseline_record_i2c_transport_error(app_perf_i2c_error_t error_kind)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_failure_count++;

    switch (error_kind)
    {
    case APP_PERF_I2C_ERROR_AF:
        s_i2c_af_count++;
        break;

    case APP_PERF_I2C_ERROR_BERR:
        s_i2c_berr_count++;
        break;

    case APP_PERF_I2C_ERROR_ARLO:
        s_i2c_arlo_count++;
        break;

    case APP_PERF_I2C_ERROR_OVR:
        s_i2c_ovr_count++;
        break;

    case APP_PERF_I2C_ERROR_BUSY_STUCK:
        s_i2c_busy_stuck_count++;
        break;

    case APP_PERF_I2C_ERROR_DMA_ERR:
        s_i2c_dma_err_count++;
        break;

    case APP_PERF_I2C_ERROR_TIMEOUT:
    default:
        s_i2c_timeout_count++;
        break;
    }
#else
    (void)error_kind;
#endif
}

/**
 * @brief  记录 I2C DMA 超时时的寄存器快照
 * @param  ndtr  — DMA 剩余传输计数
 * @param  state — I2C 状态机状态
 * @param  sr1   — I2C SR1 寄存器值
 * @param  sr2   — I2C SR2 寄存器值
 */
void app_perf_baseline_record_i2c_dma_timeout_snapshot(uint16_t ndtr,
                                                       uint8_t state,
                                                       uint32_t sr1,
                                                       uint32_t sr2)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_timeout_ndtr  = (uint32_t)ndtr;
    s_i2c_dma_timeout_state = (uint32_t)state;
    s_i2c_dma_timeout_sr1   = sr1;
    s_i2c_dma_timeout_sr2   = sr2;
#else
    (void)ndtr;
    (void)state;
    (void)sr1;
    (void)sr2;
#endif
}

/**
 * @brief  记录 I2C DMA 传输完成时的寄存器快照
 * @param  ndtr  — DMA 剩余传输计数
 * @param  state — I2C 状态机状态
 * @param  sr1   — I2C SR1 寄存器值
 * @param  sr2   — I2C SR2 寄存器值
 */
void app_perf_baseline_record_i2c_dma_tc_snapshot(uint16_t ndtr,
                                                  uint8_t state,
                                                  uint32_t sr1,
                                                  uint32_t sr2)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_tc_ndtr  = (uint32_t)ndtr;
    s_i2c_dma_tc_state = (uint32_t)state;
    s_i2c_dma_tc_sr1   = sr1;
    s_i2c_dma_tc_sr2   = sr2;
#else
    (void)ndtr;
    (void)state;
    (void)sr1;
    (void)sr2;
#endif
}

/**
 * @brief  记录一次 I2C 事件中断
 */
void app_perf_baseline_record_i2c_dma_ev_irq(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_ev_irq_count++;
#endif
}

/**
 * @brief  记录一次 DMA 传输完成中断
 */
void app_perf_baseline_record_i2c_dma_tc_irq(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_tc_irq_count++;
#endif
}

/**
 * @brief  记录一次 DMA 等待超时
 */
void app_perf_baseline_record_i2c_dma_wait_timeout(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_wait_timeout_count++;
#endif
}

/* =========================================================================
 *  32. I2C 轮询超时记录接口实现
 * ======================================================================= */

/**
 * @brief  记录 I2C 轮询事件超时详情
 * @param  path       — 轮询路径
 * @param  phase      — 轮询阶段
 * @param  event      — 等待的事件标志
 * @param  start_addr — 起始寄存器地址
 * @param  word_count — 传输字数
 * @param  sr1        — I2C SR1 寄存器值
 * @param  sr2        — I2C SR2 寄存器值
 */
void app_perf_baseline_record_i2c_poll_event_timeout(app_perf_i2c_poll_path_t path,
                                                     app_perf_i2c_poll_phase_t phase,
                                                     uint32_t event,
                                                     uint16_t start_addr,
                                                     uint16_t word_count,
                                                     uint32_t sr1,
                                                     uint32_t sr2)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_poll_event_timeout_count++;
    app_perf_baseline_record_i2c_timeout_classified(path, phase, start_addr);

    s_i2c_poll_timeout_path         = (uint32_t)path;
    s_i2c_poll_timeout_phase        = (uint32_t)phase;
    s_i2c_poll_timeout_event        = event;
    s_i2c_poll_timeout_sr1          = sr1;
    s_i2c_poll_timeout_sr2          = sr2;
    s_i2c_poll_timeout_start_addr   = (uint32_t)start_addr;
    s_i2c_poll_timeout_word_count   = (uint32_t)word_count;
#else
    (void)path;
    (void)phase;
    (void)event;
    (void)start_addr;
    (void)word_count;
    (void)sr1;
    (void)sr2;
#endif
}

/**
 * @brief  记录 I2C 轮询总线忙超时详情
 * @param  path       — 轮询路径
 * @param  start_addr — 起始寄存器地址
 * @param  word_count — 传输字数
 * @param  sr1        — I2C SR1 寄存器值
 * @param  sr2        — I2C SR2 寄存器值
 */
void app_perf_baseline_record_i2c_poll_busy_timeout(app_perf_i2c_poll_path_t path,
                                                    uint16_t start_addr,
                                                    uint16_t word_count,
                                                    uint32_t sr1,
                                                    uint32_t sr2)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_poll_busy_timeout_count++;
    app_perf_baseline_record_i2c_timeout_classified(path,
                                                    APP_PERF_I2C_POLL_PHASE_WAIT_BUSY,
                                                    start_addr);

    /* 按路径分类统计 */
    switch (path)
    {
    case APP_PERF_I2C_POLL_PATH_READ:
        s_i2c_busy_timeout_read_count++;
        break;

    case APP_PERF_I2C_POLL_PATH_WRITE:
        s_i2c_busy_timeout_write_count++;
        break;

    case APP_PERF_I2C_POLL_PATH_VERIFY_READ:
        s_i2c_busy_timeout_verify_count++;
        break;

    case APP_PERF_I2C_POLL_PATH_NONE:
    default:
        break;
    }

    /* 记录最近超时快照 */
    s_i2c_poll_timeout_path         = (uint32_t)path;
    s_i2c_poll_timeout_phase        = (uint32_t)APP_PERF_I2C_POLL_PHASE_WAIT_BUSY;
    s_i2c_poll_timeout_event        = 0U;
    s_i2c_poll_timeout_sr1          = sr1;
    s_i2c_poll_timeout_sr2          = sr2;
    s_i2c_poll_timeout_start_addr   = (uint32_t)start_addr;
    s_i2c_poll_timeout_word_count   = (uint32_t)word_count;
#else
    (void)path;
    (void)start_addr;
    (void)word_count;
    (void)sr1;
    (void)sr2;
#endif
}

/**
 * @brief  记录 I2C 错误中断超时详情
 * @param  path       — 轮询路径
 * @param  phase      — 轮询阶段
 * @param  start_addr — 起始寄存器地址
 * @param  word_count — 传输字数
 * @param  sr1        — I2C SR1 寄存器值
 * @param  sr2        — I2C SR2 寄存器值
 */
void app_perf_baseline_record_i2c_er_timeout(app_perf_i2c_poll_path_t path,
                                             app_perf_i2c_poll_phase_t phase,
                                             uint16_t start_addr,
                                             uint16_t word_count,
                                             uint32_t sr1,
                                             uint32_t sr2)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_er_timeout_count++;
    app_perf_baseline_record_i2c_timeout_classified(path, phase, start_addr);

    s_i2c_poll_timeout_path         = (uint32_t)path;
    s_i2c_poll_timeout_phase        = (uint32_t)phase;
    s_i2c_poll_timeout_event        = 0U;
    s_i2c_poll_timeout_sr1          = sr1;
    s_i2c_poll_timeout_sr2          = sr2;
    s_i2c_poll_timeout_start_addr   = (uint32_t)start_addr;
    s_i2c_poll_timeout_word_count   = (uint32_t)word_count;
#else
    (void)path;
    (void)phase;
    (void)start_addr;
    (void)word_count;
    (void)sr1;
    (void)sr2;
#endif
}

/**
 * @brief  记录一次 I2C 总线恢复（Bus Clear）
 * @param  source — 触发来源
 */
void app_perf_baseline_record_i2c_bus_clear(app_perf_i2c_bus_clear_source_t source)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_bus_clear_count++;

    switch (source)
    {
    case APP_PERF_I2C_BUS_CLEAR_READ:
        s_i2c_bus_clear_read_count++;
        break;

    case APP_PERF_I2C_BUS_CLEAR_WRITE:
        s_i2c_bus_clear_write_count++;
        break;

    case APP_PERF_I2C_BUS_CLEAR_DMA:
        s_i2c_bus_clear_dma_count++;
        break;

    case APP_PERF_I2C_BUS_CLEAR_BUSY_TIMEOUT:
        s_i2c_bus_clear_busy_timeout_count++;
        break;

    default:
        break;
    }
#else
    (void)source;
#endif
}

/**
 * @brief  记录一次 STOP 信号释放超时
 */
void app_perf_baseline_record_i2c_stop_release_timeout(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_stop_release_timeout_count++;
#endif
}

/* =========================================================================
 *  33. 热成像子页配对记录接口实现
 * ======================================================================= */

/**
 * @brief  记录热成像退避事件
 */
void app_perf_baseline_record_thermal_backoff(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_backoff_count++;
#endif
}

/**
 * @brief  记录一次子页配对超时
 */
void app_perf_baseline_record_thermal_pair_timeout(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_pair_timeout_count++;
    s_thermal_pair_last_result = APP_PERF_THERMAL_PAIR_RESULT_TIMEOUT;
#endif
}

/**
 * @brief  将配对超时间隔分配到对应的桶中
 * @param  gap_ms — 配对间隔（ms）
 */
static void app_perf_baseline_record_thermal_pair_timeout_gap_bucket(uint32_t gap_ms)
{
#if APP_PERF_BASELINE_ENABLE
    if (gap_ms <= 120U)
    {
        s_thermal_pair_timeout_gap_80_120_count++;
    }
    else if (gap_ms <= 160U)
    {
        s_thermal_pair_timeout_gap_120_160_count++;
    }
    else if (gap_ms <= 240U)
    {
        s_thermal_pair_timeout_gap_160_240_count++;
    }
    else
    {
        s_thermal_pair_timeout_gap_240_plus_count++;
    }
#else
    (void)gap_ms;
#endif
}

/**
 * @brief  记录等待另一个子页的详情
 * @param  subpage              — 当前子页编号
 * @param  missing_subpage      — 缺失的子页编号
 * @param  gap_ms               — 配对间隔（ms）
 * @param  same_subpage_streak  — 连续同子页计数
 */
void app_perf_baseline_record_thermal_pair_wait_other(uint8_t subpage,
                                                      uint8_t missing_subpage,
                                                      uint32_t gap_ms,
                                                      uint32_t same_subpage_streak)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_pair_wait_other_count++;
    s_thermal_pair_last_result              = APP_PERF_THERMAL_PAIR_RESULT_WAIT_OTHER;
    s_thermal_pair_last_subpage             = (uint32_t)subpage;
    s_thermal_pair_last_missing_subpage     = (uint32_t)missing_subpage;
    s_thermal_pair_last_gap_ms              = gap_ms;
    s_thermal_pair_same_subpage_streak_last = same_subpage_streak;

    if (same_subpage_streak > s_thermal_pair_same_subpage_streak_max)
    {
        s_thermal_pair_same_subpage_streak_max = same_subpage_streak;
    }
#else
    (void)subpage;
    (void)missing_subpage;
    (void)gap_ms;
    (void)same_subpage_streak;
#endif
}

/**
 * @brief  记录子页配对超时的详细信息
 * @param  subpage              — 当前子页编号
 * @param  missing_subpage      — 缺失的子页编号
 * @param  gap_ms               — 配对间隔（ms）
 * @param  same_subpage_streak  — 连续同子页计数
 * @param  get_temp_elapsed_us  — GetTemp 耗时（us）
 * @param  step_elapsed_us      — 步进耗时（us）
 */
void app_perf_baseline_record_thermal_pair_timeout_detail(uint8_t subpage,
                                                          uint8_t missing_subpage,
                                                          uint32_t gap_ms,
                                                          uint32_t same_subpage_streak,
                                                          uint32_t get_temp_elapsed_us,
                                                          uint32_t step_elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_pair_timeout_count++;
    s_thermal_pair_last_result              = APP_PERF_THERMAL_PAIR_RESULT_TIMEOUT;
    s_thermal_pair_last_subpage             = (uint32_t)subpage;
    s_thermal_pair_last_missing_subpage     = (uint32_t)missing_subpage;
    s_thermal_pair_last_gap_ms              = gap_ms;
    s_thermal_pair_timeout_gap_last_ms      = gap_ms;

    if (gap_ms > s_thermal_pair_timeout_gap_max_ms)
    {
        s_thermal_pair_timeout_gap_max_ms = gap_ms;
    }

    app_perf_baseline_record_thermal_pair_timeout_gap_bucket(gap_ms);

    s_thermal_pair_same_subpage_streak_last     = same_subpage_streak;
    s_thermal_pair_timeout_get_temp_last_us     = get_temp_elapsed_us;
    s_thermal_pair_timeout_step_last_us         = step_elapsed_us;

    if (same_subpage_streak > s_thermal_pair_same_subpage_streak_max)
    {
        s_thermal_pair_same_subpage_streak_max = same_subpage_streak;
    }
#else
    (void)subpage;
    (void)missing_subpage;
    (void)gap_ms;
    (void)same_subpage_streak;
    (void)get_temp_elapsed_us;
    (void)step_elapsed_us;
#endif
}

/**
 * @brief  记录宽限期配对成功
 * @param  subpage              — 当前子页编号
 * @param  other_subpage        — 另一个子页编号
 * @param  gap_ms               — 配对间隔（ms）
 * @param  same_subpage_streak  — 连续同子页计数
 */
void app_perf_baseline_record_thermal_pair_grace_ok(uint8_t subpage,
                                                    uint8_t other_subpage,
                                                    uint32_t gap_ms,
                                                    uint32_t same_subpage_streak)
{
#if APP_PERF_BASELINE_ENABLE
    (void)other_subpage;

    s_thermal_pair_grace_ok_count++;
    s_thermal_pair_last_result              = APP_PERF_THERMAL_PAIR_RESULT_GRACE_OK;
    s_thermal_pair_last_subpage             = (uint32_t)subpage;
    s_thermal_pair_last_missing_subpage     = 0xFFU;
    s_thermal_pair_last_gap_ms              = gap_ms;
    s_thermal_pair_compose_gap_last_ms      = gap_ms;

    if (gap_ms > s_thermal_pair_compose_gap_max_ms)
    {
        s_thermal_pair_compose_gap_max_ms = gap_ms;
    }

    s_thermal_pair_same_subpage_streak_last = same_subpage_streak;

    if (same_subpage_streak > s_thermal_pair_same_subpage_streak_max)
    {
        s_thermal_pair_same_subpage_streak_max = same_subpage_streak;
    }
#else
    (void)subpage;
    (void)other_subpage;
    (void)gap_ms;
    (void)same_subpage_streak;
#endif
}

/**
 * @brief  记录正常合成成功
 * @param  subpage              — 当前子页编号
 * @param  other_subpage        — 另一个子页编号
 * @param  gap_ms               — 配对间隔（ms）
 * @param  same_subpage_streak  — 连续同子页计数
 */
void app_perf_baseline_record_thermal_pair_compose_ok(uint8_t subpage,
                                                      uint8_t other_subpage,
                                                      uint32_t gap_ms,
                                                      uint32_t same_subpage_streak)
{
#if APP_PERF_BASELINE_ENABLE
    (void)other_subpage;

    s_thermal_pair_compose_ok_count++;
    s_thermal_pair_last_result              = APP_PERF_THERMAL_PAIR_RESULT_COMPOSE_OK;
    s_thermal_pair_last_subpage             = (uint32_t)subpage;
    s_thermal_pair_last_missing_subpage     = 0xFFU;
    s_thermal_pair_last_gap_ms              = gap_ms;
    s_thermal_pair_compose_gap_last_ms      = gap_ms;

    if (gap_ms > s_thermal_pair_compose_gap_max_ms)
    {
        s_thermal_pair_compose_gap_max_ms = gap_ms;
    }

    s_thermal_pair_same_subpage_streak_last = same_subpage_streak;

    if (same_subpage_streak > s_thermal_pair_same_subpage_streak_max)
    {
        s_thermal_pair_same_subpage_streak_max = same_subpage_streak;
    }
#else
    (void)subpage;
    (void)other_subpage;
    (void)gap_ms;
    (void)same_subpage_streak;
#endif
}

/**
 * @brief  记录一次软超时
 */
void app_perf_baseline_record_thermal_soft_timeout(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_pair_soft_timeout_count++;
#endif
}

/**
 * @brief  记录一次后备槽为空
 */
void app_perf_baseline_record_thermal_back_slot_null(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_pair_back_slot_null_count++;
#endif
}

/**
 * @brief  记录一次就绪替换
 */
void app_perf_baseline_record_thermal_ready_replace(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_ready_replace_count++;
#endif
}

/**
 * @brief  记录一次显示取消
 */
void app_perf_baseline_record_thermal_display_cancel(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_display_cancel_count++;
#endif
}

/* =========================================================================
 *  34. 热成像 3D 同步记录接口实现
 * ======================================================================= */

/** @brief  记录 3D 同步呈现尝试 */
void app_perf_baseline_record_thermal_3d_sync_present_attempt(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_sync_present_attempt_count++;
#endif
}

/** @brief  记录 3D 同步呈现成功 */
void app_perf_baseline_record_thermal_3d_sync_present_ok(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_sync_present_ok_count++;
#endif
}

/** @brief  记录 3D 同步呈现失败 */
void app_perf_baseline_record_thermal_3d_sync_present_fail(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_sync_present_fail_count++;
#endif
}

/** @brief  记录 3D 帧认领 */
void app_perf_baseline_record_thermal_3d_claim(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_claim_count++;
#endif
}

/** @brief  记录 3D 完成-成功 */
void app_perf_baseline_record_thermal_3d_done_ok(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_done_ok_count++;
#endif
}

/** @brief  记录 3D 完成-错误 */
void app_perf_baseline_record_thermal_3d_done_error(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_done_error_count++;
#endif
}

/** @brief  记录 3D 完成-取消 */
void app_perf_baseline_record_thermal_3d_done_cancel(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_done_cancel_count++;
#endif
}

/** @brief  记录 3D 等待超时 */
void app_perf_baseline_record_thermal_3d_wait_timeout(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_wait_timeout_count++;
#endif
}

/* =========================================================================
 *  35. LCD DMA 事件记录接口实现
 * ======================================================================= */

/** @brief  记录一次 LCD DMA 函数入口 */
void app_perf_baseline_record_lcd_dma_enter(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_lcd_dma_enter_count++;
#endif
}

/** @brief  记录一次 DMA 传输完成中断 */
void app_perf_baseline_record_dma_irq_tc(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_dma_irq_tc_count++;
#endif
}

/** @brief  记录一次 DMA 传输错误中断 */
void app_perf_baseline_record_dma_irq_te(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_dma_irq_te_count++;
#endif
}

/** @brief  记录一次 DMA 信号量获取 */
void app_perf_baseline_record_dma_wait_take(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_dma_wait_take_count++;
#endif
}

/* =========================================================================
 *  36. 看门狗与运行时状态接口实现
 * ======================================================================= */

/**
 * @brief  设置看门狗快照信息
 * @param  missing_progress_mask — 缺失进展的任务掩码
 * @param  fault_flags           — 故障标志
 */
void app_perf_baseline_set_watchdog_snapshot(uint32_t missing_progress_mask,
                                             uint32_t fault_flags)
{
#if APP_PERF_BASELINE_ENABLE
    s_watchdog_missing_progress_mask = missing_progress_mask;
    s_watchdog_fault_flags           = fault_flags;
#else
    (void)missing_progress_mask;
    (void)fault_flags;
#endif
}

/**
 * @brief  设置运行时状态信息
 * @param  power_state    — 当前电源状态
 * @param  clock_profile  — 当前时钟配置
 * @param  thermal_active — 热成像是否活跃
 * @param  screen_off     — 屏幕是否关闭
 */
void app_perf_baseline_set_runtime_state(power_state_t power_state,
                                         clock_profile_t clock_profile,
                                         uint8_t thermal_active,
                                         uint8_t screen_off)
{
#if APP_PERF_BASELINE_ENABLE
    s_power_state    = power_state;
    s_clock_profile  = clock_profile;
    s_thermal_active = (thermal_active != 0U) ? 1U : 0U;
    s_screen_off     = (screen_off != 0U) ? 1U : 0U;
#else
    (void)power_state;
    (void)clock_profile;
    (void)thermal_active;
    (void)screen_off;
#endif
}

/* =========================================================================
 *  37. 任务栈水位与快照导出接口实现
 * ======================================================================= */

/**
 * @brief  刷新所有任务的栈水位信息
 * @note   仅在调度器运行中且句柄有效时采样。
 * @param  input_task    — 输入任务句柄
 * @param  service_task  — 服务任务句柄
 * @param  ui_task       — UI 任务句柄
 * @param  display_task  — 显示任务句柄
 * @param  thermal_task  — 热成像任务句柄
 * @param  power_task    — 电源任务句柄
 */
void app_perf_baseline_refresh_task_stacks(TaskHandle_t input_task,
                                           TaskHandle_t service_task,
                                           TaskHandle_t ui_task,
                                           TaskHandle_t display_task,
                                           TaskHandle_t thermal_task,
                                           TaskHandle_t power_task)
{
#if APP_PERF_BASELINE_ENABLE
    if (app_perf_baseline_scheduler_running() == 0U)
    {
        return;
    }

    if (input_task != 0)
    {
        s_input_stack_words = uxTaskGetStackHighWaterMark(input_task);
    }

    if (service_task != 0)
    {
        s_service_stack_words = uxTaskGetStackHighWaterMark(service_task);
    }

    if (ui_task != 0)
    {
        s_ui_stack_words = uxTaskGetStackHighWaterMark(ui_task);
    }

    if (display_task != 0)
    {
        s_display_stack_words = uxTaskGetStackHighWaterMark(display_task);
    }

    if (thermal_task != 0)
    {
        s_thermal_stack_words = uxTaskGetStackHighWaterMark(thermal_task);
    }

    if (power_task != 0)
    {
        s_power_stack_words = uxTaskGetStackHighWaterMark(power_task);
    }
#else
    (void)input_task;
    (void)service_task;
    (void)ui_task;
    (void)display_task;
    (void)thermal_task;
    (void)power_task;
#endif
}

/**
 * @brief  导出性能基线快照
 * @note   在临界区内批量拷贝所有 volatile 计数器到快照结构体，
 *         保证快照数据的一致性。
 * @param  snapshot — 输出：快照结构体指针
 */
void app_perf_baseline_get_snapshot(app_perf_baseline_snapshot_t *snapshot)
{
    if (snapshot == 0)
    {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->enabled = app_perf_baseline_is_enabled();

#if APP_PERF_BASELINE_ENABLE
    if (app_perf_baseline_scheduler_running() != 0U)
    {
        taskENTER_CRITICAL();
    }

    /* ---- 通用状态 ---- */
    snapshot->last_dma_ok       = s_last_dma_ok;
    snapshot->last_dma_status   = s_last_dma_status;
    snapshot->thermal_active    = s_thermal_active;
    snapshot->screen_off        = s_screen_off;
    snapshot->power_state       = s_power_state;
    snapshot->clock_profile     = s_clock_profile;

    /* ---- 热成像帧率与捕获 ---- */
    snapshot->thermal_capture_frames    = s_thermal_capture_frames;
    snapshot->thermal_display_frames    = s_thermal_display_frames;
    snapshot->thermal_capture_failures  = s_thermal_capture_failures;
    snapshot->thermal_fps               = s_thermal_fps;
    snapshot->thermal_display_fps       = s_thermal_display_fps;
    snapshot->lcd_present_fps           = s_lcd_present_fps;
    snapshot->thermal_display_age_samples   = s_thermal_display_age_stats.count;
    snapshot->thermal_display_age_last_ms   = s_thermal_display_age_stats.last;
    snapshot->thermal_display_age_max_ms    = s_thermal_display_age_stats.max;
    snapshot->thermal_display_age_avg_ms    = app_perf_stat_avg(&s_thermal_display_age_stats);

    /* ---- 帧周期统计 ---- */
    snapshot->thermal_frame_period_samples  = s_frame_period_stats.count;
    snapshot->thermal_frame_period_last_ms  = s_frame_period_stats.last;
    snapshot->thermal_frame_period_min_ms   = s_frame_period_stats.min;
    snapshot->thermal_frame_period_max_ms   = s_frame_period_stats.max;
    snapshot->thermal_frame_period_avg_ms   = app_perf_stat_avg(&s_frame_period_stats);

    /* ---- GetTemp 耗时 ---- */
    snapshot->get_temp_samples  = s_get_temp_stats.count;
    snapshot->get_temp_last_us  = s_get_temp_stats.last;
    snapshot->get_temp_max_us   = s_get_temp_stats.max;
    snapshot->get_temp_avg_us   = app_perf_stat_avg(&s_get_temp_stats);

    /* ---- 灰度转换耗时 ---- */
    snapshot->gray_samples  = s_gray_stats.count;
    snapshot->gray_last_us  = s_gray_stats.last;
    snapshot->gray_max_us   = s_gray_stats.max;
    snapshot->gray_avg_us   = app_perf_stat_avg(&s_gray_stats);

    /* ---- 热成像步进耗时 ---- */
    snapshot->thermal_step_samples  = s_thermal_step_stats.count;
    snapshot->thermal_step_last_us  = s_thermal_step_stats.last;
    snapshot->thermal_step_max_us   = s_thermal_step_stats.max;
    snapshot->thermal_step_avg_us   = app_perf_stat_avg(&s_thermal_step_stats);

    /* ---- LCD DMA 耗时统计 ---- */
    snapshot->lcd_dma_samples       = s_lcd_dma_stats.count;
    snapshot->lcd_dma_last_us       = s_lcd_dma_stats.last;
    snapshot->lcd_dma_max_us        = s_lcd_dma_stats.max;
    snapshot->lcd_dma_avg_us        = app_perf_stat_avg(&s_lcd_dma_stats);
    snapshot->lcd_dma_render_samples    = s_lcd_dma_render_stats.count;
    snapshot->lcd_dma_render_last_us    = s_lcd_dma_render_stats.last;
    snapshot->lcd_dma_render_max_us     = s_lcd_dma_render_stats.max;
    snapshot->lcd_dma_render_avg_us     = app_perf_stat_avg(&s_lcd_dma_render_stats);
    snapshot->lcd_dma_start_samples     = s_lcd_dma_start_stats.count;
    snapshot->lcd_dma_start_last_us     = s_lcd_dma_start_stats.last;
    snapshot->lcd_dma_start_max_us      = s_lcd_dma_start_stats.max;
    snapshot->lcd_dma_start_avg_us      = app_perf_stat_avg(&s_lcd_dma_start_stats);
    snapshot->lcd_dma_wait_samples      = s_lcd_dma_wait_stats.count;
    snapshot->lcd_dma_wait_last_us      = s_lcd_dma_wait_stats.last;
    snapshot->lcd_dma_wait_max_us       = s_lcd_dma_wait_stats.max;
    snapshot->lcd_dma_wait_avg_us       = app_perf_stat_avg(&s_lcd_dma_wait_stats);
    snapshot->lcd_dma_spi_idle_samples  = s_lcd_dma_spi_idle_stats.count;
    snapshot->lcd_dma_spi_idle_last_us  = s_lcd_dma_spi_idle_stats.last;
    snapshot->lcd_dma_spi_idle_max_us   = s_lcd_dma_spi_idle_stats.max;
    snapshot->lcd_dma_spi_idle_avg_us   = app_perf_stat_avg(&s_lcd_dma_spi_idle_stats);
    snapshot->lcd_dma_overlay_samples   = s_lcd_dma_overlay_stats.count;
    snapshot->lcd_dma_overlay_last_us   = s_lcd_dma_overlay_stats.last;
    snapshot->lcd_dma_overlay_max_us    = s_lcd_dma_overlay_stats.max;
    snapshot->lcd_dma_overlay_avg_us    = app_perf_stat_avg(&s_lcd_dma_overlay_stats);

    /* ---- 最新温度 ---- */
    snapshot->latest_min_temp       = s_latest_min_temp;
    snapshot->latest_max_temp       = s_latest_max_temp;
    snapshot->latest_center_temp    = s_latest_center_temp;

    /* ---- 队列溢出 ---- */
    snapshot->key_queue_drop_count      = s_key_queue_drop_count;
    snapshot->ui_msg_drop_count         = s_ui_msg_drop_count;
    snapshot->service_queue_fail_count  = s_service_queue_fail_count;
    snapshot->display_queue_fail_count  = s_display_queue_fail_count;

    /* ---- TaskNotify ---- */
    snapshot->input_notify_count    = s_input_notify_count;
    snapshot->ui_notify_count       = s_ui_notify_count;
    snapshot->service_notify_count  = s_service_notify_count;
    snapshot->display_notify_count  = s_display_notify_count;

    /* ---- UART 错误 ---- */
    snapshot->uart_error_count          = s_uart_error_count;
    snapshot->last_uart_error_flags     = s_last_uart_error_flags;

    /* ---- I2C 错误分类 ---- */
    snapshot->i2c_failure_count     = s_i2c_failure_count;
    snapshot->i2c_af_count          = s_i2c_af_count;
    snapshot->i2c_berr_count        = s_i2c_berr_count;
    snapshot->i2c_arlo_count        = s_i2c_arlo_count;
    snapshot->i2c_ovr_count         = s_i2c_ovr_count;
    snapshot->i2c_timeout_count     = s_i2c_timeout_count;
    snapshot->i2c_busy_stuck_count  = s_i2c_busy_stuck_count;
    snapshot->i2c_dma_err_count     = s_i2c_dma_err_count;

    /* ---- I2C DMA 快照 ---- */
    snapshot->i2c_dma_timeout_ndtr  = s_i2c_dma_timeout_ndtr;
    snapshot->i2c_dma_timeout_state = s_i2c_dma_timeout_state;
    snapshot->i2c_dma_timeout_sr1   = s_i2c_dma_timeout_sr1;
    snapshot->i2c_dma_timeout_sr2   = s_i2c_dma_timeout_sr2;
    snapshot->i2c_dma_tc_ndtr       = s_i2c_dma_tc_ndtr;
    snapshot->i2c_dma_tc_state      = s_i2c_dma_tc_state;
    snapshot->i2c_dma_tc_sr1        = s_i2c_dma_tc_sr1;
    snapshot->i2c_dma_tc_sr2        = s_i2c_dma_tc_sr2;

    /* ---- I2C DMA 中断与超时 ---- */
    snapshot->i2c_dma_ev_irq_count      = s_i2c_dma_ev_irq_count;
    snapshot->i2c_dma_tc_irq_count      = s_i2c_dma_tc_irq_count;
    snapshot->i2c_dma_wait_timeout_count = s_i2c_dma_wait_timeout_count;

    /* ---- I2C 轮询超时详情 ---- */
    snapshot->i2c_poll_event_timeout_count  = s_i2c_poll_event_timeout_count;
    snapshot->i2c_poll_busy_timeout_count   = s_i2c_poll_busy_timeout_count;
    snapshot->i2c_er_timeout_count          = s_i2c_er_timeout_count;
    snapshot->i2c_poll_timeout_path         = s_i2c_poll_timeout_path;
    snapshot->i2c_poll_timeout_phase        = s_i2c_poll_timeout_phase;
    snapshot->i2c_poll_timeout_event        = s_i2c_poll_timeout_event;
    snapshot->i2c_poll_timeout_sr1          = s_i2c_poll_timeout_sr1;
    snapshot->i2c_poll_timeout_sr2          = s_i2c_poll_timeout_sr2;
    snapshot->i2c_poll_timeout_start_addr   = s_i2c_poll_timeout_start_addr;
    snapshot->i2c_poll_timeout_word_count   = s_i2c_poll_timeout_word_count;

    /* ---- I2C 轮询超时分类 ---- */
    snapshot->i2c_poll_timeout_read_count       = s_i2c_poll_timeout_read_count;
    snapshot->i2c_poll_timeout_write_count      = s_i2c_poll_timeout_write_count;
    snapshot->i2c_poll_timeout_verify_count     = s_i2c_poll_timeout_verify_count;
    snapshot->i2c_addrw_timeout_read_count      = s_i2c_addrw_timeout_read_count;
    snapshot->i2c_addrw_timeout_write_count     = s_i2c_addrw_timeout_write_count;
    snapshot->i2c_addr_8000_timeout_read_count  = s_i2c_addr_8000_timeout_read_count;
    snapshot->i2c_addr_8000_timeout_write_count = s_i2c_addr_8000_timeout_write_count;
    snapshot->i2c_addr_800d_timeout_read_count  = s_i2c_addr_800d_timeout_read_count;
    snapshot->i2c_addr_800d_timeout_write_count = s_i2c_addr_800d_timeout_write_count;
    snapshot->i2c_r8000_addrw_timeout_count     = s_i2c_r8000_addrw_timeout_count;
    snapshot->i2c_w8000_addrw_timeout_count     = s_i2c_w8000_addrw_timeout_count;
    snapshot->i2c_r800d_addrw_timeout_count     = s_i2c_r800d_addrw_timeout_count;
    snapshot->i2c_r800d_rx_timeout_count        = s_i2c_r800d_rx_timeout_count;

    /* ---- I2C 总线忙超时分类 ---- */
    snapshot->i2c_busy_timeout_read_count   = s_i2c_busy_timeout_read_count;
    snapshot->i2c_busy_timeout_write_count  = s_i2c_busy_timeout_write_count;
    snapshot->i2c_busy_timeout_verify_count = s_i2c_busy_timeout_verify_count;

    /* ---- I2C 总线恢复 ---- */
    snapshot->i2c_bus_clear_count               = s_i2c_bus_clear_count;
    snapshot->i2c_stop_release_timeout_count    = s_i2c_stop_release_timeout_count;
    snapshot->i2c_bus_clear_read_count          = s_i2c_bus_clear_read_count;
    snapshot->i2c_bus_clear_write_count         = s_i2c_bus_clear_write_count;
    snapshot->i2c_bus_clear_dma_count           = s_i2c_bus_clear_dma_count;
    snapshot->i2c_bus_clear_busy_timeout_count  = s_i2c_bus_clear_busy_timeout_count;

    /* ---- DMA 与热成像退避 ---- */
    snapshot->dma_timeout_count     = s_dma_timeout_count;
    snapshot->thermal_backoff_count = s_thermal_backoff_count;

    /* ---- 热成像子页配对 ---- */
    snapshot->thermal_pair_timeout_count                = s_thermal_pair_timeout_count;
    snapshot->thermal_pair_grace_ok_count               = s_thermal_pair_grace_ok_count;
    snapshot->thermal_pair_compose_ok_count             = s_thermal_pair_compose_ok_count;
    snapshot->thermal_pair_wait_other_count             = s_thermal_pair_wait_other_count;
    snapshot->thermal_pair_last_result                  = s_thermal_pair_last_result;
    snapshot->thermal_pair_last_subpage                 = s_thermal_pair_last_subpage;
    snapshot->thermal_pair_last_missing_subpage         = s_thermal_pair_last_missing_subpage;
    snapshot->thermal_pair_last_gap_ms                  = s_thermal_pair_last_gap_ms;
    snapshot->thermal_pair_timeout_gap_last_ms          = s_thermal_pair_timeout_gap_last_ms;
    snapshot->thermal_pair_timeout_gap_max_ms           = s_thermal_pair_timeout_gap_max_ms;
    snapshot->thermal_pair_timeout_gap_80_120_count     = s_thermal_pair_timeout_gap_80_120_count;
    snapshot->thermal_pair_timeout_gap_120_160_count    = s_thermal_pair_timeout_gap_120_160_count;
    snapshot->thermal_pair_timeout_gap_160_240_count    = s_thermal_pair_timeout_gap_160_240_count;
    snapshot->thermal_pair_timeout_gap_240_plus_count   = s_thermal_pair_timeout_gap_240_plus_count;
    snapshot->thermal_pair_compose_gap_last_ms          = s_thermal_pair_compose_gap_last_ms;
    snapshot->thermal_pair_compose_gap_max_ms           = s_thermal_pair_compose_gap_max_ms;
    snapshot->thermal_pair_same_subpage_streak_last     = s_thermal_pair_same_subpage_streak_last;
    snapshot->thermal_pair_same_subpage_streak_max      = s_thermal_pair_same_subpage_streak_max;
    snapshot->thermal_pair_timeout_get_temp_last_us     = s_thermal_pair_timeout_get_temp_last_us;
    snapshot->thermal_pair_timeout_step_last_us         = s_thermal_pair_timeout_step_last_us;
    snapshot->thermal_pair_soft_timeout_count           = s_thermal_pair_soft_timeout_count;
    snapshot->thermal_pair_back_slot_null_count         = s_thermal_pair_back_slot_null_count;
    snapshot->thermal_ready_replace_count               = s_thermal_ready_replace_count;
    snapshot->thermal_display_cancel_count              = s_thermal_display_cancel_count;

    /* ---- 热成像 3D 同步 ---- */
    snapshot->thermal_3d_sync_present_attempt_count = s_thermal_3d_sync_present_attempt_count;
    snapshot->thermal_3d_sync_present_ok_count      = s_thermal_3d_sync_present_ok_count;
    snapshot->thermal_3d_sync_present_fail_count    = s_thermal_3d_sync_present_fail_count;
    snapshot->thermal_3d_claim_count                = s_thermal_3d_claim_count;
    snapshot->thermal_3d_done_ok_count              = s_thermal_3d_done_ok_count;
    snapshot->thermal_3d_done_error_count           = s_thermal_3d_done_error_count;
    snapshot->thermal_3d_done_cancel_count          = s_thermal_3d_done_cancel_count;
    snapshot->thermal_3d_wait_timeout_count         = s_thermal_3d_wait_timeout_count;

    /* ---- LCD DMA 事件 ---- */
    snapshot->lcd_dma_enter_count   = s_lcd_dma_enter_count;
    snapshot->dma_irq_tc_count      = s_dma_irq_tc_count;
    snapshot->dma_irq_te_count      = s_dma_irq_te_count;
    snapshot->dma_wait_take_count   = s_dma_wait_take_count;

    /* ---- 看门狗 ---- */
    snapshot->watchdog_missing_progress_mask = s_watchdog_missing_progress_mask;
    snapshot->watchdog_fault_flags           = s_watchdog_fault_flags;

    /* ---- 任务栈水位 ---- */
    snapshot->input_stack_words     = s_input_stack_words;
    snapshot->service_stack_words   = s_service_stack_words;
    snapshot->ui_stack_words        = s_ui_stack_words;
    snapshot->display_stack_words   = s_display_stack_words;
    snapshot->thermal_stack_words   = s_thermal_stack_words;
    snapshot->power_stack_words     = s_power_stack_words;

    if (app_perf_baseline_scheduler_running() != 0U)
    {
        taskEXIT_CRITICAL();
    }
#endif
}
