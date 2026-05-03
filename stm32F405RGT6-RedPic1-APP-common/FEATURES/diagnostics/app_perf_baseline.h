/**
 * @file    app_perf_baseline.h
 * @brief   性能基线诊断模块 —— 公共接口
 * @note    本模块提供系统运行时性能数据采集与快照导出功能。
 *          通过编译宏 APP_PERF_BASELINE_ENABLE 可整体开关性能采集，
 *          关闭后所有 record 函数编译为空操作，不产生运行时开销。
 *
 * @par 采集维度
 *      - 热成像帧率 / 捕获失败计数
 *      - LCD DMA 传输耗时（渲染、启动、等待、SPI 空闲、叠加）
 *      - I2C 错误分类计数（AF / BERR / ARLO / OVR / TIMEOUT / BUSY_STUCK / DMA_ERR）
 *      - I2C 轮询超时分类（按路径、阶段、寄存器地址细分）
 *      - I2C 总线恢复（Bus Clear）来源统计
 *      - 热成像子页配对结果（等待 / 超时 / 宽限成功 / 合成成功）
 *      - 3D 同步呈现统计
 *      - 队列溢出 / TaskNotify 次数
 *      - 看门狗状态 / 任务栈水位
 *
 * @par 线程安全
 *      所有 record 函数可在任意任务或中断中调用，内部使用 volatile 变量保证可见性。
 *      get_snapshot 在临界区内批量拷贝，保证快照一致性。
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef APP_PERF_BASELINE_H
#define APP_PERF_BASELINE_H

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include <stdint.h>

#include "FreeRTOS.h"
#include "clock_profile_service.h"
#include "power_manager.h"
#include "task.h"

/* =========================================================================
 *  2. 编译开关
 * ======================================================================= */

/**
 * @brief 性能基线采集总开关
 * @note  设为 0 可完全关闭性能采集，所有 record 函数编译为空操作。
 */
#ifndef APP_PERF_BASELINE_ENABLE
    #define APP_PERF_BASELINE_ENABLE    1
#endif

/* =========================================================================
 *  3. 枚举类型定义
 * ======================================================================= */

/**
 * @brief TaskNotify 目标枚举
 * @note  用于区分不同任务的 TaskNotify 调用来源。
 */
typedef enum
{
    APP_PERF_NOTIFY_INPUT = 0,      /**< 输入任务                        */
    APP_PERF_NOTIFY_UI,             /**< UI 任务                         */
    APP_PERF_NOTIFY_SERVICE,        /**< 服务任务                        */
    APP_PERF_NOTIFY_DISPLAY         /**< 显示任务                        */
} app_perf_notify_target_t;

/**
 * @brief LCD DMA 传输状态枚举
 * @note  记录最近一次 DMA 传输的执行结果。
 */
typedef enum
{
    APP_PERF_LCD_DMA_STATUS_NONE = 0,   /**< 无传输（初始状态）          */
    APP_PERF_LCD_DMA_STATUS_OK,         /**< 传输成功                    */
    APP_PERF_LCD_DMA_STATUS_TIMEOUT,    /**< 传输超时                    */
    APP_PERF_LCD_DMA_STATUS_ERROR        /**< 传输错误                    */
} app_perf_lcd_dma_status_t;

/**
 * @brief I2C 传输错误类型枚举
 * @note  对应 STM32 I2C 外设的各类错误标志位。
 */
typedef enum
{
    APP_PERF_I2C_ERROR_AF = 0,          /**< 应答失败（Acknowledge Fail） */
    APP_PERF_I2C_ERROR_BERR,            /**< 总线错误（Bus Error）        */
    APP_PERF_I2C_ERROR_ARLO,            /**< 仲裁丢失（Arbitration Lost） */
    APP_PERF_I2C_ERROR_OVR,             /**< 溢出错误（Overrun/Underrun） */
    APP_PERF_I2C_ERROR_TIMEOUT,         /**< 超时错误                    */
    APP_PERF_I2C_ERROR_BUSY_STUCK,      /**< 总线忙卡死                  */
    APP_PERF_I2C_ERROR_DMA_ERR          /**< DMA 传输错误                */
} app_perf_i2c_error_t;

/**
 * @brief I2C 轮询路径枚举
 * @note  标识触发超时的 I2C 操作类型。
 */
typedef enum
{
    APP_PERF_I2C_POLL_PATH_NONE = 0,    /**< 无效路径                    */
    APP_PERF_I2C_POLL_PATH_READ,        /**< 读操作                      */
    APP_PERF_I2C_POLL_PATH_WRITE,       /**< 写操作                      */
    APP_PERF_I2C_POLL_PATH_VERIFY_READ  /**< 校验读操作                  */
} app_perf_i2c_poll_path_t;

/**
 * @brief I2C 轮询阶段枚举
 * @note  标识超时发生时 I2C 状态机所处的具体阶段。
 */
typedef enum
{
    APP_PERF_I2C_POLL_PHASE_NONE = 0,           /**< 无阶段              */
    APP_PERF_I2C_POLL_PHASE_WAIT_BUSY,          /**< 等待总线空闲        */
    APP_PERF_I2C_POLL_PHASE_START,              /**< 发送起始条件        */
    APP_PERF_I2C_POLL_PHASE_ADDR_W,            /**< 发送写地址          */
    APP_PERF_I2C_POLL_PHASE_REG_HI,            /**< 发送寄存器高字节    */
    APP_PERF_I2C_POLL_PHASE_REG_LO,            /**< 发送寄存器低字节    */
    APP_PERF_I2C_POLL_PHASE_RESTART,           /**< 发送重复起始条件    */
    APP_PERF_I2C_POLL_PHASE_ADDR_R,            /**< 发送读地址          */
    APP_PERF_I2C_POLL_PHASE_BYTE_RECEIVED,     /**< 接收字节完成        */
    APP_PERF_I2C_POLL_PHASE_BYTE_TRANSMITTED   /**< 发送字节完成        */
} app_perf_i2c_poll_phase_t;

/**
 * @brief I2C 总线恢复（Bus Clear）来源枚举
 * @note  标识触发 Bus Clear 的操作上下文。
 */
typedef enum
{
    APP_PERF_I2C_BUS_CLEAR_READ = 0,           /**< 读操作触发          */
    APP_PERF_I2C_BUS_CLEAR_WRITE,              /**< 写操作触发          */
    APP_PERF_I2C_BUS_CLEAR_DMA,                /**< DMA 操作触发        */
    APP_PERF_I2C_BUS_CLEAR_BUSY_TIMEOUT        /**< 总线忙超时触发      */
} app_perf_i2c_bus_clear_source_t;

/**
 * @brief 热成像子页配对结果枚举
 * @note  MLX90640 需要两个子页数据才能合成完整帧，
 *        此枚举记录每次配对尝试的结果。
 */
typedef enum
{
    APP_PERF_THERMAL_PAIR_RESULT_NONE = 0,      /**< 无结果（初始状态）  */
    APP_PERF_THERMAL_PAIR_RESULT_WAIT_OTHER,    /**< 等待另一个子页      */
    APP_PERF_THERMAL_PAIR_RESULT_TIMEOUT,       /**< 配对超时            */
    APP_PERF_THERMAL_PAIR_RESULT_GRACE_OK,      /**< 宽限期内配对成功    */
    APP_PERF_THERMAL_PAIR_RESULT_COMPOSE_OK     /**< 正常合成成功        */
} app_perf_thermal_pair_result_t;

/* =========================================================================
 *  4. 快照结构体定义
 * ======================================================================= */

/**
 * @brief 性能基线快照结构体
 * @note  包含系统运行时所有性能指标的只读副本，
 *        由 app_perf_baseline_get_snapshot() 在临界区内批量填充。
 */
typedef struct
{
    /* ---- 通用状态 ---- */
    uint8_t             enabled;            /**< 性能采集是否启用            */
    uint8_t             last_dma_ok;        /**< 最近一次 DMA 传输是否成功   */
    uint8_t             last_dma_status;    /**< 最近一次 DMA 状态码         */
    uint8_t             thermal_active;     /**< 热成像采集是否活跃          */
    uint8_t             screen_off;         /**< 屏幕是否关闭                */
    power_state_t       power_state;        /**< 当前电源状态                */
    clock_profile_t     clock_profile;      /**< 当前时钟配置                */

    /* ---- 热成像帧率与捕获统计 ---- */
    uint32_t thermal_capture_frames;        /**< 热成像捕获帧总数            */
    uint32_t thermal_display_frames;        /**< 热成像显示帧总数            */
    uint32_t thermal_capture_failures;      /**< 热成像捕获失败次数          */
    uint32_t thermal_fps;                   /**< 热成像采集帧率（帧/秒）     */
    uint32_t thermal_display_fps;           /**< 热成像显示帧率（帧/秒）     */
    uint32_t lcd_present_fps;               /**< LCD 呈现帧率（帧/秒）       */
    uint32_t thermal_display_age_samples;   /**< 显示延迟采样数              */
    uint32_t thermal_display_age_last_ms;   /**< 最近一次显示延迟（ms）      */
    uint32_t thermal_display_age_max_ms;    /**< 最大显示延迟（ms）          */
    uint32_t thermal_display_age_avg_ms;    /**< 平均显示延迟（ms）          */

    /* ---- 热成像帧周期统计 ---- */
    uint32_t thermal_frame_period_samples;  /**< 帧周期采样数                */
    uint32_t thermal_frame_period_last_ms;  /**< 最近一次帧周期（ms）        */
    uint32_t thermal_frame_period_min_ms;   /**< 最小帧周期（ms）            */
    uint32_t thermal_frame_period_max_ms;   /**< 最大帧周期（ms）            */
    uint32_t thermal_frame_period_avg_ms;   /**< 平均帧周期（ms）            */

    /* ---- GetTemp 耗时统计 ---- */
    uint32_t get_temp_samples;              /**< GetTemp 采样数              */
    uint32_t get_temp_last_us;              /**< 最近一次 GetTemp 耗时（us） */
    uint32_t get_temp_max_us;               /**< 最大 GetTemp 耗时（us）     */
    uint32_t get_temp_avg_us;               /**< 平均 GetTemp 耗时（us）     */

    /* ---- 灰度转换耗时统计 ---- */
    uint32_t gray_samples;                  /**< 灰度转换采样数              */
    uint32_t gray_last_us;                  /**< 最近一次灰度转换耗时（us）  */
    uint32_t gray_max_us;                   /**< 最大灰度转换耗时（us）      */
    uint32_t gray_avg_us;                   /**< 平均灰度转换耗时（us）      */

    /* ---- 热成像步进耗时统计 ---- */
    uint32_t thermal_step_samples;          /**< 热成像步进采样数            */
    uint32_t thermal_step_last_us;          /**< 最近一次步进耗时（us）      */
    uint32_t thermal_step_max_us;           /**< 最大步进耗时（us）          */
    uint32_t thermal_step_avg_us;           /**< 平均步进耗时（us）          */

    /* ---- LCD DMA 传输耗时统计 ---- */
    uint32_t lcd_dma_samples;               /**< DMA 总耗时采样数            */
    uint32_t lcd_dma_last_us;               /**< 最近一次 DMA 总耗时（us）   */
    uint32_t lcd_dma_max_us;                /**< 最大 DMA 总耗时（us）       */
    uint32_t lcd_dma_avg_us;                /**< 平均 DMA 总耗时（us）       */
    uint32_t lcd_dma_render_samples;        /**< DMA 渲染阶段采样数          */
    uint32_t lcd_dma_render_last_us;        /**< 最近一次渲染耗时（us）      */
    uint32_t lcd_dma_render_max_us;         /**< 最大渲染耗时（us）          */
    uint32_t lcd_dma_render_avg_us;         /**< 平均渲染耗时（us）          */
    uint32_t lcd_dma_start_samples;         /**< DMA 启动阶段采样数          */
    uint32_t lcd_dma_start_last_us;         /**< 最近一次启动耗时（us）      */
    uint32_t lcd_dma_start_max_us;          /**< 最大启动耗时（us）          */
    uint32_t lcd_dma_start_avg_us;          /**< 平均启动耗时（us）          */
    uint32_t lcd_dma_wait_samples;          /**< DMA 等待阶段采样数          */
    uint32_t lcd_dma_wait_last_us;          /**< 最近一次等待耗时（us）      */
    uint32_t lcd_dma_wait_max_us;           /**< 最大等待耗时（us）          */
    uint32_t lcd_dma_wait_avg_us;           /**< 平均等待耗时（us）          */
    uint32_t lcd_dma_spi_idle_samples;      /**< SPI 空闲等待采样数          */
    uint32_t lcd_dma_spi_idle_last_us;      /**< 最近一次 SPI 空闲（us）     */
    uint32_t lcd_dma_spi_idle_max_us;       /**< 最大 SPI 空闲等待（us）     */
    uint32_t lcd_dma_spi_idle_avg_us;       /**< 平均 SPI 空闲等待（us）     */
    uint32_t lcd_dma_overlay_samples;       /**< 十字光标叠加采样数          */
    uint32_t lcd_dma_overlay_last_us;       /**< 最近一次叠加耗时（us）      */
    uint32_t lcd_dma_overlay_max_us;        /**< 最大叠加耗时（us）          */
    uint32_t lcd_dma_overlay_avg_us;        /**< 平均叠加耗时（us）          */

    /* ---- 最新温度数据 ---- */
    float    latest_min_temp;               /**< 最近一次最低温度（℃）       */
    float    latest_max_temp;               /**< 最近一次最高温度（℃）       */
    float    latest_center_temp;            /**< 最近一次中心温度（℃）       */

    /* ---- 队列溢出计数 ---- */
    uint32_t key_queue_drop_count;          /**< 按键队列溢出次数            */
    uint32_t ui_msg_drop_count;             /**< UI 消息队列溢出次数         */
    uint32_t service_queue_fail_count;      /**< 服务队列入队失败次数        */
    uint32_t display_queue_fail_count;      /**< 显示队列入队失败次数        */

    /* ---- TaskNotify 计数 ---- */
    uint32_t input_notify_count;            /**< 输入任务 Notify 次数        */
    uint32_t ui_notify_count;               /**< UI 任务 Notify 次数         */
    uint32_t service_notify_count;          /**< 服务任务 Notify 次数        */
    uint32_t display_notify_count;          /**< 显示任务 Notify 次数        */

    /* ---- UART 错误计数 ---- */
    uint32_t uart_error_count;              /**< UART 错误总次数             */
    uint32_t last_uart_error_flags;         /**< 最近一次 UART 错误标志      */

    /* ---- I2C 错误分类计数 ---- */
    uint32_t i2c_failure_count;             /**< I2C 失败总次数              */
    uint32_t i2c_af_count;                  /**< 应答失败次数                */
    uint32_t i2c_berr_count;                /**< 总线错误次数                */
    uint32_t i2c_arlo_count;                /**< 仲裁丢失次数                */
    uint32_t i2c_ovr_count;                 /**< 溢出错误次数                */
    uint32_t i2c_timeout_count;             /**< I2C 超时次数                */
    uint32_t i2c_busy_stuck_count;          /**< 总线忙卡死次数              */
    uint32_t i2c_dma_err_count;             /**< I2C DMA 错误次数            */

    /* ---- I2C DMA 快照（超时时） ---- */
    uint32_t i2c_dma_timeout_ndtr;          /**< 超时时 DMA 剩余传输计数     */
    uint32_t i2c_dma_timeout_state;         /**< 超时时 I2C 状态机状态       */
    uint32_t i2c_dma_timeout_sr1;           /**< 超时时 I2C SR1 寄存器       */
    uint32_t i2c_dma_timeout_sr2;           /**< 超时时 I2C SR2 寄存器       */

    /* ---- I2C DMA 快照（传输完成时） ---- */
    uint32_t i2c_dma_tc_ndtr;               /**< 完成时 DMA 剩余传输计数     */
    uint32_t i2c_dma_tc_state;              /**< 完成时 I2C 状态机状态       */
    uint32_t i2c_dma_tc_sr1;                /**< 完成时 I2C SR1 寄存器       */
    uint32_t i2c_dma_tc_sr2;                /**< 完成时 I2C SR2 寄存器       */

    /* ---- I2C DMA 中断与超时计数 ---- */
    uint32_t i2c_dma_ev_irq_count;          /**< I2C 事件中断次数            */
    uint32_t i2c_dma_tc_irq_count;          /**< DMA 传输完成中断次数        */
    uint32_t i2c_dma_wait_timeout_count;    /**< DMA 等待超时次数            */

    /* ---- I2C 轮询超时详情 ---- */
    uint32_t i2c_poll_event_timeout_count;  /**< 轮询事件超时次数            */
    uint32_t i2c_poll_busy_timeout_count;   /**< 轮询总线忙超时次数          */
    uint32_t i2c_er_timeout_count;          /**< 错误中断超时次数            */
    uint32_t i2c_poll_timeout_path;         /**< 最近超时的轮询路径          */
    uint32_t i2c_poll_timeout_phase;        /**< 最近超时的轮询阶段          */
    uint32_t i2c_poll_timeout_event;        /**< 最近超时的事件标志          */
    uint32_t i2c_poll_timeout_sr1;          /**< 最近超时时 SR1              */
    uint32_t i2c_poll_timeout_sr2;          /**< 最近超时时 SR2              */
    uint32_t i2c_poll_timeout_start_addr;   /**< 最近超时的起始地址          */
    uint32_t i2c_poll_timeout_word_count;   /**< 最近超时的字数              */

    /* ---- I2C 轮询超时分类计数 ---- */
    uint32_t i2c_poll_timeout_read_count;       /**< 读路径超时次数          */
    uint32_t i2c_poll_timeout_write_count;      /**< 写路径超时次数          */
    uint32_t i2c_poll_timeout_verify_count;     /**< 校验读路径超时次数      */
    uint32_t i2c_addrw_timeout_read_count;      /**< 读-ADDR_W 阶段超时次数  */
    uint32_t i2c_addrw_timeout_write_count;     /**< 写-ADDR_W 阶段超时次数  */
    uint32_t i2c_addr_8000_timeout_read_count;  /**< 读 0x8000 地址超时次数  */
    uint32_t i2c_addr_8000_timeout_write_count; /**< 写 0x8000 地址超时次数  */
    uint32_t i2c_addr_800d_timeout_read_count;  /**< 读 0x800D 地址超时次数  */
    uint32_t i2c_addr_800d_timeout_write_count; /**< 写 0x800D 地址超时次数  */
    uint32_t i2c_r8000_addrw_timeout_count;     /**< 读 0x8000 + ADDR_W 超时 */
    uint32_t i2c_w8000_addrw_timeout_count;     /**< 写 0x8000 + ADDR_W 超时 */
    uint32_t i2c_r800d_addrw_timeout_count;     /**< 读 0x800D + ADDR_W 超时 */
    uint32_t i2c_r800d_rx_timeout_count;        /**< 读 0x800D + RX 超时     */
    uint32_t i2c_busy_timeout_read_count;       /**< 读-总线忙超时次数        */
    uint32_t i2c_busy_timeout_write_count;      /**< 写-总线忙超时次数        */
    uint32_t i2c_busy_timeout_verify_count;     /**< 校验读-总线忙超时次数    */

    /* ---- I2C 总线恢复统计 ---- */
    uint32_t i2c_bus_clear_count;               /**< Bus Clear 总次数        */
    uint32_t i2c_stop_release_timeout_count;    /**< STOP 释放超时次数       */
    uint32_t i2c_bus_clear_read_count;          /**< 读触发 Bus Clear 次数   */
    uint32_t i2c_bus_clear_write_count;         /**< 写触发 Bus Clear 次数   */
    uint32_t i2c_bus_clear_dma_count;           /**< DMA 触发 Bus Clear 次数 */
    uint32_t i2c_bus_clear_busy_timeout_count;  /**< 忙超时触发 Bus Clear    */

    /* ---- DMA 与热成像退避统计 ---- */
    uint32_t dma_timeout_count;             /**< DMA 超时总次数              */
    uint32_t thermal_backoff_count;         /**< 热成像退避次数              */

    /* ---- 热成像子页配对统计 ---- */
    uint32_t thermal_pair_timeout_count;            /**< 配对超时次数        */
    uint32_t thermal_pair_grace_ok_count;           /**< 宽限期成功次数      */
    uint32_t thermal_pair_compose_ok_count;         /**< 正常合成成功次数    */
    uint32_t thermal_pair_wait_other_count;         /**< 等待另一子页次数    */
    uint32_t thermal_pair_last_result;              /**< 最近一次配对结果    */
    uint32_t thermal_pair_last_subpage;             /**< 最近一次子页编号    */
    uint32_t thermal_pair_last_missing_subpage;     /**< 最近缺失的子页编号  */
    uint32_t thermal_pair_last_gap_ms;              /**< 最近配对间隔（ms）  */
    uint32_t thermal_pair_timeout_gap_last_ms;      /**< 超时间隔-最近（ms） */
    uint32_t thermal_pair_timeout_gap_max_ms;       /**< 超时间隔-最大（ms） */
    uint32_t thermal_pair_timeout_gap_80_120_count; /**< 超时间隔 80~120ms   */
    uint32_t thermal_pair_timeout_gap_120_160_count;/**< 超时间隔 120~160ms  */
    uint32_t thermal_pair_timeout_gap_160_240_count;/**< 超时间隔 160~240ms  */
    uint32_t thermal_pair_timeout_gap_240_plus_count;/**< 超时间隔 >240ms    */
    uint32_t thermal_pair_compose_gap_last_ms;      /**< 合成间隔-最近（ms） */
    uint32_t thermal_pair_compose_gap_max_ms;       /**< 合成间隔-最大（ms） */
    uint32_t thermal_pair_same_subpage_streak_last; /**< 连续同子页-最近     */
    uint32_t thermal_pair_same_subpage_streak_max;  /**< 连续同子页-最大     */
    uint32_t thermal_pair_timeout_get_temp_last_us; /**< 超时时 GetTemp 耗时 */
    uint32_t thermal_pair_timeout_step_last_us;     /**< 超时时步进耗时      */
    uint32_t thermal_pair_soft_timeout_count;       /**< 软超时次数          */
    uint32_t thermal_pair_back_slot_null_count;     /**< 后备槽为空次数      */
    uint32_t thermal_ready_replace_count;           /**< 就绪替换次数        */
    uint32_t thermal_display_cancel_count;          /**< 显示取消次数        */

    /* ---- 热成像 3D 同步统计 ---- */
    uint32_t thermal_3d_sync_present_attempt_count; /**< 3D 同步呈现尝试次数 */
    uint32_t thermal_3d_sync_present_ok_count;      /**< 3D 同步呈现成功次数 */
    uint32_t thermal_3d_sync_present_fail_count;    /**< 3D 同步呈现失败次数 */
    uint32_t thermal_3d_claim_count;                /**< 3D 帧认领次数       */
    uint32_t thermal_3d_done_ok_count;              /**< 3D 完成-成功次数    */
    uint32_t thermal_3d_done_error_count;           /**< 3D 完成-错误次数    */
    uint32_t thermal_3d_done_cancel_count;          /**< 3D 完成-取消次数    */
    uint32_t thermal_3d_wait_timeout_count;         /**< 3D 等待超时次数     */

    /* ---- LCD DMA 事件计数 ---- */
    uint32_t lcd_dma_enter_count;           /**< LCD DMA 函数入口次数        */
    uint32_t dma_irq_tc_count;              /**< DMA 传输完成中断次数        */
    uint32_t dma_irq_te_count;              /**< DMA 传输错误中断次数        */
    uint32_t dma_wait_take_count;           /**< DMA 信号量获取次数          */

    /* ---- 看门狗状态 ---- */
    uint32_t watchdog_missing_progress_mask;/**< 缺失进展的任务掩码          */
    uint32_t watchdog_fault_flags;          /**< 看门狗故障标志              */

    /* ---- 任务栈水位（剩余字数） ---- */
    UBaseType_t input_stack_words;          /**< 输入任务栈剩余字数          */
    UBaseType_t service_stack_words;        /**< 服务任务栈剩余字数          */
    UBaseType_t ui_stack_words;             /**< UI 任务栈剩余字数           */
    UBaseType_t display_stack_words;        /**< 显示任务栈剩余字数          */
    UBaseType_t thermal_stack_words;        /**< 热成像任务栈剩余字数        */
    UBaseType_t power_stack_words;          /**< 电源任务栈剩余字数          */
} app_perf_baseline_snapshot_t;

/* =========================================================================
 *  5. 初始化与控制接口
 * ======================================================================= */

/**
 * @brief  初始化性能基线模块
 * @note   重置所有计数器并启用 DWT 周期计数器。
 */
void app_perf_baseline_init(void);

/**
 * @brief  重置所有性能计数器与统计量
 * @note   在临界区内批量清零，保证多任务环境下的原子性。
 */
void app_perf_baseline_reset(void);

/**
 * @brief  查询性能采集是否启用
 * @retval 1 — 已启用；0 — 已关闭
 */
uint8_t app_perf_baseline_is_enabled(void);

/* =========================================================================
 *  6. 时间测量工具接口
 * ======================================================================= */

/**
 * @brief  获取当前 DWT 周期计数值
 * @return 当前周期计数（32 位，溢出自动回绕）
 */
uint32_t app_perf_baseline_cycle_now(void);

/**
 * @brief  计算从 start_cycle 到现在的经过时间（微秒）
 * @param  start_cycle — 起始周期计数值
 * @return 经过的微秒数
 */
uint32_t app_perf_baseline_elapsed_us(uint32_t start_cycle);

/* =========================================================================
 *  7. 热成像帧采集记录接口
 * ======================================================================= */

/**
 * @brief  记录一次成功的热成像捕获
 * @param  capture_tick_ms — 捕获时的系统 tick（ms）
 * @param  min_temp        — 最低温度
 * @param  max_temp        — 最高温度
 * @param  center_temp     — 中心温度
 */
void app_perf_baseline_record_thermal_capture_success(uint32_t capture_tick_ms,
                                                      float min_temp,
                                                      float max_temp,
                                                      float center_temp);

/**
 * @brief  记录一次热成像捕获失败
 */
void app_perf_baseline_record_thermal_capture_failure(void);

/**
 * @brief  记录热成像显示延迟
 * @param  elapsed_ms — 从捕获到显示的延迟（ms）
 */
void app_perf_baseline_record_thermal_display_age_ms(uint32_t elapsed_ms);

/* =========================================================================
 *  8. 耗时测量记录接口
 * ======================================================================= */

/**
 * @brief  记录 GetTemp 调用耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_get_temp_us(uint32_t elapsed_us);

/**
 * @brief  记录灰度转换耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_gray_us(uint32_t elapsed_us);

/**
 * @brief  记录热成像步进耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_thermal_step_us(uint32_t elapsed_us);

/* =========================================================================
 *  9. LCD DMA 传输记录接口
 * ======================================================================= */

/**
 * @brief  记录 LCD DMA 传输结果
 * @param  elapsed_us — 传输总耗时（微秒）
 * @param  status     — 传输状态
 */
void app_perf_baseline_record_lcd_dma_result(uint32_t elapsed_us,
                                             app_perf_lcd_dma_status_t status);

/**
 * @brief  记录 LCD DMA 渲染阶段耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_render_us(uint32_t elapsed_us);

/**
 * @brief  记录 LCD DMA 启动阶段耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_start_us(uint32_t elapsed_us);

/**
 * @brief  记录 LCD DMA 等待阶段耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_wait_us(uint32_t elapsed_us);

/**
 * @brief  记录 SPI 总线空闲等待耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_spi_idle_us(uint32_t elapsed_us);

/**
 * @brief  记录十字光标叠加耗时
 * @param  elapsed_us — 耗时（微秒）
 */
void app_perf_baseline_record_lcd_dma_overlay_us(uint32_t elapsed_us);

/* =========================================================================
 *  10. TaskNotify 与队列记录接口
 * ======================================================================= */

/**
 * @brief  记录一次 TaskNotify 调用
 * @param  target — 通知目标任务
 */
void app_perf_baseline_record_task_notify(app_perf_notify_target_t target);

/**
 * @brief  记录按键队列溢出
 */
void app_perf_baseline_record_key_queue_drop(void);

/**
 * @brief  记录 UI 消息队列溢出
 */
void app_perf_baseline_record_ui_msg_drop(void);

/**
 * @brief  记录服务队列入队失败
 */
void app_perf_baseline_record_service_queue_fail(void);

/**
 * @brief  记录显示队列入队失败
 */
void app_perf_baseline_record_display_queue_fail(void);

/* =========================================================================
 *  11. UART 错误记录接口
 * ======================================================================= */

/**
 * @brief  记录 UART 错误
 * @param  flags — UART 错误标志位
 */
void app_perf_baseline_record_uart_errors(uint32_t flags);

/* =========================================================================
 *  12. I2C 错误记录接口
 * ======================================================================= */

/**
 * @brief  记录一次 I2C 失败（通用）
 */
void app_perf_baseline_record_i2c_failure(void);

/**
 * @brief  记录一次 I2C 传输错误（分类）
 * @param  error_kind — 错误类型
 */
void app_perf_baseline_record_i2c_transport_error(app_perf_i2c_error_t error_kind);

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
                                                       uint32_t sr2);

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
                                                  uint32_t sr2);

/**
 * @brief  记录一次 I2C 事件中断
 */
void app_perf_baseline_record_i2c_dma_ev_irq(void);

/**
 * @brief  记录一次 DMA 传输完成中断
 */
void app_perf_baseline_record_i2c_dma_tc_irq(void);

/**
 * @brief  记录一次 DMA 等待超时
 */
void app_perf_baseline_record_i2c_dma_wait_timeout(void);

/* =========================================================================
 *  13. I2C 轮询超时记录接口
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
                                                     uint32_t sr2);

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
                                                    uint32_t sr2);

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
                                             uint32_t sr2);

/**
 * @brief  记录一次 STOP 信号释放超时
 */
void app_perf_baseline_record_i2c_stop_release_timeout(void);

/**
 * @brief  记录一次 I2C 总线恢复（Bus Clear）
 * @param  source — 触发来源
 */
void app_perf_baseline_record_i2c_bus_clear(app_perf_i2c_bus_clear_source_t source);

/* =========================================================================
 *  14. 热成像子页配对记录接口
 * ======================================================================= */

/**
 * @brief  记录热成像退避事件
 */
void app_perf_baseline_record_thermal_backoff(void);

/**
 * @brief  记录一次子页配对超时
 */
void app_perf_baseline_record_thermal_pair_timeout(void);

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
                                                      uint32_t same_subpage_streak);

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
                                                          uint32_t step_elapsed_us);

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
                                                    uint32_t same_subpage_streak);

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
                                                      uint32_t same_subpage_streak);

/**
 * @brief  记录一次软超时
 */
void app_perf_baseline_record_thermal_soft_timeout(void);

/**
 * @brief  记录一次后备槽为空
 */
void app_perf_baseline_record_thermal_back_slot_null(void);

/**
 * @brief  记录一次就绪替换
 */
void app_perf_baseline_record_thermal_ready_replace(void);

/**
 * @brief  记录一次显示取消
 */
void app_perf_baseline_record_thermal_display_cancel(void);

/* =========================================================================
 *  15. 热成像 3D 同步记录接口
 * ======================================================================= */

/** @brief  记录 3D 同步呈现尝试 */
void app_perf_baseline_record_thermal_3d_sync_present_attempt(void);

/** @brief  记录 3D 同步呈现成功 */
void app_perf_baseline_record_thermal_3d_sync_present_ok(void);

/** @brief  记录 3D 同步呈现失败 */
void app_perf_baseline_record_thermal_3d_sync_present_fail(void);

/** @brief  记录 3D 帧认领 */
void app_perf_baseline_record_thermal_3d_claim(void);

/** @brief  记录 3D 完成-成功 */
void app_perf_baseline_record_thermal_3d_done_ok(void);

/** @brief  记录 3D 完成-错误 */
void app_perf_baseline_record_thermal_3d_done_error(void);

/** @brief  记录 3D 完成-取消 */
void app_perf_baseline_record_thermal_3d_done_cancel(void);

/** @brief  记录 3D 等待超时 */
void app_perf_baseline_record_thermal_3d_wait_timeout(void);

/* =========================================================================
 *  16. LCD DMA 事件记录接口
 * ======================================================================= */

/** @brief  记录一次 LCD DMA 函数入口 */
void app_perf_baseline_record_lcd_dma_enter(void);

/** @brief  记录一次 DMA 传输完成中断 */
void app_perf_baseline_record_dma_irq_tc(void);

/** @brief  记录一次 DMA 传输错误中断 */
void app_perf_baseline_record_dma_irq_te(void);

/** @brief  记录一次 DMA 信号量获取 */
void app_perf_baseline_record_dma_wait_take(void);

/* =========================================================================
 *  17. 看门狗与运行时状态接口
 * ======================================================================= */

/**
 * @brief  设置看门狗快照信息
 * @param  missing_progress_mask — 缺失进展的任务掩码
 * @param  fault_flags           — 故障标志
 */
void app_perf_baseline_set_watchdog_snapshot(uint32_t missing_progress_mask,
                                             uint32_t fault_flags);

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
                                         uint8_t screen_off);

/* =========================================================================
 *  18. 任务栈水位与快照导出接口
 * ======================================================================= */

/**
 * @brief  刷新所有任务的栈水位信息
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
                                           TaskHandle_t power_task);

/**
 * @brief  导出性能基线快照
 * @note   在临界区内批量拷贝所有 volatile 计数器到快照结构体，
 *         保证快照数据的一致性。
 * @param  snapshot — 输出：快照结构体指针
 */
void app_perf_baseline_get_snapshot(app_perf_baseline_snapshot_t *snapshot);

#endif /* APP_PERF_BASELINE_H */
