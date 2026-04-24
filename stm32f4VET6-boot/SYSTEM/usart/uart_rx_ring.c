#include "uart_rx_ring.h"

static volatile uint16_t s_uart_rx_head = 0U;
static volatile uint16_t s_uart_rx_tail = 0U;
static volatile uint32_t s_uart_rx_error_flags = 0U;
static uint8_t s_uart_rx_buffer[UART_RX_RING_SIZE];

void uart_rx_ring_reset(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    s_uart_rx_head = 0U;
    s_uart_rx_tail = 0U;
    s_uart_rx_error_flags = 0U;
    if (primask == 0U)
    {
        __enable_irq();
    }
}

uint8_t uart_rx_ring_pop(uint8_t *byte)
{
    uint8_t ok = 0U;
    uint32_t primask = 0U;

    if (byte == 0)
    {
        return 0U;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    if (s_uart_rx_head != s_uart_rx_tail)
    {
        *byte = s_uart_rx_buffer[s_uart_rx_tail];
        s_uart_rx_tail = (uint16_t)((s_uart_rx_tail + 1U) % UART_RX_RING_SIZE);
        ok = 1U;
    }
    if (primask == 0U)
    {
        __enable_irq();
    }

    return ok;
}

void uart_rx_ring_push_isr(uint8_t byte)
{
    uint16_t next_head = (uint16_t)((s_uart_rx_head + 1U) % UART_RX_RING_SIZE);

    if (next_head == s_uart_rx_tail)
    {
        s_uart_rx_error_flags |= UART_RX_RING_FLAG_OVERFLOW;
        return;
    }

    s_uart_rx_buffer[s_uart_rx_head] = byte;
    s_uart_rx_head = next_head;
}

void uart_rx_ring_record_error_isr(uint32_t flags)
{
    s_uart_rx_error_flags |= flags;
}

uint32_t uart_rx_ring_take_error_flags(void)
{
    uint32_t flags = 0U;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    flags = s_uart_rx_error_flags;
    s_uart_rx_error_flags = 0U;
    if (primask == 0U)
    {
        __enable_irq();
    }

    return flags;
}

uint32_t uart_rx_ring_peek_error_flags(void)
{
    uint32_t flags = 0U;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    flags = s_uart_rx_error_flags;
    if (primask == 0U)
    {
        __enable_irq();
    }

    return flags;
}
