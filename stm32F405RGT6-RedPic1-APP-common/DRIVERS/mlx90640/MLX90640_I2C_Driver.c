/**
 * @file    MLX90640_I2C_Driver.c
 * @brief   MLX90640 红外热成像传感器 I2C 驱动
 * @note    本模块为 MLX90640 提供 I2C 通信驱动，支持两种传输路径：
 *          - 轮询路径（Polling）：适用于小数据量寄存器读写，延迟可控
 *          - DMA 路径：适用于大数据量帧读取（>= 阈值），降低 CPU 占用
 *
 * @par 架构设计
 *      公共 API（MLX90640_I2CRead / MLX90640_I2CWrite）保持同步语义，
 *      内部根据数据量和调度器状态自动选择最优传输路径。
 *      总线互斥锁确保多任务环境下 I2C 总线访问的原子性。
 *
 * @par 错误恢复策略
 *      - NACK / 总线错误 / 仲裁丢失：终止传输并重新初始化 I2C 外设
 *      - 总线忙超时：执行时钟脉冲清障（Bus Clear）后重新初始化
 *      - STOP 后总线未释放：执行 Bus Clear 后重新初始化
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "MLX90640_I2C_Driver.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "app_perf_baseline.h"

/* =========================================================================
 *  2. 宏定义 — I2C 外设引脚配置
 * ======================================================================= */

/** @defgroup MLX90640_I2C_HW  I2C 硬件引脚定义
 *  @{ */
#define I2Cx                    I2C1                    /**< 使用的 I2C 外设实例  */
#define I2Cx_CLK                RCC_APB1Periph_I2C1     /**< I2C 时钟总线         */
#define I2Cx_GPIO_PORT          GPIOB                   /**< I2C GPIO 端口        */
#define I2Cx_SCL_PIN            GPIO_Pin_6              /**< SCL 引脚（PB6）      */
#define I2Cx_SDA_PIN            GPIO_Pin_7              /**< SDA 引脚（PB7）      */
#define I2Cx_SCL_PINSOURCE      GPIO_PinSource6         /**< SCL 引脚复用源       */
#define I2Cx_SDA_PINSOURCE      GPIO_PinSource7         /**< SDA 引脚复用源       */
/** @} */

/* =========================================================================
 *  3. 宏定义 — DMA RX 配置
 * ======================================================================= */

/** @defgroup MLX90640_DMA_HW  DMA 接收通道配置
 *  @{ */
#define MLX90640_I2C_RX_DMA_STREAM      DMA1_Stream0            /**< DMA 接收流           */
#define MLX90640_I2C_RX_DMA_CHANNEL     DMA_Channel_1           /**< DMA 通道             */
#define MLX90640_I2C_RX_DMA_CLK         RCC_AHB1Periph_DMA1     /**< DMA 时钟             */
#define MLX90640_I2C_RX_DMA_IRQn        DMA1_Stream0_IRQn       /**< DMA 中断号           */
#define MLX90640_I2C_RX_DMA_TC_FLAG     DMA_IT_TCIF0            /**< DMA 传输完成标志     */
#define MLX90640_I2C_RX_DMA_TE_FLAG     DMA_IT_TEIF0            /**< DMA 传输错误标志     */
#define MLX90640_I2C_RX_DMA_ALL_FLAGS   (DMA_FLAG_FEIF0  | \
                                          DMA_FLAG_DMEIF0 | \
                                          DMA_FLAG_TEIF0  | \
                                          DMA_FLAG_HTIF0  | \
                                          DMA_FLAG_TCIF0)        /**< DMA 全部中断标志掩码 */
/** @} */

/* =========================================================================
 *  4. 宏定义 — 错误码与超时参数
 * ======================================================================= */

/** @defgroup MLX90640_ERR  I2C 传输错误码
 *  @{ */
#define MLX90640_I2C_ERROR_NACK         (-1)    /**< 从机无应答（NACK）        */
#define MLX90640_I2C_ERROR_VERIFY       (-2)    /**< 写后读校验失败            */
#define MLX90640_I2C_ERROR_TIMEOUT      (-3)    /**< 总线超时 / 仲裁丢失等     */
/** @} */

/** @defgroup MLX90640_TIMEOUT  超时与等待参数
 *  @{ */
#define MLX90640_I2C_BUSY_TIMEOUT_US        5000UL      /**< 总线忙等待超时（微秒）   */
#define MLX90640_I2C_EVENT_TIMEOUT_US       5000UL      /**< 事件等待超时（微秒）     */
#define MLX90640_I2C_STOP_RELEASE_WAIT_US   5000UL      /**< STOP 后总线释放等待      */
#define MLX90640_I2C_BUS_CLEAR_PULSE_US     5UL         /**< Bus Clear 时钟脉冲宽度   */
#define MLX90640_I2C_BUS_CLEAR_PULSE_COUNT  9U          /**< Bus Clear 最大脉冲数     */
#define MLX90640_I2C_DMA_DISABLE_WAIT_LOOPS 100000UL    /**< DMA 流关闭忙等待上限     */
#define MLX90640_I2C_RX_BUFFER_BYTES        1664U       /**< DMA 接收缓冲区大小       */
#define MLX90640_I2C_WAIT_FLAG_EVENT_BASE   0x80000000UL /**< Flag 等待事件基址（诊断用） */
/** @} */

/** @brief I2C 总线速率：1MHz（Fast Mode） */
#define I2Cx_SPEED  1000000U

/* =========================================================================
 *  5. 数据类型定义 — DMA 状态机
 * ======================================================================= */

/**
 * @brief DMA 传输状态机枚举
 * @note  用于中断服务程序中跟踪 I2C+DMA 多阶段传输的当前进度。
 *        状态转换由 I2C 事件中断驱动。
 */
typedef enum
{
    MLX90640_I2C_DMA_STATE_IDLE         = 0,    /**< 空闲态                  */
    MLX90640_I2C_DMA_STATE_START_WRITE  = 1,    /**< 已发送 START，等待 SB   */
    MLX90640_I2C_DMA_STATE_ADDR_WRITE   = 2,    /**< 已发送写地址，等待 ADDR */
    MLX90640_I2C_DMA_STATE_MEM_HI       = 3,    /**< 已发送寄存器地址高字节  */
    MLX90640_I2C_DMA_STATE_MEM_LO       = 4,    /**< 已发送寄存器地址低字节  */
    MLX90640_I2C_DMA_STATE_START_READ   = 5,    /**< 已发送重复 START        */
    MLX90640_I2C_DMA_STATE_ADDR_READ    = 6,    /**< 已发送读地址，等待 ADDR */
    MLX90640_I2C_DMA_STATE_DMA_RX       = 7,    /**< DMA 接收进行中          */
    MLX90640_I2C_DMA_STATE_DONE         = 8,    /**< 传输完成                */
    MLX90640_I2C_DMA_STATE_ERROR        = 9     /**< 传输错误                */
} mlx90640_i2c_dma_state_t;

/**
 * @brief DMA 传输上下文结构体
 * @note  记录当前 DMA 事务的所有状态信息，
 *        由任务上下文填写、ISR 中更新、任务上下文读取。
 */
typedef struct
{
    volatile uint8_t                    active;             /**< DMA 传输是否激活         */
    volatile uint8_t                    recovery_pending;   /**< 是否需要错误恢复         */
    volatile uint8_t                    slave_addr;         /**< 从机地址（已左移 1 位）  */
    volatile uint16_t                   start_address;      /**< 起始寄存器地址           */
    volatile uint16_t                   byte_count;         /**< 传输字节数               */
    volatile uint32_t                   transaction_seq;    /**< 事务序列号（递增）       */
    volatile uint32_t                   completed_seq;      /**< 已完成事务序列号         */
    volatile int                        result;             /**< 传输结果（0=成功）       */
    volatile app_perf_i2c_error_t       error_kind;         /**< 错误类型                 */
    volatile mlx90640_i2c_dma_state_t   state;              /**< 当前状态机状态           */
} mlx90640_i2c_dma_context_t;

/* =========================================================================
 *  6. 模块级静态变量
 * ======================================================================= */

/** @defgroup MLX90640_VARS  模块级变量
 *  @{ */

/* ---- DMA 接收缓冲区（必须位于系统 SRAM，DMA 可达） ---- */
__attribute__((section("dma_sram"), aligned(4)))
static uint8_t s_mlx90640_i2c_rx_buffer[MLX90640_I2C_RX_BUFFER_BYTES]; /**< DMA 接收缓冲区 */

/* ---- 同步原语 ---- */
static SemaphoreHandle_t    s_mlx90640_i2c_bus_mutex   = 0;    /**< I2C 总线互斥锁       */
static SemaphoreHandle_t    s_mlx90640_i2c_done_sem    = 0;    /**< DMA 完成信号量        */

/* ---- 运行时状态 ---- */
static uint8_t              s_mlx90640_i2c_runtime_ready = 0U; /**< 运行时是否已初始化   */
static mlx90640_i2c_dma_context_t s_mlx90640_i2c_dma_ctx;       /**< DMA 传输上下文       */

/* ---- 轮询诊断信息 ---- */
static app_perf_i2c_poll_path_t   s_mlx90640_i2c_poll_diag_path       = APP_PERF_I2C_POLL_PATH_NONE;
static app_perf_i2c_poll_phase_t  s_mlx90640_i2c_poll_diag_phase      = APP_PERF_I2C_POLL_PHASE_NONE;
static uint16_t                   s_mlx90640_i2c_poll_diag_start_addr = 0U;
static uint16_t                   s_mlx90640_i2c_poll_diag_word_count = 0U;
static uint32_t                   s_mlx90640_i2c_poll_diag_event      = 0U;
static app_perf_i2c_poll_path_t   s_mlx90640_i2c_next_read_path      = APP_PERF_I2C_POLL_PATH_NONE;

/** @} */

/* =========================================================================
 *  7. 内部函数前向声明
 * ======================================================================= */

static uint8_t  MLX90640_I2CWaitBusReleaseAfterStop  (void);
static void     MLX90640_I2CBusClearAndReinit        (app_perf_i2c_bus_clear_source_t source);
static void     MLX90640_I2CStopRequestsNoReinit     (void);

/* =========================================================================
 *  8. 诊断辅助函数
 * ======================================================================= */

/**
 * @brief  开始一次轮询诊断记录
 * @param  path         — 读/写/校验读路径标识
 * @param  start_address — 起始寄存器地址
 * @param  word_count    — 读取字数
 */
static void MLX90640_I2CPollDiagBegin(app_perf_i2c_poll_path_t path,
                                      uint16_t start_address,
                                      uint16_t word_count)
{
    s_mlx90640_i2c_poll_diag_path       = path;
    s_mlx90640_i2c_poll_diag_phase      = APP_PERF_I2C_POLL_PHASE_NONE;
    s_mlx90640_i2c_poll_diag_start_addr = start_address;
    s_mlx90640_i2c_poll_diag_word_count = word_count;
    s_mlx90640_i2c_poll_diag_event      = 0U;
}

/**
 * @brief  更新当前轮询阶段和事件
 */
static void MLX90640_I2CPollDiagSetPhase(app_perf_i2c_poll_phase_t phase, uint32_t event)
{
    s_mlx90640_i2c_poll_diag_phase = phase;
    s_mlx90640_i2c_poll_diag_event = event;
}

/**
 * @brief  消费并返回下一次读操作的路径标识
 * @note   读操作完成后自动重置为 NONE，默认为 READ。
 */
static app_perf_i2c_poll_path_t MLX90640_I2CPollDiagConsumeReadPath(void)
{
    app_perf_i2c_poll_path_t path = s_mlx90640_i2c_next_read_path;

    s_mlx90640_i2c_next_read_path = APP_PERF_I2C_POLL_PATH_NONE;
    if (path == APP_PERF_I2C_POLL_PATH_NONE)
    {
        path = APP_PERF_I2C_POLL_PATH_READ;
    }

    return path;
}

/* =========================================================================
 *  9. 特殊寄存器读取判断
 * ======================================================================= */

/**
 * @brief  判断是否需要使用特殊单字读取流程
 * @note   MLX90640 的 0x8000 和 0x800D 寄存器需要特殊的读取时序。
 * @param  start_address — 起始寄存器地址
 * @param  word_count    — 读取字数
 * @retval 1 — 需要特殊读取；0 — 使用常规流程
 */
static uint8_t MLX90640_I2CUseSpecialRegRead(uint16_t start_address, uint16_t word_count)
{
#if (REDPIC1_MLX90640_I2C_SPECIAL_REG_READ_ENABLE != 0U)
    if ((word_count == 1U) &&
        ((start_address == 0x8000U) || (start_address == 0x800DU)))
    {
        return 1U;
    }
#else
    (void)start_address;
    (void)word_count;
#endif

    return 0U;
}

/* =========================================================================
 *  10. 定时器与延时工具
 * ======================================================================= */

/**
 * @brief  判断 FreeRTOS 调度器是否正在运行
 */
static uint8_t MLX90640_I2CSchedulerRunning(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

/**
 * @brief  初始化 DWT 周期计数器
 * @note   DWT_CYCCNT 是 Cortex-M4 内核的高精度周期计数器，
 *         用于实现微秒级忙等待延时。
 */
static void MLX90640_I2CTimerInit(void)
{
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

/**
 * @brief  计算自 start_cycle 以来经过的微秒数
 * @param  start_cycle — 起始 DWT 周期计数
 * @return uint32_t — 经过的微秒数
 */
static uint32_t MLX90640_I2CElapsedUs(uint32_t start_cycle)
{
    uint32_t cycles_per_us = SystemCoreClock / 1000000UL;

    if (cycles_per_us == 0U)
    {
        cycles_per_us = 1U;
    }

    return (DWT->CYCCNT - start_cycle) / cycles_per_us;
}

/**
 * @brief  微秒级忙等待延时
 * @param  wait_us — 等待微秒数
 */
static void MLX90640_I2CBusyWaitUs(uint32_t wait_us)
{
    uint32_t start_cycle = 0U;

    MLX90640_I2CTimerInit();
    start_cycle = DWT->CYCCNT;
    while (MLX90640_I2CElapsedUs(start_cycle) < wait_us)
    {
    }
}

/* =========================================================================
 *  11. 总线清障与重新初始化
 * ======================================================================= */

/**
 * @brief  将 SCL/SDA 引脚配置为开漏输出模式（用于 Bus Clear）
 */
static void MLX90640_I2CConfigurePinsForBusClear(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_InitStruct.GPIO_Pin   = I2Cx_SCL_PIN | I2Cx_SDA_PIN;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_OD;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStruct.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(I2Cx_GPIO_PORT, &GPIO_InitStruct);
    GPIO_SetBits(I2Cx_GPIO_PORT, I2Cx_SCL_PIN | I2Cx_SDA_PIN);
}

/**
 * @brief  执行 I2C 总线清障并重新初始化
 * @note   当总线卡死（SDA 被从机拉低）时，通过手动产生时钟脉冲
 *         释放 SDA，然后发送 STOP 条件恢复总线。
 * @param  source — 触发清障的来源（用于诊断记录）
 */
static void MLX90640_I2CBusClearAndReinit(app_perf_i2c_bus_clear_source_t source)
{
    uint32_t pulse = 0U;

    app_perf_baseline_record_i2c_bus_clear(source);
    MLX90640_I2CStopRequestsNoReinit();

    /* 关闭 I2C 外设，切换到 GPIO 手动控制模式 */
    I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, DISABLE);
    I2C_Cmd(I2Cx, DISABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    MLX90640_I2CConfigurePinsForBusClear();
    MLX90640_I2CBusyWaitUs(MLX90640_I2C_BUS_CLEAR_PULSE_US);

    /* 若 SDA 仍被拉低，发送最多 9 个时钟脉冲尝试释放 */
    if (GPIO_ReadInputDataBit(I2Cx_GPIO_PORT, I2Cx_SDA_PIN) == Bit_RESET)
    {
        for (pulse = 0U;
             pulse < MLX90640_I2C_BUS_CLEAR_PULSE_COUNT &&
             GPIO_ReadInputDataBit(I2Cx_GPIO_PORT, I2Cx_SDA_PIN) == Bit_RESET;
             ++pulse)
        {
            GPIO_ResetBits(I2Cx_GPIO_PORT, I2Cx_SCL_PIN);
            MLX90640_I2CBusyWaitUs(MLX90640_I2C_BUS_CLEAR_PULSE_US);
            GPIO_SetBits(I2Cx_GPIO_PORT, I2Cx_SCL_PIN);
            MLX90640_I2CBusyWaitUs(MLX90640_I2C_BUS_CLEAR_PULSE_US);
        }
    }

    /* 发送 STOP 条件：SDA 低 -> SCL 高 -> SDA 高 */
    GPIO_ResetBits(I2Cx_GPIO_PORT, I2Cx_SDA_PIN);
    MLX90640_I2CBusyWaitUs(MLX90640_I2C_BUS_CLEAR_PULSE_US);
    GPIO_SetBits(I2Cx_GPIO_PORT, I2Cx_SCL_PIN);
    MLX90640_I2CBusyWaitUs(MLX90640_I2C_BUS_CLEAR_PULSE_US);
    GPIO_SetBits(I2Cx_GPIO_PORT, I2Cx_SDA_PIN);
    MLX90640_I2CBusyWaitUs(MLX90640_I2C_BUS_CLEAR_PULSE_US);

    /* 验证 SDA 已释放（从机可能仍拉低 SDA） */
    if (GPIO_ReadInputDataBit(I2Cx_GPIO_PORT, I2Cx_SDA_PIN) == Bit_RESET)
    {
        app_perf_baseline_record_i2c_bus_clear(source);
    }

    /* 重新初始化 I2C 外设 */
    MLX90640_I2CInit();
}

/* =========================================================================
 *  12. 错误标志与 DMA 标志管理
 * ======================================================================= */

/**
 * @brief  清除 I2C 全部错误中断标志
 */
static void MLX90640_I2CClearErrorFlags(void)
{
    I2C_ClearITPendingBit(I2Cx, I2C_IT_AF);
    I2C_ClearITPendingBit(I2Cx, I2C_IT_BERR);
    I2C_ClearITPendingBit(I2Cx, I2C_IT_ARLO);
    I2C_ClearITPendingBit(I2Cx, I2C_IT_OVR);
    I2C_ClearITPendingBit(I2Cx, I2C_IT_TIMEOUT);
}

/**
 * @brief  清除 DMA 接收流全部中断标志
 */
static void MLX90640_I2CClearDmaFlags(void)
{
    DMA_ClearFlag(MLX90640_I2C_RX_DMA_STREAM, MLX90640_I2C_RX_DMA_ALL_FLAGS);
}

/**
 * @brief  读取 DMA 当前剩余数据计数
 */
static uint16_t MLX90640_I2CReadDmaNdtr(void)
{
    return (uint16_t)DMA_GetCurrDataCounter(MLX90640_I2C_RX_DMA_STREAM);
}

/**
 * @brief  记录 DMA 超时时的硬件快照（用于诊断）
 */
static void MLX90640_I2CCaptureTimeoutSnapshot(void)
{
    app_perf_baseline_record_i2c_dma_timeout_snapshot(MLX90640_I2CReadDmaNdtr(),
                                                      (uint8_t)s_mlx90640_i2c_dma_ctx.state,
                                                      I2Cx->SR1,
                                                      I2Cx->SR2);
}

/**
 * @brief  记录 DMA 传输完成时的硬件快照（用于诊断）
 */
static void MLX90640_I2CCaptureTcSnapshot(void)
{
    app_perf_baseline_record_i2c_dma_tc_snapshot(MLX90640_I2CReadDmaNdtr(),
                                                 (uint8_t)s_mlx90640_i2c_dma_ctx.state,
                                                 I2Cx->SR1,
                                                 I2Cx->SR2);
}

/* =========================================================================
 *  13. 信号量与 DMA 流管理
 * ======================================================================= */

/**
 * @brief  排空 DMA 完成信号量
 * @note   确保信号量处于干净的空状态，防止上次超时遗留的信号干扰。
 */
static void MLX90640_I2CDrainDoneSemaphore(void)
{
    if (s_mlx90640_i2c_done_sem == 0)
    {
        return;
    }

    while (xSemaphoreTake(s_mlx90640_i2c_done_sem, 0U) == pdPASS)
    {
    }
}

/**
 * @brief  关闭 DMA 接收流（带超时保护）
 */
static void MLX90640_I2CDmaDisableStream(void)
{
    uint32_t timeout = MLX90640_I2C_DMA_DISABLE_WAIT_LOOPS;

    DMA_Cmd(MLX90640_I2C_RX_DMA_STREAM, DISABLE);
    while (DMA_GetCmdStatus(MLX90640_I2C_RX_DMA_STREAM) != DISABLE)
    {
        if (timeout-- == 0U)
        {
            break;
        }
    }
}

/* =========================================================================
 *  14. 传输中止与完成处理
 * ======================================================================= */

/**
 * @brief  停止所有 I2C/DMA 请求（不重新初始化外设）
 * @note   关闭中断、DMA 使能，清除 DMA 标志。
 */
static void MLX90640_I2CStopRequestsNoReinit(void)
{
    I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, DISABLE);
    I2C_DMACmd(I2Cx, DISABLE);
    I2C_DMALastTransferCmd(I2Cx, DISABLE);
    MLX90640_I2CDmaDisableStream();
    MLX90640_I2CClearDmaFlags();
}

/**
 * @brief  中止传输并在任务上下文中恢复 I2C 外设
 * @note   发送 STOP、使能 ACK、清除错误标志后重新初始化。
 */
static void MLX90640_I2CAbortTransferTaskContext(void)
{
    MLX90640_I2CStopRequestsNoReinit();
    I2C_GenerateSTOP(I2Cx, ENABLE);
    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    MLX90640_I2CClearErrorFlags();
    s_mlx90640_i2c_dma_ctx.active             = 0U;
    s_mlx90640_i2c_dma_ctx.recovery_pending   = 0U;
    s_mlx90640_i2c_dma_ctx.slave_addr         = 0U;
    s_mlx90640_i2c_dma_ctx.start_address      = 0U;
    s_mlx90640_i2c_dma_ctx.byte_count         = 0U;
    s_mlx90640_i2c_dma_ctx.transaction_seq    = 0U;
    s_mlx90640_i2c_dma_ctx.completed_seq      = 0U;
    s_mlx90640_i2c_dma_ctx.result             = 0;
    s_mlx90640_i2c_dma_ctx.error_kind         = APP_PERF_I2C_ERROR_AF;
    s_mlx90640_i2c_dma_ctx.state              = MLX90640_I2C_DMA_STATE_IDLE;
    MLX90640_I2CInit();
}

/**
 * @brief  DMA 传输成功完成后的任务上下文处理
 * @note   停止请求、使能 ACK、等待总线释放。
 */
static void MLX90640_I2CFinishDmaSuccessTaskContext(void)
{
    MLX90640_I2CStopRequestsNoReinit();
    I2C_AcknowledgeConfig(I2Cx, ENABLE);

    /* 等待 STOP 后总线释放，超时则执行 Bus Clear */
    if (MLX90640_I2CWaitBusReleaseAfterStop() == 0U)
    {
        app_perf_baseline_record_i2c_stop_release_timeout();
        MLX90640_I2CBusClearAndReinit(APP_PERF_I2C_BUS_CLEAR_DMA);
    }

    s_mlx90640_i2c_dma_ctx.active             = 0U;
    s_mlx90640_i2c_dma_ctx.recovery_pending   = 0U;
    s_mlx90640_i2c_dma_ctx.slave_addr         = 0U;
    s_mlx90640_i2c_dma_ctx.start_address      = 0U;
    s_mlx90640_i2c_dma_ctx.byte_count         = 0U;
    s_mlx90640_i2c_dma_ctx.transaction_seq    = 0U;
    s_mlx90640_i2c_dma_ctx.completed_seq      = 0U;
    s_mlx90640_i2c_dma_ctx.result             = 0;
    s_mlx90640_i2c_dma_ctx.error_kind         = APP_PERF_I2C_ERROR_AF;
    s_mlx90640_i2c_dma_ctx.state              = MLX90640_I2C_DMA_STATE_IDLE;
}

/* =========================================================================
 *  15. 错误处理与映射
 * ======================================================================= */

/**
 * @brief  将内部错误类型映射为公共错误码
 */
static int MLX90640_I2CMapTransportError(app_perf_i2c_error_t error_kind)
{
    if (error_kind == APP_PERF_I2C_ERROR_AF)
    {
        return MLX90640_I2C_ERROR_NACK;
    }

    return MLX90640_I2C_ERROR_TIMEOUT;
}

/**
 * @brief  检查并处理 I2C 轮询路径中的错误标志
 * @note   依次检查 NACK、总线错误、仲裁丢失、溢出、超时，
 *         任一错误触发则中止传输并返回对应错误码。
 * @retval 0 — 无错误；非 0 — 错误码
 */
static int MLX90640_I2CHandlePollingErrorFlags(void)
{
    uint32_t sr1 = I2Cx->SR1;

    /* NACK：从机无应答 */
    if ((sr1 & I2C_SR1_AF) != 0U)
    {
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_AF);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_NACK;
    }

    /* 总线错误 */
    if ((sr1 & I2C_SR1_BERR) != 0U)
    {
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_BERR);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    /* 仲裁丢失 */
    if ((sr1 & I2C_SR1_ARLO) != 0U)
    {
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_ARLO);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    /* 数据溢出 */
    if ((sr1 & I2C_SR1_OVR) != 0U)
    {
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_OVR);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    /* 超时 */
    if ((sr1 & I2C_SR1_TIMEOUT) != 0U)
    {
        app_perf_baseline_record_i2c_poll_event_timeout(
            s_mlx90640_i2c_poll_diag_path,
            s_mlx90640_i2c_poll_diag_phase,
            s_mlx90640_i2c_poll_diag_event,
            s_mlx90640_i2c_poll_diag_start_addr,
            s_mlx90640_i2c_poll_diag_word_count,
            sr1, 0U);
        app_perf_baseline_record_i2c_er_timeout(
            s_mlx90640_i2c_poll_diag_path,
            s_mlx90640_i2c_poll_diag_phase,
            s_mlx90640_i2c_poll_diag_start_addr,
            s_mlx90640_i2c_poll_diag_word_count,
            sr1, 0U);
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_TIMEOUT);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    return 0;
}

/* =========================================================================
 *  16. 总线状态等待函数
 * ======================================================================= */

/**
 * @brief  等待 I2C 总线空闲
 * @note   总线忙超时后执行 Bus Clear 恢复。
 * @retval 0 — 总线空闲；MLX90640_I2C_ERROR_TIMEOUT — 超时
 */
static int MLX90640_I2CWaitWhileBusy(void)
{
    uint32_t start_cycle = 0U;

    MLX90640_I2CTimerInit();
    start_cycle = DWT->CYCCNT;

    while (I2C_GetFlagStatus(I2Cx, I2C_FLAG_BUSY) != RESET)
    {
        if (MLX90640_I2CElapsedUs(start_cycle) >= MLX90640_I2C_BUSY_TIMEOUT_US)
        {
            uint32_t sr1 = I2Cx->SR1;
            uint32_t sr2 = I2Cx->SR2;

            app_perf_baseline_record_i2c_poll_busy_timeout(
                s_mlx90640_i2c_poll_diag_path,
                s_mlx90640_i2c_poll_diag_start_addr,
                s_mlx90640_i2c_poll_diag_word_count,
                sr1, sr2);
            app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_BUSY_STUCK);
            MLX90640_I2CBusClearAndReinit(APP_PERF_I2C_BUS_CLEAR_BUSY_TIMEOUT);
            return MLX90640_I2C_ERROR_TIMEOUT;
        }
    }

    return 0;
}

/**
 * @brief  等待 STOP 发送后总线释放
 * @note   非阻塞式超时，超时后由调用方决定恢复策略。
 * @retval 1 — 总线已释放；0 — 超时
 */
static uint8_t MLX90640_I2CWaitBusReleaseAfterStop(void)
{
    uint32_t start_cycle = 0U;

    MLX90640_I2CTimerInit();
    start_cycle = DWT->CYCCNT;

    while (I2C_GetFlagStatus(I2Cx, I2C_FLAG_BUSY) != RESET)
    {
        if (MLX90640_I2CElapsedUs(start_cycle) >= MLX90640_I2C_STOP_RELEASE_WAIT_US)
        {
            return 0U;
        }
    }

    return 1U;
}

/**
 * @brief  等待 I2C 事件发生
 * @note   轮询 I2C 状态寄存器直到目标事件匹配，同时检查错误标志。
 *         超时边界再复查一次，防止误判。
 * @param  event — 目标 I2C 事件
 * @retval 0 — 事件匹配；非 0 — 错误码
 */
static int MLX90640_I2CWaitEvent(uint32_t event)
{
    uint32_t start_cycle = 0U;
    int error = 0;

    MLX90640_I2CTimerInit();
    start_cycle = DWT->CYCCNT;

    while (I2C_CheckEvent(I2Cx, event) == ERROR)
    {
        error = MLX90640_I2CHandlePollingErrorFlags();
        if (error != 0)
        {
            return error;
        }

        if (MLX90640_I2CElapsedUs(start_cycle) >= MLX90640_I2C_EVENT_TIMEOUT_US)
        {
            /* 超时边界再复查一次 */
            if (I2C_CheckEvent(I2Cx, event) != ERROR)
            {
                return 0;
            }

            uint32_t sr1 = I2Cx->SR1;
            uint32_t sr2 = I2Cx->SR2;

            app_perf_baseline_record_i2c_poll_event_timeout(
                s_mlx90640_i2c_poll_diag_path,
                s_mlx90640_i2c_poll_diag_phase,
                event,
                s_mlx90640_i2c_poll_diag_start_addr,
                s_mlx90640_i2c_poll_diag_word_count,
                sr1, sr2);
            app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_TIMEOUT);
            MLX90640_I2CAbortTransferTaskContext();
            return MLX90640_I2C_ERROR_TIMEOUT;
        }
    }

    return 0;
}

/**
 * @brief  等待 I2C 状态标志置位
 * @note   与 MLX90640_I2CWaitEvent 类似，但使用 GetFlagStatus 接口。
 *         超时边界再复查一次，防止误判。
 * @param  flag — 目标状态标志
 * @retval 0 — 标志已置位；非 0 — 错误码
 */
static int MLX90640_I2CWaitFlag(uint32_t flag)
{
    uint32_t start_cycle = 0U;
    int error = 0;

    MLX90640_I2CTimerInit();
    start_cycle = DWT->CYCCNT;

    while (I2C_GetFlagStatus(I2Cx, flag) == RESET)
    {
        error = MLX90640_I2CHandlePollingErrorFlags();
        if (error != 0)
        {
            return error;
        }

        if (MLX90640_I2CElapsedUs(start_cycle) >= MLX90640_I2C_EVENT_TIMEOUT_US)
        {
            /* 超时边界再复查一次，防止 flag 刚好到达时误判 timeout */
            if (I2C_GetFlagStatus(I2Cx, flag) != RESET)
            {
                return 0;
            }

            uint32_t sr1 = I2Cx->SR1;
            uint32_t sr2 = I2Cx->SR2;

            app_perf_baseline_record_i2c_poll_event_timeout(
                s_mlx90640_i2c_poll_diag_path,
                s_mlx90640_i2c_poll_diag_phase,
                MLX90640_I2C_WAIT_FLAG_EVENT_BASE | flag,
                s_mlx90640_i2c_poll_diag_start_addr,
                s_mlx90640_i2c_poll_diag_word_count,
                sr1, sr2);
            app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_TIMEOUT);
            MLX90640_I2CAbortTransferTaskContext();
            return MLX90640_I2C_ERROR_TIMEOUT;
        }
    }

    return 0;
}

/**
 * @brief  清除 ADDR 标志（通过读取 SR1 和 SR2）
 */
static void MLX90640_I2CClearAddrFlag(void)
{
    volatile uint32_t dummy = 0U;

    dummy = I2Cx->SR1;
    dummy = I2Cx->SR2;
    (void)dummy;
}

/**
 * @brief  等待写地址阶段就绪
 * @note   等待 ADDR 标志 -> 清除 ADDR -> 等待 TXE 标志。
 * @param  first_tx_phase — TXE 等待阶段的诊断标识
 * @retval 0 — 就绪；非 0 — 错误码
 */
static int MLX90640_I2CWaitAddrWriteReady(app_perf_i2c_poll_phase_t first_tx_phase)
{
    int error = 0;

    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_ADDR_W,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_ADDR);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_ADDR);
    if (error != 0)
    {
        return error;
    }

    MLX90640_I2CClearAddrFlag();

    MLX90640_I2CPollDiagSetPhase(first_tx_phase,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_TXE);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_TXE);
    if (error != 0)
    {
        return error;
    }

    return 0;
}

/* =========================================================================
 *  17. 数据格式转换
 * ======================================================================= */

/**
 * @brief  将 DMA 接收的字节流转换为 16 位大端字数组
 * @note   MLX90640 数据格式为大端序（高字节在前）。
 * @param  word_count — 字数
 * @param  data       — 输出：字数组指针
 */
static void MLX90640_I2CConvertBytesToWords(uint16_t word_count, uint16_t *data)
{
    uint16_t index = 0U;

    if (data == 0)
    {
        return;
    }

    for (index = 0U; index < word_count; ++index)
    {
        uint16_t byte_index = (uint16_t)(index << 1U);
        data[index] = (uint16_t)(((uint16_t)s_mlx90640_i2c_rx_buffer[byte_index] << 8U) |
                                 (uint16_t)s_mlx90640_i2c_rx_buffer[byte_index + 1U]);
    }
}

/* =========================================================================
 *  18. 轮询路径 — 特殊单字读取
 * ======================================================================= */

/**
 * @brief  使用特殊时序读取单个 16 位寄存器
 * @note   针对 MLX90640 的 0x8000 / 0x800D 寄存器，
 *         使用 BTF 标志控制数据接收时序。
 * @param  slaveAddr     — 7 位从机地址
 * @param  startAddress  — 起始寄存器地址
 * @param  data          — 输出：读取到的 16 位数据
 * @retval 0 — 成功；非 0 — 错误码
 */
static int MLX90640_I2CReadSpecialWordPollingLocked(uint8_t slaveAddr,
                                                    uint16_t startAddress,
                                                    uint16_t *data)
{
    uint8_t sa = (uint8_t)(slaveAddr << 1U);
    uint8_t cmd[2];
    int error = 0;
    app_perf_i2c_poll_path_t poll_path = MLX90640_I2CPollDiagConsumeReadPath();

    if (data == 0)
    {
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    MLX90640_I2CPollDiagBegin(poll_path, startAddress, 1U);
    cmd[0] = (uint8_t)(startAddress >> 8U);
    cmd[1] = (uint8_t)(startAddress & 0xFFU);

    I2C_NACKPositionConfig(I2Cx, I2C_NACKPosition_Current);
    I2C_AcknowledgeConfig(I2Cx, ENABLE);

    /* 1. 等待总线空闲 */
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_WAIT_BUSY, 0U);
    error = MLX90640_I2CWaitWhileBusy();
    if (error != 0)
    {
        return error;
    }

    /* 2. 发送 START + 写地址 */
    I2C_GenerateSTART(I2Cx, ENABLE);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_START,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_SB);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_SB);
    if (error != 0)
    {
        return error;
    }

    I2C_Send7bitAddress(I2Cx, sa, I2C_Direction_Transmitter);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_ADDR_W,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_ADDR);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_ADDR);
    if (error != 0)
    {
        return error;
    }
    MLX90640_I2CClearAddrFlag();

    /* 3. 发送寄存器地址（高字节 + 低字节） */
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_REG_HI,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_TXE);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_TXE);
    if (error != 0)
    {
        return error;
    }
    I2C_SendData(I2Cx, cmd[0]);

    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_REG_LO,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_TXE);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_TXE);
    if (error != 0)
    {
        return error;
    }
    I2C_SendData(I2Cx, cmd[1]);

    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_REG_LO,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_BTF);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_BTF);
    if (error != 0)
    {
        return error;
    }

    /* 4. 重复 START + 读地址 */
    I2C_GenerateSTART(I2Cx, ENABLE);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_RESTART,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_SB);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_SB);
    if (error != 0)
    {
        return error;
    }

    I2C_NACKPositionConfig(I2Cx, I2C_NACKPosition_Next);
    I2C_Send7bitAddress(I2Cx, (uint8_t)(sa | 0x01U), I2C_Direction_Receiver);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_ADDR_R,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_ADDR);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_ADDR);
    if (error != 0)
    {
        I2C_NACKPositionConfig(I2Cx, I2C_NACKPosition_Current);
        I2C_AcknowledgeConfig(I2Cx, ENABLE);
        return error;
    }

    /* 5. 禁用 ACK（单字读取，NACK 告知从机停止发送） */
    __disable_irq();
    I2C_AcknowledgeConfig(I2Cx, DISABLE);
    MLX90640_I2CClearAddrFlag();
    __enable_irq();

    /* 6. 等待 BTF（两个字节全部收到） */
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_BYTE_RECEIVED,
                                 MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_BTF);
    error = MLX90640_I2CWaitFlag(I2C_FLAG_BTF);
    if (error != 0)
    {
        I2C_NACKPositionConfig(I2Cx, I2C_NACKPosition_Current);
        I2C_AcknowledgeConfig(I2Cx, ENABLE);
        return error;
    }

    /* 7. 发送 STOP 并读取两个字节 */
    __disable_irq();
    I2C_GenerateSTOP(I2Cx, ENABLE);
    s_mlx90640_i2c_rx_buffer[0] = I2C_ReceiveData(I2Cx);
    __enable_irq();
    s_mlx90640_i2c_rx_buffer[1] = I2C_ReceiveData(I2Cx);

    /* 恢复 ACK 配置 */
    I2C_NACKPositionConfig(I2Cx, I2C_NACKPosition_Current);
    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    MLX90640_I2CConvertBytesToWords(1U, data);
    return 0;
}

/* =========================================================================
 *  19. 轮询路径 — 通用读取
 * ======================================================================= */

/**
 * @brief  使用轮询方式读取多个 16 位寄存器
 * @note   对于 2 字节（1 字）读取使用优化路径，
 *         大于 2 字节使用逐字节接收循环。
 * @param  slaveAddr        — 7 位从机地址
 * @param  startAddress     — 起始寄存器地址
 * @param  nMemAddressRead  — 读取字数
 * @param  data             — 输出：读取到的数据数组
 * @retval 0 — 成功；非 0 — 错误码
 */
static int MLX90640_I2CReadPollingLocked(uint8_t slaveAddr,
                                         uint16_t startAddress,
                                         uint16_t nMemAddressRead,
                                         uint16_t *data)
{
    uint8_t  sa         = (uint8_t)(slaveAddr << 1U);
    uint8_t  cmd[2];
    uint16_t byte_count = (uint16_t)(nMemAddressRead << 1U);
    uint16_t index      = 0U;
    int      error      = 0;
    app_perf_i2c_poll_path_t poll_path = MLX90640_I2CPollDiagConsumeReadPath();

    if (data == 0 || byte_count > MLX90640_I2C_RX_BUFFER_BYTES)
    {
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    /* 特殊寄存器走专用读取流程 */
    if (MLX90640_I2CUseSpecialRegRead(startAddress, nMemAddressRead) != 0U)
    {
        return MLX90640_I2CReadSpecialWordPollingLocked(slaveAddr, startAddress, data);
    }

    MLX90640_I2CPollDiagBegin(poll_path, startAddress, nMemAddressRead);
    cmd[0] = (uint8_t)(startAddress >> 8U);
    cmd[1] = (uint8_t)(startAddress & 0xFFU);

    /* 1. 等待总线空闲 */
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_WAIT_BUSY, 0U);
    error = MLX90640_I2CWaitWhileBusy();
    if (error != 0)
    {
        return error;
    }

    /* 2. 发送 START + 写地址 */
    I2C_GenerateSTART(I2Cx, ENABLE);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_START,
                                 I2C_EVENT_MASTER_MODE_SELECT);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);
    if (error != 0)
    {
        return error;
    }

    I2C_Send7bitAddress(I2Cx, sa, I2C_Direction_Transmitter);
    error = MLX90640_I2CWaitAddrWriteReady(APP_PERF_I2C_POLL_PHASE_REG_HI);
    if (error != 0)
    {
        return error;
    }

    /* 3. 发送寄存器地址（高字节 + 低字节） */
    I2C_SendData(I2Cx, cmd[0]);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_REG_HI,
                                 I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    if (error != 0)
    {
        return error;
    }

    I2C_SendData(I2Cx, cmd[1]);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_REG_LO,
                                 I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    if (error != 0)
    {
        return error;
    }

    /* 4. 重复 START + 读地址 */
    I2C_GenerateSTART(I2Cx, ENABLE);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_RESTART,
                                 I2C_EVENT_MASTER_MODE_SELECT);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);
    if (error != 0)
    {
        return error;
    }

    I2C_Send7bitAddress(I2Cx, (uint8_t)(sa | 0x01U), I2C_Direction_Receiver);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_ADDR_R,
                                 I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED);
    if (error != 0)
    {
        return error;
    }

    /* 5a. 2 字节优化路径 */
    if (byte_count == 2U)
    {
        I2C_NACKPositionConfig(I2Cx, I2C_NACKPosition_Next);

        __disable_irq();
        I2C_AcknowledgeConfig(I2Cx, DISABLE);
        MLX90640_I2CClearAddrFlag();
        __enable_irq();

        MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_BYTE_RECEIVED,
                                     MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_BTF);
        error = MLX90640_I2CWaitFlag(I2C_FLAG_BTF);
        if (error != 0)
        {
            I2C_NACKPositionConfig(I2Cx, I2C_NACKPosition_Current);
            I2C_AcknowledgeConfig(I2Cx, ENABLE);
            return error;
        }

        __disable_irq();
        I2C_GenerateSTOP(I2Cx, ENABLE);
        s_mlx90640_i2c_rx_buffer[0] = I2C_ReceiveData(I2Cx);
        __enable_irq();
        s_mlx90640_i2c_rx_buffer[1] = I2C_ReceiveData(I2Cx);

        I2C_NACKPositionConfig(I2Cx, I2C_NACKPosition_Current);
        I2C_AcknowledgeConfig(I2Cx, ENABLE);
        if (MLX90640_I2CWaitBusReleaseAfterStop() == 0U)
        {
            app_perf_baseline_record_i2c_stop_release_timeout();
            MLX90640_I2CBusClearAndReinit(APP_PERF_I2C_BUS_CLEAR_READ);
        }
        MLX90640_I2CConvertBytesToWords(nMemAddressRead, data);
        return 0;
    }

    /* 5b. 多字节逐字节接收循环 */
    for (index = 0U; index < byte_count; ++index)
    {
        /* 最后一个字节：禁用 ACK 并发送 STOP */
        if (index == (uint16_t)(byte_count - 1U))
        {
            I2C_AcknowledgeConfig(I2Cx, DISABLE);
            I2C_GenerateSTOP(I2Cx, ENABLE);
        }

        MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_BYTE_RECEIVED,
                                     MLX90640_I2C_WAIT_FLAG_EVENT_BASE | I2C_FLAG_RXNE);
        error = MLX90640_I2CWaitFlag(I2C_FLAG_RXNE);
        if (error != 0)
        {
            return error;
        }

        s_mlx90640_i2c_rx_buffer[index] = I2C_ReceiveData(I2Cx);
    }

    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    if (MLX90640_I2CWaitBusReleaseAfterStop() == 0U)
    {
        app_perf_baseline_record_i2c_stop_release_timeout();
        MLX90640_I2CBusClearAndReinit(APP_PERF_I2C_BUS_CLEAR_READ);
    }
    MLX90640_I2CConvertBytesToWords(nMemAddressRead, data);
    return 0;
}

/* =========================================================================
 *  20. 轮询路径 — 写入
 * ======================================================================= */

/**
 * @brief  使用轮询方式写入单个 16 位寄存器
 * @note   写入后自动执行读回校验（0x8000 寄存器除外）。
 * @param  slaveAddr     — 7 位从机地址
 * @param  writeAddress  — 目标寄存器地址
 * @param  data          — 写入的 16 位数据
 * @retval 0 — 成功；非 0 — 错误码
 */
static int MLX90640_I2CWritePollingLocked(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    uint8_t  sa        = (uint8_t)(slaveAddr << 1U);
    uint8_t  cmd[4];
    uint16_t dataCheck = 0U;
    int      error     = 0;
    int      index     = 0;

    MLX90640_I2CPollDiagBegin(APP_PERF_I2C_POLL_PATH_WRITE, writeAddress, 1U);
    cmd[0] = (uint8_t)(writeAddress >> 8U);
    cmd[1] = (uint8_t)(writeAddress & 0xFFU);
    cmd[2] = (uint8_t)(data >> 8U);
    cmd[3] = (uint8_t)(data & 0xFFU);

    /* 1. 等待总线空闲 */
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_WAIT_BUSY, 0U);
    error = MLX90640_I2CWaitWhileBusy();
    if (error != 0)
    {
        return error;
    }

    /* 2. 发送 START + 写地址 */
    I2C_GenerateSTART(I2Cx, ENABLE);
    MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_START,
                                 I2C_EVENT_MASTER_MODE_SELECT);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);
    if (error != 0)
    {
        return error;
    }

    I2C_Send7bitAddress(I2Cx, sa, I2C_Direction_Transmitter);
    error = MLX90640_I2CWaitAddrWriteReady(APP_PERF_I2C_POLL_PHASE_REG_HI);
    if (error != 0)
    {
        return error;
    }

    /* 3. 逐字节发送：寄存器地址（2 字节）+ 数据（2 字节） */
    for (index = 0; index < 4; ++index)
    {
        I2C_SendData(I2Cx, cmd[index]);
        MLX90640_I2CPollDiagSetPhase(APP_PERF_I2C_POLL_PHASE_BYTE_TRANSMITTED,
                                     I2C_EVENT_MASTER_BYTE_TRANSMITTED);
        error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
        if (error != 0)
        {
            return error;
        }
    }

    /* 4. 发送 STOP */
    I2C_GenerateSTOP(I2Cx, ENABLE);
    if (MLX90640_I2CWaitBusReleaseAfterStop() == 0U)
    {
        app_perf_baseline_record_i2c_stop_release_timeout();
        MLX90640_I2CBusClearAndReinit(APP_PERF_I2C_BUS_CLEAR_WRITE);
    }

    /* 0x8000 寄存器写入后无需校验 */
    if (writeAddress == 0x8000U)
    {
        return 0;
    }

    /* 5. 写后读校验 */
    s_mlx90640_i2c_next_read_path = APP_PERF_I2C_POLL_PATH_VERIFY_READ;
    error = MLX90640_I2CReadPollingLocked(slaveAddr, writeAddress, 1U, &dataCheck);
    if (error != 0)
    {
        return error;
    }

    return (dataCheck == data) ? 0 : MLX90640_I2C_ERROR_VERIFY;
}

/* =========================================================================
 *  21. DMA 路径 — 判断与准备
 * ======================================================================= */

/**
 * @brief  判断是否允许使用 DMA 读取路径
 * @note   需同时满足：数据量 >= 阈值、调度器运行中、运行时已初始化。
 * @param  word_count — 读取字数
 * @retval 1 — 允许 DMA；0 — 使用轮询
 */
static uint8_t MLX90640_I2CDmaReadAllowed(uint16_t word_count)
{
#if (REDPIC1_MLX90640_I2C_DMA_ENABLE != 0U)
    if (word_count < REDPIC1_MLX90640_I2C_DMA_WORD_THRESHOLD)
    {
        return 0U;
    }

    if (MLX90640_I2CSchedulerRunning() == 0U)
    {
        return 0U;
    }

    if (s_mlx90640_i2c_runtime_ready == 0U ||
        s_mlx90640_i2c_done_sem == 0)
    {
        return 0U;
    }

    return 1U;
#else
    (void)word_count;
    return 0U;
#endif
}

/**
 * @brief  准备 DMA 传输上下文
 * @note   递增事务序列号，填写从机地址、寄存器地址、字节数等。
 */
static void MLX90640_I2CDmaPrepareContext(uint8_t slaveAddr,
                                          uint16_t startAddress,
                                          uint16_t byte_count)
{
    s_mlx90640_i2c_dma_ctx.transaction_seq++;
    if (s_mlx90640_i2c_dma_ctx.transaction_seq == 0U)
    {
        s_mlx90640_i2c_dma_ctx.transaction_seq = 1U;
    }

    s_mlx90640_i2c_dma_ctx.active            = 1U;
    s_mlx90640_i2c_dma_ctx.recovery_pending  = 0U;
    s_mlx90640_i2c_dma_ctx.slave_addr        = (uint8_t)(slaveAddr << 1U);
    s_mlx90640_i2c_dma_ctx.start_address     = startAddress;
    s_mlx90640_i2c_dma_ctx.byte_count        = byte_count;
    s_mlx90640_i2c_dma_ctx.result            = MLX90640_I2C_ERROR_TIMEOUT;
    s_mlx90640_i2c_dma_ctx.error_kind        = APP_PERF_I2C_ERROR_TIMEOUT;
    s_mlx90640_i2c_dma_ctx.completed_seq     = 0U;
    s_mlx90640_i2c_dma_ctx.state             = MLX90640_I2C_DMA_STATE_START_WRITE;
}

/* =========================================================================
 *  22. DMA 路径 — ISR 完成信号
 * ======================================================================= */

/**
 * @brief  从 ISR 中通知 DMA 传输完成
 * @note   更新上下文状态，释放完成信号量唤醒等待任务。
 * @param  result     — 传输结果（0=成功）
 * @param  error_kind — 错误类型
 * @param  state      — 最终状态
 */
static void MLX90640_I2CDmaSignalDoneFromISR(int result,
                                             app_perf_i2c_error_t error_kind,
                                             mlx90640_i2c_dma_state_t state)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (s_mlx90640_i2c_dma_ctx.active == 0U)
    {
        return;
    }

    /* 防止重复完成 */
    if (s_mlx90640_i2c_dma_ctx.completed_seq == s_mlx90640_i2c_dma_ctx.transaction_seq)
    {
        return;
    }

    s_mlx90640_i2c_dma_ctx.result           = result;
    s_mlx90640_i2c_dma_ctx.error_kind       = error_kind;
    s_mlx90640_i2c_dma_ctx.recovery_pending = (result == 0) ? 0U : 1U;
    s_mlx90640_i2c_dma_ctx.state            = state;
    s_mlx90640_i2c_dma_ctx.completed_seq    = s_mlx90640_i2c_dma_ctx.transaction_seq;

    if (s_mlx90640_i2c_done_sem != 0)
    {
        xSemaphoreGiveFromISR(s_mlx90640_i2c_done_sem, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

/**
 * @brief  从 ISR 中停止 I2C/DMA 请求
 */
static void MLX90640_I2CDmaStopRequestsFromISR(void)
{
    I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, DISABLE);
    I2C_DMACmd(I2Cx, DISABLE);
    I2C_DMALastTransferCmd(I2Cx, DISABLE);
    DMA_Cmd(MLX90640_I2C_RX_DMA_STREAM, DISABLE);
    MLX90640_I2CClearDmaFlags();
}

/* =========================================================================
 *  23. DMA 路径 — 等待完成
 * ======================================================================= */

/**
 * @brief  等待 DMA 事务完成
 * @note   使用信号量阻塞等待，超时后返回 0。
 *         多次检查 completed_seq 防止信号量遗漏。
 * @param  transaction_seq — 期望的事务序列号
 * @retval 1 — 完成；0 — 超时
 */
static int MLX90640_I2CWaitForDmaCompletion(uint32_t transaction_seq)
{
    TickType_t start_ticks   = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(REDPIC1_MLX90640_I2C_DMA_TIMEOUT_MS);

    while (1)
    {
        TickType_t now_ticks      = xTaskGetTickCount();
        TickType_t elapsed_ticks  = now_ticks - start_ticks;
        TickType_t remaining_ticks = 0U;

        /* 直接检查完成标志 */
        if (s_mlx90640_i2c_dma_ctx.completed_seq == transaction_seq)
        {
            return 1;
        }

        /* 超时检查（含二次确认） */
        if (elapsed_ticks >= timeout_ticks)
        {
            if (s_mlx90640_i2c_dma_ctx.completed_seq == transaction_seq)
            {
                return 1;
            }
            return 0;
        }

        /* 阻塞等待信号量 */
        remaining_ticks = timeout_ticks - elapsed_ticks;
        if (xSemaphoreTake(s_mlx90640_i2c_done_sem, remaining_ticks) != pdPASS)
        {
            if (s_mlx90640_i2c_dma_ctx.completed_seq == transaction_seq)
            {
                return 1;
            }
            return 0;
        }

        /* 收到信号量后再次检查 */
        if (s_mlx90640_i2c_dma_ctx.completed_seq == transaction_seq)
        {
            return 1;
        }
    }
}

/* =========================================================================
 *  24. DMA 路径 — 读取实现
 * ======================================================================= */

/**
 * @brief  使用 DMA 方式读取多个 16 位寄存器
 * @note   通过 I2C 事件中断驱动状态机完成地址发送阶段，
 *         数据接收阶段由 DMA 自动完成，大幅降低 CPU 占用。
 * @param  slaveAddr        — 7 位从机地址
 * @param  startAddress     — 起始寄存器地址
 * @param  nMemAddressRead  — 读取字数
 * @param  data             — 输出：读取到的数据数组
 * @retval 0 — 成功；非 0 — 错误码
 */
static int MLX90640_I2CReadDmaLocked(uint8_t slaveAddr,
                                     uint16_t startAddress,
                                     uint16_t nMemAddressRead,
                                     uint16_t *data)
{
    uint16_t byte_count      = (uint16_t)(nMemAddressRead << 1U);
    uint32_t transaction_seq = 0U;
    int      error           = 0;

    if (data == 0 || byte_count > MLX90640_I2C_RX_BUFFER_BYTES)
    {
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    /* 等待总线空闲 */
    error = MLX90640_I2CWaitWhileBusy();
    if (error != 0)
    {
        return error;
    }

    /* 清理上次残留状态 */
    MLX90640_I2CDrainDoneSemaphore();
    MLX90640_I2CClearErrorFlags();
    MLX90640_I2CClearDmaFlags();
    MLX90640_I2CDmaDisableStream();

    /* 准备 DMA 缓冲区和上下文 */
    memset(s_mlx90640_i2c_rx_buffer, 0, byte_count);
    MLX90640_I2CDmaPrepareContext(slaveAddr, startAddress, byte_count);
    transaction_seq = s_mlx90640_i2c_dma_ctx.transaction_seq;

    /* 配置 DMA 接收 */
    MLX90640_I2C_RX_DMA_STREAM->M0AR = (uint32_t)s_mlx90640_i2c_rx_buffer;
    DMA_SetCurrDataCounter(MLX90640_I2C_RX_DMA_STREAM, byte_count);

    /* 启动 I2C 传输（中断驱动状态机） */
    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR, ENABLE);
    I2C_GenerateSTART(I2Cx, ENABLE);

    /* 等待 DMA 完成或超时 */
    if (MLX90640_I2CWaitForDmaCompletion(transaction_seq) == 0)
    {
        MLX90640_I2CCaptureTimeoutSnapshot();
        app_perf_baseline_record_i2c_dma_wait_timeout();
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_TIMEOUT);
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    /* 检查 DMA 传输结果 */
    if (s_mlx90640_i2c_dma_ctx.result != 0)
    {
        error = MLX90640_I2CMapTransportError(s_mlx90640_i2c_dma_ctx.error_kind);
        MLX90640_I2CAbortTransferTaskContext();
        return error;
    }

    /* 成功：清理并转换数据 */
    MLX90640_I2CFinishDmaSuccessTaskContext();
    MLX90640_I2CConvertBytesToWords(nMemAddressRead, data);
    return 0;
}

/* =========================================================================
 *  25. 内部读写调度（自动选择路径）
 * ======================================================================= */

/**
 * @brief  内部读取调度：自动选择 DMA 或轮询路径
 */
static int MLX90640_I2CReadLocked(uint8_t slaveAddr,
                                  uint16_t startAddress,
                                  uint16_t nMemAddressRead,
                                  uint16_t *data)
{
    if (MLX90640_I2CDmaReadAllowed(nMemAddressRead) != 0U)
    {
        return MLX90640_I2CReadDmaLocked(slaveAddr, startAddress, nMemAddressRead, data);
    }

    return MLX90640_I2CReadPollingLocked(slaveAddr, startAddress, nMemAddressRead, data);
}

/**
 * @brief  内部写入调度：当前仅支持轮询路径
 */
static int MLX90640_I2CWriteLocked(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    return MLX90640_I2CWritePollingLocked(slaveAddr, writeAddress, data);
}

/* =========================================================================
 *  26. 总线互斥锁
 * ======================================================================= */

/**
 * @brief  获取 I2C 总线互斥锁
 * @retval 1 — 已锁定；0 — 调度器未运行或锁未创建
 */
static uint8_t MLX90640_I2CLockBus(void)
{
    if (MLX90640_I2CSchedulerRunning() == 0U || s_mlx90640_i2c_bus_mutex == 0)
    {
        return 0U;
    }

    (void)xSemaphoreTake(s_mlx90640_i2c_bus_mutex, portMAX_DELAY);
    return 1U;
}

/**
 * @brief  释放 I2C 总线互斥锁
 */
static void MLX90640_I2CUnlockBus(uint8_t locked)
{
    if (locked != 0U && s_mlx90640_i2c_bus_mutex != 0)
    {
        (void)xSemaphoreGive(s_mlx90640_i2c_bus_mutex);
    }
}

/* =========================================================================
 *  27. 公共 API — 运行时初始化
 * ======================================================================= */

/**
 * @brief  初始化 MLX90640 I2C 运行时环境
 * @note   创建总线互斥锁和 DMA 完成信号量。
 *         仅在 FreeRTOS 调度器启动后调用。
 * @retval 1 — 成功；0 — 失败
 */
uint8_t MLX90640_I2CRuntimeInit(void)
{
    if (s_mlx90640_i2c_runtime_ready != 0U)
    {
        return 1U;
    }

    s_mlx90640_i2c_bus_mutex = xSemaphoreCreateMutex();
    s_mlx90640_i2c_done_sem  = xSemaphoreCreateBinary();
    if (s_mlx90640_i2c_bus_mutex == 0 || s_mlx90640_i2c_done_sem == 0)
    {
        return 0U;
    }

    MLX90640_I2CDrainDoneSemaphore();
    s_mlx90640_i2c_runtime_ready = 1U;
    return 1U;
}

/* =========================================================================
 *  28. 公共 API — I2C 外设硬件初始化
 * ======================================================================= */

/**
 * @brief  初始化 I2C1 外设及 DMA 接收通道
 * @note   配置 GPIO 复用、I2C 速率、DMA 通道、中断优先级等。
 *         可在调度器启动前调用（Bus Clear 恢复路径也会调用）。
 */
void MLX90640_I2CInit(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    I2C_InitTypeDef  I2C_InitStruct;
    DMA_InitTypeDef  DMA_InitStruct;
    NVIC_InitTypeDef NVIC_InitStruct;

    /* ---- 1. 使能时钟 ---- */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(I2Cx_CLK, ENABLE);
    RCC_AHB1PeriphClockCmd(MLX90640_I2C_RX_DMA_CLK, ENABLE);

    /* ---- 2. GPIO 配置（复用功能，开漏上拉） ---- */
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource6, GPIO_AF_I2C1);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource7, GPIO_AF_I2C1);

    GPIO_InitStruct.GPIO_Pin   = I2Cx_SCL_PIN | I2Cx_SDA_PIN;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_OD;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStruct.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(I2Cx_GPIO_PORT, &GPIO_InitStruct);

    /* ---- 3. I2C 外设配置 ---- */
    RCC_APB1PeriphResetCmd(I2Cx_CLK, ENABLE);
    RCC_APB1PeriphResetCmd(I2Cx_CLK, DISABLE);

    I2C_StructInit(&I2C_InitStruct);
    I2C_InitStruct.I2C_Mode                = I2C_Mode_I2C;
    I2C_InitStruct.I2C_DutyCycle           = I2C_DutyCycle_2;
    I2C_InitStruct.I2C_ClockSpeed          = I2Cx_SPEED;
    I2C_InitStruct.I2C_OwnAddress1         = 0x00;
    I2C_InitStruct.I2C_Ack                 = I2C_Ack_Enable;
    I2C_InitStruct.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(I2Cx, &I2C_InitStruct);
    I2C_Cmd(I2Cx, ENABLE);
    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    I2C_DMALastTransferCmd(I2Cx, DISABLE);
    I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, DISABLE);
    MLX90640_I2CClearErrorFlags();

    /* ---- 4. DMA 接收通道配置 ---- */
    DMA_DeInit(MLX90640_I2C_RX_DMA_STREAM);
    MLX90640_I2CDmaDisableStream();

    DMA_StructInit(&DMA_InitStruct);
    DMA_InitStruct.DMA_Channel            = MLX90640_I2C_RX_DMA_CHANNEL;
    DMA_InitStruct.DMA_PeripheralBaseAddr = (uint32_t)&I2Cx->DR;
    DMA_InitStruct.DMA_Memory0BaseAddr    = (uint32_t)s_mlx90640_i2c_rx_buffer;
    DMA_InitStruct.DMA_DIR                = DMA_DIR_PeripheralToMemory;
    DMA_InitStruct.DMA_BufferSize         = MLX90640_I2C_RX_BUFFER_BYTES;
    DMA_InitStruct.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    DMA_InitStruct.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    DMA_InitStruct.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStruct.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    DMA_InitStruct.DMA_Mode               = DMA_Mode_Normal;
    DMA_InitStruct.DMA_Priority           = DMA_Priority_VeryHigh;
    DMA_InitStruct.DMA_FIFOMode           = DMA_FIFOMode_Disable;
    DMA_InitStruct.DMA_FIFOThreshold      = DMA_FIFOThreshold_Full;
    DMA_InitStruct.DMA_MemoryBurst        = DMA_MemoryBurst_Single;
    DMA_InitStruct.DMA_PeripheralBurst    = DMA_PeripheralBurst_Single;
    DMA_Init(MLX90640_I2C_RX_DMA_STREAM, &DMA_InitStruct);

    /* 使能 DMA 传输完成和传输错误中断 */
    DMA_ITConfig(MLX90640_I2C_RX_DMA_STREAM, DMA_IT_TC | DMA_IT_TE, ENABLE);
    MLX90640_I2CClearDmaFlags();

    /* ---- 5. NVIC 中断优先级配置 ---- */
    /* I2C 错误中断（最高优先级） */
    NVIC_InitStruct.NVIC_IRQChannel                   = I2C1_ER_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority        = 1;
    NVIC_InitStruct.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    /* I2C 事件中断 */
    NVIC_InitStruct.NVIC_IRQChannel                   = I2C1_EV_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 6;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority        = 0;
    NVIC_Init(&NVIC_InitStruct);

    /* DMA 接收中断 */
    NVIC_InitStruct.NVIC_IRQChannel                   = MLX90640_I2C_RX_DMA_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority        = 0;
    NVIC_Init(&NVIC_InitStruct);
}

/* =========================================================================
 *  29. 公共 API — 读写接口
 * ======================================================================= */

/**
 * @brief  读取 MLX90640 寄存器（公共接口）
 * @note   自动获取总线锁，选择最优传输路径，完成后释放锁。
 * @param  slaveAddr        — 7 位从机地址
 * @param  startAddress     — 起始寄存器地址
 * @param  nMemAddressRead  — 读取字数
 * @param  data             — 输出：读取到的数据数组
 * @retval 0 — 成功；非 0 — 错误码
 */
int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress,
                     uint16_t nMemAddressRead, uint16_t *data)
{
    uint8_t locked = MLX90640_I2CLockBus();
    int result = MLX90640_I2CReadLocked(slaveAddr, startAddress, nMemAddressRead, data);

    MLX90640_I2CUnlockBus(locked);
    return result;
}

/**
 * @brief  写入 MLX90640 寄存器（公共接口）
 * @note   自动获取总线锁，写入后执行读回校验。
 * @param  slaveAddr     — 7 位从机地址
 * @param  writeAddress  — 目标寄存器地址
 * @param  data          — 写入的 16 位数据
 * @retval 0 — 成功；非 0 — 错误码
 */
int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    uint8_t locked = MLX90640_I2CLockBus();
    int result = MLX90640_I2CWriteLocked(slaveAddr, writeAddress, data);

    MLX90640_I2CUnlockBus(locked);
    return result;
}

/* =========================================================================
 *  30. 中断服务程序
 * ======================================================================= */

/**
 * @brief  I2C1 事件中断处理函数
 * @note   驱动 DMA 读取的状态机：START -> 写地址 -> 寄存器地址 ->
 *         重复 START -> 读地址 -> 使能 DMA 接收。
 *         仅在 DMA 模式启用且传输激活时处理。
 */
void I2C1_EV_IRQHandler(void)
{
#if (REDPIC1_MLX90640_I2C_DMA_ENABLE != 0U)
    if (s_mlx90640_i2c_dma_ctx.active == 0U)
    {
        return;
    }

    app_perf_baseline_record_i2c_dma_ev_irq();

    /* ---- START 已发送：发送写地址 ---- */
    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_START_WRITE) &&
        (I2C_GetITStatus(I2Cx, I2C_IT_SB) != RESET))
    {
        I2C_Send7bitAddress(I2Cx, s_mlx90640_i2c_dma_ctx.slave_addr,
                            I2C_Direction_Transmitter);
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_ADDR_WRITE;
        return;
    }

    /* ---- 写地址已发送：清除 ADDR，发送寄存器地址高字节 ---- */
    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_ADDR_WRITE) &&
        (I2C_GetITStatus(I2Cx, I2C_IT_ADDR) != RESET))
    {
        volatile uint32_t dummy = I2Cx->SR1;
        dummy = I2Cx->SR2;
        (void)dummy;
        I2C_SendData(I2Cx, (uint8_t)(s_mlx90640_i2c_dma_ctx.start_address >> 8U));
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_MEM_HI;
        return;
    }

    /* ---- 寄存器地址高字节已发送：发送低字节 ---- */
    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_MEM_HI) &&
        ((I2C_GetITStatus(I2Cx, I2C_IT_TXE) != RESET) ||
         (I2C_GetITStatus(I2Cx, I2C_IT_BTF) != RESET)))
    {
        I2C_SendData(I2Cx, (uint8_t)(s_mlx90640_i2c_dma_ctx.start_address & 0xFFU));
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_MEM_LO;
        return;
    }

    /* ---- 寄存器地址低字节已发送：发送重复 START ---- */
    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_MEM_LO) &&
        (I2C_GetITStatus(I2Cx, I2C_IT_BTF) != RESET))
    {
        I2C_GenerateSTART(I2Cx, ENABLE);
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_START_READ;
        return;
    }

    /* ---- 重复 START 已发送：发送读地址 ---- */
    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_START_READ) &&
        (I2C_GetITStatus(I2Cx, I2C_IT_SB) != RESET))
    {
        I2C_Send7bitAddress(I2Cx,
                            (uint8_t)(s_mlx90640_i2c_dma_ctx.slave_addr | 0x01U),
                            I2C_Direction_Receiver);
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_ADDR_READ;
        return;
    }

    /* ---- 读地址已发送：使能 DMA 接收 ---- */
    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_ADDR_READ) &&
        (I2C_GetITStatus(I2Cx, I2C_IT_ADDR) != RESET))
    {
        volatile uint32_t dummy = I2Cx->SR1;
        (void)dummy;

        I2C_AcknowledgeConfig(I2Cx, ENABLE);
        I2C_DMALastTransferCmd(I2Cx, ENABLE);

        /* 配置 DMA 接收 */
        MLX90640_I2CClearDmaFlags();
        MLX90640_I2C_RX_DMA_STREAM->M0AR = (uint32_t)s_mlx90640_i2c_rx_buffer;
        DMA_SetCurrDataCounter(MLX90640_I2C_RX_DMA_STREAM,
                               s_mlx90640_i2c_dma_ctx.byte_count);
        DMA_Cmd(MLX90640_I2C_RX_DMA_STREAM, ENABLE);
        I2C_DMACmd(I2Cx, ENABLE);

        dummy = I2Cx->SR2;
        (void)dummy;

        /* DMA 接管后仅保留 ERR 中断，关闭 EVT/BUF 防止中断风暴 */
        I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_BUF, DISABLE);
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_DMA_RX;
        return;
    }
#endif
}

/**
 * @brief  I2C1 错误中断处理函数
 * @note   处理 NACK、总线错误、仲裁丢失、溢出、超时等错误。
 *         记录诊断信息后停止 DMA 并通知任务上下文。
 */
void I2C1_ER_IRQHandler(void)
{
#if (REDPIC1_MLX90640_I2C_DMA_ENABLE != 0U)
    app_perf_i2c_error_t error_kind = APP_PERF_I2C_ERROR_TIMEOUT;
    uint8_t has_error = 0U;

    if (s_mlx90640_i2c_dma_ctx.active == 0U)
    {
        MLX90640_I2CClearErrorFlags();
        return;
    }

    /* 识别具体错误类型 */
    if (I2C_GetITStatus(I2Cx, I2C_IT_AF) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_AF;
        has_error  = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_AF);
    }
    else if (I2C_GetITStatus(I2Cx, I2C_IT_BERR) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_BERR;
        has_error  = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_BERR);
    }
    else if (I2C_GetITStatus(I2Cx, I2C_IT_ARLO) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_ARLO;
        has_error  = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_ARLO);
    }
    else if (I2C_GetITStatus(I2Cx, I2C_IT_OVR) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_OVR;
        has_error  = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_OVR);
    }
    else if (I2C_GetITStatus(I2Cx, I2C_IT_TIMEOUT) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_TIMEOUT;
        has_error  = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_TIMEOUT);
    }

    if (has_error == 0U)
    {
        return;
    }

    /* 记录诊断快照，停止 DMA，通知任务上下文 */
    MLX90640_I2CCaptureTimeoutSnapshot();
    app_perf_baseline_record_i2c_transport_error(error_kind);
    MLX90640_I2CDmaStopRequestsFromISR();
    MLX90640_I2CDmaSignalDoneFromISR(MLX90640_I2CMapTransportError(error_kind),
                                     error_kind,
                                     MLX90640_I2C_DMA_STATE_ERROR);
#endif
}

/**
 * @brief  DMA1 Stream0 中断处理函数
 * @note   处理 I2C1 RX DMA 的传输完成和传输错误中断。
 *         传输完成后发送 STOP 并通知任务上下文。
 */
void DMA1_Stream0_IRQHandler(void)
{
#if (REDPIC1_MLX90640_I2C_DMA_ENABLE != 0U)
    /* ---- 传输完成中断 ---- */
    if (DMA_GetITStatus(MLX90640_I2C_RX_DMA_STREAM, MLX90640_I2C_RX_DMA_TC_FLAG) != RESET)
    {
        app_perf_baseline_record_i2c_dma_tc_irq();
        MLX90640_I2CCaptureTcSnapshot();
        DMA_ClearITPendingBit(MLX90640_I2C_RX_DMA_STREAM, MLX90640_I2C_RX_DMA_TC_FLAG);

        /* 停止 DMA 和 I2C，发送 STOP */
        I2C_DMACmd(I2Cx, DISABLE);
        I2C_DMALastTransferCmd(I2Cx, DISABLE);
        DMA_Cmd(MLX90640_I2C_RX_DMA_STREAM, DISABLE);
        I2C_GenerateSTOP(I2Cx, ENABLE);
        I2C_AcknowledgeConfig(I2Cx, ENABLE);
        I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, DISABLE);

        MLX90640_I2CDmaSignalDoneFromISR(0,
                                         APP_PERF_I2C_ERROR_TIMEOUT,
                                         MLX90640_I2C_DMA_STATE_DONE);
        return;
    }

    /* ---- 传输错误中断 ---- */
    if (DMA_GetITStatus(MLX90640_I2C_RX_DMA_STREAM, MLX90640_I2C_RX_DMA_TE_FLAG) != RESET)
    {
        MLX90640_I2CCaptureTimeoutSnapshot();
        DMA_ClearITPendingBit(MLX90640_I2C_RX_DMA_STREAM, MLX90640_I2C_RX_DMA_TE_FLAG);

        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_DMA_ERR);
        MLX90640_I2CDmaStopRequestsFromISR();
        MLX90640_I2CDmaSignalDoneFromISR(MLX90640_I2C_ERROR_TIMEOUT,
                                         APP_PERF_I2C_ERROR_DMA_ERR,
                                         MLX90640_I2C_DMA_STATE_ERROR);
    }
#endif
}
