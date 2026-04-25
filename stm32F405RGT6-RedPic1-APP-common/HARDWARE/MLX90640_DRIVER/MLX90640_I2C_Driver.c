#include "MLX90640_I2C_Driver.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "app_perf_baseline.h"

/*
 * MLX90640 I2C driver
 *
 * Keep the public API synchronous while moving large reads to an
 * RTOS-friendly I2C1 + DMA1 RX path. Small register reads/writes remain on the
 * proven polling path so the migration risk stays limited.
 */

#define I2Cx               I2C1
#define I2Cx_CLK           RCC_APB1Periph_I2C1
#define I2Cx_GPIO_PORT     GPIOB
#define I2Cx_SCL_PIN       GPIO_Pin_6
#define I2Cx_SDA_PIN       GPIO_Pin_7

#define MLX90640_I2C_RX_DMA_STREAM        DMA1_Stream0
#define MLX90640_I2C_RX_DMA_CHANNEL       DMA_Channel_1
#define MLX90640_I2C_RX_DMA_CLK           RCC_AHB1Periph_DMA1
#define MLX90640_I2C_RX_DMA_IRQn          DMA1_Stream0_IRQn
#define MLX90640_I2C_RX_DMA_TC_FLAG       DMA_IT_TCIF0
#define MLX90640_I2C_RX_DMA_TE_FLAG       DMA_IT_TEIF0
#define MLX90640_I2C_RX_DMA_ALL_FLAGS     (DMA_FLAG_FEIF0 | DMA_FLAG_DMEIF0 | DMA_FLAG_TEIF0 | DMA_FLAG_HTIF0 | DMA_FLAG_TCIF0)

#define MLX90640_I2C_ERROR_NACK           (-1)
#define MLX90640_I2C_ERROR_VERIFY         (-2)
#define MLX90640_I2C_ERROR_TIMEOUT        (-3)

#define MLX90640_I2C_BUSY_TIMEOUT_US      5000UL
#define MLX90640_I2C_EVENT_TIMEOUT_US     5000UL
#define MLX90640_I2C_DMA_DISABLE_WAIT_LOOPS 100000UL
#define MLX90640_I2C_RX_BUFFER_BYTES      1664U
#define I2Cx_SPEED                        1000000U

typedef enum
{
    MLX90640_I2C_DMA_STATE_IDLE = 0,
    MLX90640_I2C_DMA_STATE_START_WRITE,
    MLX90640_I2C_DMA_STATE_ADDR_WRITE,
    MLX90640_I2C_DMA_STATE_MEM_HI,
    MLX90640_I2C_DMA_STATE_MEM_LO,
    MLX90640_I2C_DMA_STATE_START_READ,
    MLX90640_I2C_DMA_STATE_ADDR_READ,
    MLX90640_I2C_DMA_STATE_DMA_RX,
    MLX90640_I2C_DMA_STATE_DONE,
    MLX90640_I2C_DMA_STATE_ERROR
} mlx90640_i2c_dma_state_t;

typedef struct
{
    volatile uint8_t active;
    volatile uint8_t recovery_pending;
    volatile uint8_t slave_addr;
    volatile uint16_t start_address;
    volatile uint16_t byte_count;
    volatile uint32_t transaction_seq;
    volatile uint32_t completed_seq;
    volatile int result;
    volatile app_perf_i2c_error_t error_kind;
    volatile mlx90640_i2c_dma_state_t state;
} mlx90640_i2c_dma_context_t;

/* DMA RX buffer must stay in normal SRAM, not stack/CCM. */
__attribute__((section("dma_sram"), aligned(4))) static uint8_t s_mlx90640_i2c_rx_buffer[MLX90640_I2C_RX_BUFFER_BYTES];

static SemaphoreHandle_t s_mlx90640_i2c_bus_mutex = 0;
static SemaphoreHandle_t s_mlx90640_i2c_done_sem = 0;
static uint8_t s_mlx90640_i2c_runtime_ready = 0U;
static mlx90640_i2c_dma_context_t s_mlx90640_i2c_dma_ctx;

static uint8_t MLX90640_I2CSchedulerRunning(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

static void MLX90640_I2CTimerInit(void)
{
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

static uint32_t MLX90640_I2CElapsedUs(uint32_t start_cycle)
{
    uint32_t cycles_per_us = SystemCoreClock / 1000000UL;

    if (cycles_per_us == 0U)
    {
        cycles_per_us = 1U;
    }

    return (DWT->CYCCNT - start_cycle) / cycles_per_us;
}

static void MLX90640_I2CClearErrorFlags(void)
{
    I2C_ClearITPendingBit(I2Cx, I2C_IT_AF);
    I2C_ClearITPendingBit(I2Cx, I2C_IT_BERR);
    I2C_ClearITPendingBit(I2Cx, I2C_IT_ARLO);
    I2C_ClearITPendingBit(I2Cx, I2C_IT_OVR);
    I2C_ClearITPendingBit(I2Cx, I2C_IT_TIMEOUT);
}

static void MLX90640_I2CClearDmaFlags(void)
{
    DMA_ClearFlag(MLX90640_I2C_RX_DMA_STREAM, MLX90640_I2C_RX_DMA_ALL_FLAGS);
}

static uint16_t MLX90640_I2CReadDmaNdtr(void)
{
    return (uint16_t)DMA_GetCurrDataCounter(MLX90640_I2C_RX_DMA_STREAM);
}

static void MLX90640_I2CCaptureTimeoutSnapshot(void)
{
    app_perf_baseline_record_i2c_dma_timeout_snapshot(MLX90640_I2CReadDmaNdtr(),
                                                      (uint8_t)s_mlx90640_i2c_dma_ctx.state,
                                                      I2Cx->SR1,
                                                      I2Cx->SR2);
}

static void MLX90640_I2CCaptureTcSnapshot(void)
{
    app_perf_baseline_record_i2c_dma_tc_snapshot(MLX90640_I2CReadDmaNdtr(),
                                                 (uint8_t)s_mlx90640_i2c_dma_ctx.state,
                                                 I2Cx->SR1,
                                                 I2Cx->SR2);
}

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

static void MLX90640_I2CStopRequestsNoReinit(void)
{
    I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, DISABLE);
    I2C_DMACmd(I2Cx, DISABLE);
    I2C_DMALastTransferCmd(I2Cx, DISABLE);
    MLX90640_I2CDmaDisableStream();
    MLX90640_I2CClearDmaFlags();
}

static void MLX90640_I2CAbortTransferTaskContext(void)
{
    MLX90640_I2CStopRequestsNoReinit();
    I2C_GenerateSTOP(I2Cx, ENABLE);
    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    MLX90640_I2CClearErrorFlags();
    memset(&s_mlx90640_i2c_dma_ctx, 0, sizeof(s_mlx90640_i2c_dma_ctx));
    MLX90640_I2CInit();
}

static void MLX90640_I2CFinishDmaSuccessTaskContext(void)
{
    MLX90640_I2CStopRequestsNoReinit();
    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    memset(&s_mlx90640_i2c_dma_ctx, 0, sizeof(s_mlx90640_i2c_dma_ctx));
}

static int MLX90640_I2CMapTransportError(app_perf_i2c_error_t error_kind)
{
    if (error_kind == APP_PERF_I2C_ERROR_AF)
    {
        return MLX90640_I2C_ERROR_NACK;
    }

    return MLX90640_I2C_ERROR_TIMEOUT;
}

static int MLX90640_I2CHandlePollingErrorFlags(void)
{
    uint32_t sr1 = I2Cx->SR1;

    if ((sr1 & I2C_SR1_AF) != 0U)
    {
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_AF);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_NACK;
    }
    if ((sr1 & I2C_SR1_BERR) != 0U)
    {
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_BERR);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }
    if ((sr1 & I2C_SR1_ARLO) != 0U)
    {
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_ARLO);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }
    if ((sr1 & I2C_SR1_OVR) != 0U)
    {
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_OVR);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }
    if ((sr1 & I2C_SR1_TIMEOUT) != 0U)
    {
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_TIMEOUT);
        MLX90640_I2CClearErrorFlags();
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    return 0;
}

static int MLX90640_I2CWaitWhileBusy(void)
{
    uint32_t start_cycle = 0U;

    MLX90640_I2CTimerInit();
    start_cycle = DWT->CYCCNT;

    while (I2C_GetFlagStatus(I2Cx, I2C_FLAG_BUSY) != RESET)
    {
        if (MLX90640_I2CElapsedUs(start_cycle) >= MLX90640_I2C_BUSY_TIMEOUT_US)
        {
            app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_BUSY_STUCK);
            MLX90640_I2CAbortTransferTaskContext();
            return MLX90640_I2C_ERROR_TIMEOUT;
        }
    }

    return 0;
}

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
            app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_TIMEOUT);
            MLX90640_I2CAbortTransferTaskContext();
            return MLX90640_I2C_ERROR_TIMEOUT;
        }
    }

    return 0;
}

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

static int MLX90640_I2CReadPollingLocked(uint8_t slaveAddr,
                                         uint16_t startAddress,
                                         uint16_t nMemAddressRead,
                                         uint16_t *data)
{
    uint8_t sa = (uint8_t)(slaveAddr << 1U);
    uint8_t cmd[2];
    uint16_t byte_count = (uint16_t)(nMemAddressRead << 1U);
    uint16_t index = 0U;
    int error = 0;

    if (data == 0 || byte_count > MLX90640_I2C_RX_BUFFER_BYTES)
    {
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    cmd[0] = (uint8_t)(startAddress >> 8U);
    cmd[1] = (uint8_t)(startAddress & 0xFFU);

    error = MLX90640_I2CWaitWhileBusy();
    if (error != 0)
    {
        return error;
    }

    I2C_GenerateSTART(I2Cx, ENABLE);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);
    if (error != 0)
    {
        return error;
    }

    I2C_Send7bitAddress(I2Cx, sa, I2C_Direction_Transmitter);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED);
    if (error != 0)
    {
        return error;
    }

    I2C_SendData(I2Cx, cmd[0]);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    if (error != 0)
    {
        return error;
    }

    I2C_SendData(I2Cx, cmd[1]);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    if (error != 0)
    {
        return error;
    }

    I2C_GenerateSTART(I2Cx, ENABLE);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);
    if (error != 0)
    {
        return error;
    }

    I2C_Send7bitAddress(I2Cx, (uint8_t)(sa | 0x01U), I2C_Direction_Receiver);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED);
    if (error != 0)
    {
        return error;
    }

    for (index = 0U; index < byte_count; ++index)
    {
        if (index == (uint16_t)(byte_count - 1U))
        {
            I2C_AcknowledgeConfig(I2Cx, DISABLE);
            I2C_GenerateSTOP(I2Cx, ENABLE);
        }

        error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_RECEIVED);
        if (error != 0)
        {
            return error;
        }

        s_mlx90640_i2c_rx_buffer[index] = I2C_ReceiveData(I2Cx);
    }

    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    MLX90640_I2CConvertBytesToWords(nMemAddressRead, data);
    return 0;
}

static int MLX90640_I2CWritePollingLocked(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    uint8_t sa = (uint8_t)(slaveAddr << 1U);
    uint8_t cmd[4];
    uint16_t dataCheck = 0U;
    int error = 0;
    int index = 0;

    cmd[0] = (uint8_t)(writeAddress >> 8U);
    cmd[1] = (uint8_t)(writeAddress & 0xFFU);
    cmd[2] = (uint8_t)(data >> 8U);
    cmd[3] = (uint8_t)(data & 0xFFU);

    error = MLX90640_I2CWaitWhileBusy();
    if (error != 0)
    {
        return error;
    }

    I2C_GenerateSTART(I2Cx, ENABLE);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);
    if (error != 0)
    {
        return error;
    }

    I2C_Send7bitAddress(I2Cx, sa, I2C_Direction_Transmitter);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED);
    if (error != 0)
    {
        return error;
    }

    for (index = 0; index < 4; ++index)
    {
        I2C_SendData(I2Cx, cmd[index]);
        error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
        if (error != 0)
        {
            return error;
        }
    }

    I2C_GenerateSTOP(I2Cx, ENABLE);

    if (writeAddress == 0x8000U)
    {
        return 0;
    }

    error = MLX90640_I2CReadPollingLocked(slaveAddr, writeAddress, 1U, &dataCheck);
    if (error != 0)
    {
        return error;
    }

    return (dataCheck == data) ? 0 : MLX90640_I2C_ERROR_VERIFY;
}

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

static void MLX90640_I2CDmaPrepareContext(uint8_t slaveAddr, uint16_t startAddress, uint16_t byte_count)
{
    s_mlx90640_i2c_dma_ctx.transaction_seq++;
    if (s_mlx90640_i2c_dma_ctx.transaction_seq == 0U)
    {
        s_mlx90640_i2c_dma_ctx.transaction_seq = 1U;
    }

    s_mlx90640_i2c_dma_ctx.active = 1U;
    s_mlx90640_i2c_dma_ctx.recovery_pending = 0U;
    s_mlx90640_i2c_dma_ctx.slave_addr = (uint8_t)(slaveAddr << 1U);
    s_mlx90640_i2c_dma_ctx.start_address = startAddress;
    s_mlx90640_i2c_dma_ctx.byte_count = byte_count;
    s_mlx90640_i2c_dma_ctx.result = MLX90640_I2C_ERROR_TIMEOUT;
    s_mlx90640_i2c_dma_ctx.error_kind = APP_PERF_I2C_ERROR_TIMEOUT;
    s_mlx90640_i2c_dma_ctx.completed_seq = 0U;
    s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_START_WRITE;
}

static void MLX90640_I2CDmaSignalDoneFromISR(int result,
                                             app_perf_i2c_error_t error_kind,
                                             mlx90640_i2c_dma_state_t state)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (s_mlx90640_i2c_dma_ctx.active == 0U)
    {
        return;
    }

    if (s_mlx90640_i2c_dma_ctx.completed_seq == s_mlx90640_i2c_dma_ctx.transaction_seq)
    {
        return;
    }

    s_mlx90640_i2c_dma_ctx.result = result;
    s_mlx90640_i2c_dma_ctx.error_kind = error_kind;
    s_mlx90640_i2c_dma_ctx.recovery_pending = (result == 0) ? 0U : 1U;
    s_mlx90640_i2c_dma_ctx.state = state;
    s_mlx90640_i2c_dma_ctx.completed_seq = s_mlx90640_i2c_dma_ctx.transaction_seq;

    if (s_mlx90640_i2c_done_sem != 0)
    {
        xSemaphoreGiveFromISR(s_mlx90640_i2c_done_sem, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

static void MLX90640_I2CDmaStopRequestsFromISR(void)
{
    I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, DISABLE);
    I2C_DMACmd(I2Cx, DISABLE);
    I2C_DMALastTransferCmd(I2Cx, DISABLE);
    DMA_Cmd(MLX90640_I2C_RX_DMA_STREAM, DISABLE);
    MLX90640_I2CClearDmaFlags();
}

static int MLX90640_I2CWaitForDmaCompletion(uint32_t transaction_seq)
{
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(REDPIC1_MLX90640_I2C_DMA_TIMEOUT_MS);

    while (1)
    {
        TickType_t now_ticks = xTaskGetTickCount();
        TickType_t elapsed_ticks = now_ticks - start_ticks;
        TickType_t remaining_ticks = 0U;

        if (s_mlx90640_i2c_dma_ctx.completed_seq == transaction_seq)
        {
            return 1;
        }

        if (elapsed_ticks >= timeout_ticks)
        {
            if (s_mlx90640_i2c_dma_ctx.completed_seq == transaction_seq)
            {
                return 1;
            }
            return 0;
        }

        remaining_ticks = timeout_ticks - elapsed_ticks;
        if (xSemaphoreTake(s_mlx90640_i2c_done_sem, remaining_ticks) != pdPASS)
        {
            if (s_mlx90640_i2c_dma_ctx.completed_seq == transaction_seq)
            {
                return 1;
            }
            return 0;
        }

        if (s_mlx90640_i2c_dma_ctx.completed_seq == transaction_seq)
        {
            return 1;
        }
    }
}

static int MLX90640_I2CReadDmaLocked(uint8_t slaveAddr,
                                     uint16_t startAddress,
                                     uint16_t nMemAddressRead,
                                     uint16_t *data)
{
    uint16_t byte_count = (uint16_t)(nMemAddressRead << 1U);
    uint32_t transaction_seq = 0U;
    int error = 0;

    if (data == 0 || byte_count > MLX90640_I2C_RX_BUFFER_BYTES)
    {
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    error = MLX90640_I2CWaitWhileBusy();
    if (error != 0)
    {
        return error;
    }

    MLX90640_I2CDrainDoneSemaphore();
    MLX90640_I2CClearErrorFlags();
    MLX90640_I2CClearDmaFlags();
    MLX90640_I2CDmaDisableStream();

    memset(s_mlx90640_i2c_rx_buffer, 0, byte_count);
    MLX90640_I2CDmaPrepareContext(slaveAddr, startAddress, byte_count);
    transaction_seq = s_mlx90640_i2c_dma_ctx.transaction_seq;

    MLX90640_I2C_RX_DMA_STREAM->M0AR = (uint32_t)s_mlx90640_i2c_rx_buffer;
    DMA_SetCurrDataCounter(MLX90640_I2C_RX_DMA_STREAM, byte_count);

    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR, ENABLE);
    I2C_GenerateSTART(I2Cx, ENABLE);

    if (MLX90640_I2CWaitForDmaCompletion(transaction_seq) == 0)
    {
        MLX90640_I2CCaptureTimeoutSnapshot();
        app_perf_baseline_record_i2c_transport_error(APP_PERF_I2C_ERROR_TIMEOUT);
        MLX90640_I2CAbortTransferTaskContext();
        return MLX90640_I2C_ERROR_TIMEOUT;
    }

    if (s_mlx90640_i2c_dma_ctx.result != 0)
    {
        error = MLX90640_I2CMapTransportError(s_mlx90640_i2c_dma_ctx.error_kind);
        MLX90640_I2CAbortTransferTaskContext();
        return error;
    }

    MLX90640_I2CFinishDmaSuccessTaskContext();
    MLX90640_I2CConvertBytesToWords(nMemAddressRead, data);
    return 0;
}

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

static int MLX90640_I2CWriteLocked(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    return MLX90640_I2CWritePollingLocked(slaveAddr, writeAddress, data);
}

static uint8_t MLX90640_I2CLockBus(void)
{
    if (MLX90640_I2CSchedulerRunning() == 0U || s_mlx90640_i2c_bus_mutex == 0)
    {
        return 0U;
    }

    (void)xSemaphoreTake(s_mlx90640_i2c_bus_mutex, portMAX_DELAY);
    return 1U;
}

static void MLX90640_I2CUnlockBus(uint8_t locked)
{
    if (locked != 0U && s_mlx90640_i2c_bus_mutex != 0)
    {
        (void)xSemaphoreGive(s_mlx90640_i2c_bus_mutex);
    }
}

uint8_t MLX90640_I2CRuntimeInit(void)
{
    if (s_mlx90640_i2c_runtime_ready != 0U)
    {
        return 1U;
    }

    s_mlx90640_i2c_bus_mutex = xSemaphoreCreateMutex();
    s_mlx90640_i2c_done_sem = xSemaphoreCreateBinary();
    if (s_mlx90640_i2c_bus_mutex == 0 || s_mlx90640_i2c_done_sem == 0)
    {
        return 0U;
    }

    MLX90640_I2CDrainDoneSemaphore();
    s_mlx90640_i2c_runtime_ready = 1U;
    return 1U;
}

void MLX90640_I2CInit(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    I2C_InitTypeDef I2C_InitStruct;
    DMA_InitTypeDef DMA_InitStruct;
    NVIC_InitTypeDef NVIC_InitStruct;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(I2Cx_CLK, ENABLE);
    RCC_AHB1PeriphClockCmd(MLX90640_I2C_RX_DMA_CLK, ENABLE);

    GPIO_PinAFConfig(GPIOB, GPIO_PinSource6, GPIO_AF_I2C1);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource7, GPIO_AF_I2C1);

    GPIO_InitStruct.GPIO_Pin = I2Cx_SCL_PIN | I2Cx_SDA_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_OD;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(I2Cx_GPIO_PORT, &GPIO_InitStruct);

    RCC_APB1PeriphResetCmd(I2Cx_CLK, ENABLE);
    RCC_APB1PeriphResetCmd(I2Cx_CLK, DISABLE);

    I2C_StructInit(&I2C_InitStruct);
    I2C_InitStruct.I2C_Mode = I2C_Mode_I2C;
    I2C_InitStruct.I2C_DutyCycle = I2C_DutyCycle_2;
    I2C_InitStruct.I2C_ClockSpeed = I2Cx_SPEED;
    I2C_InitStruct.I2C_OwnAddress1 = 0x00;
    I2C_InitStruct.I2C_Ack = I2C_Ack_Enable;
    I2C_InitStruct.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(I2Cx, &I2C_InitStruct);
    I2C_Cmd(I2Cx, ENABLE);
    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    I2C_DMALastTransferCmd(I2Cx, DISABLE);
    I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, DISABLE);
    MLX90640_I2CClearErrorFlags();

    DMA_DeInit(MLX90640_I2C_RX_DMA_STREAM);
    MLX90640_I2CDmaDisableStream();
    DMA_StructInit(&DMA_InitStruct);
    DMA_InitStruct.DMA_Channel = MLX90640_I2C_RX_DMA_CHANNEL;
    DMA_InitStruct.DMA_PeripheralBaseAddr = (uint32_t)&I2Cx->DR;
    DMA_InitStruct.DMA_Memory0BaseAddr = (uint32_t)s_mlx90640_i2c_rx_buffer;
    DMA_InitStruct.DMA_DIR = DMA_DIR_PeripheralToMemory;
    DMA_InitStruct.DMA_BufferSize = MLX90640_I2C_RX_BUFFER_BYTES;
    DMA_InitStruct.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStruct.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStruct.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStruct.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStruct.DMA_Mode = DMA_Mode_Normal;
    DMA_InitStruct.DMA_Priority = DMA_Priority_VeryHigh;
    DMA_InitStruct.DMA_FIFOMode = DMA_FIFOMode_Disable;
    DMA_InitStruct.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;
    DMA_InitStruct.DMA_MemoryBurst = DMA_MemoryBurst_Single;
    DMA_InitStruct.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    DMA_Init(MLX90640_I2C_RX_DMA_STREAM, &DMA_InitStruct);
    DMA_ITConfig(MLX90640_I2C_RX_DMA_STREAM, DMA_IT_TC | DMA_IT_TE, ENABLE);
    MLX90640_I2CClearDmaFlags();

    NVIC_InitStruct.NVIC_IRQChannel = I2C1_ER_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    NVIC_InitStruct.NVIC_IRQChannel = I2C1_EV_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 6;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_Init(&NVIC_InitStruct);

    NVIC_InitStruct.NVIC_IRQChannel = MLX90640_I2C_RX_DMA_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_Init(&NVIC_InitStruct);
}

int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress, uint16_t nMemAddressRead, uint16_t *data)
{
    uint8_t locked = MLX90640_I2CLockBus();
    int result = MLX90640_I2CReadLocked(slaveAddr, startAddress, nMemAddressRead, data);

    MLX90640_I2CUnlockBus(locked);
    return result;
}

int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    uint8_t locked = MLX90640_I2CLockBus();
    int result = MLX90640_I2CWriteLocked(slaveAddr, writeAddress, data);

    MLX90640_I2CUnlockBus(locked);
    return result;
}

void I2C1_EV_IRQHandler(void)
{
#if (REDPIC1_MLX90640_I2C_DMA_ENABLE != 0U)
    if (s_mlx90640_i2c_dma_ctx.active == 0U)
    {
        return;
    }

    app_perf_baseline_record_i2c_dma_ev_irq();

    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_START_WRITE) &&
        (I2C_GetITStatus(I2Cx, I2C_IT_SB) != RESET))
    {
        I2C_Send7bitAddress(I2Cx, s_mlx90640_i2c_dma_ctx.slave_addr, I2C_Direction_Transmitter);
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_ADDR_WRITE;
        return;
    }

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

    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_MEM_HI) &&
        ((I2C_GetITStatus(I2Cx, I2C_IT_TXE) != RESET) ||
         (I2C_GetITStatus(I2Cx, I2C_IT_BTF) != RESET)))
    {
        I2C_SendData(I2Cx, (uint8_t)(s_mlx90640_i2c_dma_ctx.start_address & 0xFFU));
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_MEM_LO;
        return;
    }

    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_MEM_LO) &&
        (I2C_GetITStatus(I2Cx, I2C_IT_BTF) != RESET))
    {
        I2C_GenerateSTART(I2Cx, ENABLE);
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_START_READ;
        return;
    }

    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_START_READ) &&
        (I2C_GetITStatus(I2Cx, I2C_IT_SB) != RESET))
    {
        I2C_Send7bitAddress(I2Cx,
                            (uint8_t)(s_mlx90640_i2c_dma_ctx.slave_addr | 0x01U),
                            I2C_Direction_Receiver);
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_ADDR_READ;
        return;
    }

    if ((s_mlx90640_i2c_dma_ctx.state == MLX90640_I2C_DMA_STATE_ADDR_READ) &&
        (I2C_GetITStatus(I2Cx, I2C_IT_ADDR) != RESET))
    {
        volatile uint32_t dummy = I2Cx->SR1;

        (void)dummy;
        I2C_AcknowledgeConfig(I2Cx, ENABLE);
        I2C_DMALastTransferCmd(I2Cx, ENABLE);
        MLX90640_I2CClearDmaFlags();
        MLX90640_I2C_RX_DMA_STREAM->M0AR = (uint32_t)s_mlx90640_i2c_rx_buffer;
        DMA_SetCurrDataCounter(MLX90640_I2C_RX_DMA_STREAM, s_mlx90640_i2c_dma_ctx.byte_count);
        DMA_Cmd(MLX90640_I2C_RX_DMA_STREAM, ENABLE);
        I2C_DMACmd(I2Cx, ENABLE);
        dummy = I2Cx->SR2;
        (void)dummy;
        /* Once DMA RX owns the transfer, keep only ERR enabled.
         * Leaving EVT/BUF on here can create an IRQ storm with no
         * matching DMA_RX handler branch and eventually starve DMA TC. */
        I2C_ITConfig(I2Cx, I2C_IT_EVT | I2C_IT_BUF, DISABLE);
        s_mlx90640_i2c_dma_ctx.state = MLX90640_I2C_DMA_STATE_DMA_RX;
        return;
    }
#endif
}

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

    if (I2C_GetITStatus(I2Cx, I2C_IT_AF) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_AF;
        has_error = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_AF);
    }
    else if (I2C_GetITStatus(I2Cx, I2C_IT_BERR) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_BERR;
        has_error = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_BERR);
    }
    else if (I2C_GetITStatus(I2Cx, I2C_IT_ARLO) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_ARLO;
        has_error = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_ARLO);
    }
    else if (I2C_GetITStatus(I2Cx, I2C_IT_OVR) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_OVR;
        has_error = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_OVR);
    }
    else if (I2C_GetITStatus(I2Cx, I2C_IT_TIMEOUT) != RESET)
    {
        error_kind = APP_PERF_I2C_ERROR_TIMEOUT;
        has_error = 1U;
        I2C_ClearITPendingBit(I2Cx, I2C_IT_TIMEOUT);
    }

    if (has_error == 0U)
    {
        return;
    }

    MLX90640_I2CCaptureTimeoutSnapshot();
    app_perf_baseline_record_i2c_transport_error(error_kind);
    MLX90640_I2CDmaStopRequestsFromISR();
    MLX90640_I2CDmaSignalDoneFromISR(MLX90640_I2CMapTransportError(error_kind),
                                     error_kind,
                                     MLX90640_I2C_DMA_STATE_ERROR);
#endif
}

void DMA1_Stream0_IRQHandler(void)
{
#if (REDPIC1_MLX90640_I2C_DMA_ENABLE != 0U)
    if (DMA_GetITStatus(MLX90640_I2C_RX_DMA_STREAM, MLX90640_I2C_RX_DMA_TC_FLAG) != RESET)
    {
        app_perf_baseline_record_i2c_dma_tc_irq();
        MLX90640_I2CCaptureTcSnapshot();
        DMA_ClearITPendingBit(MLX90640_I2C_RX_DMA_STREAM, MLX90640_I2C_RX_DMA_TC_FLAG);
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
