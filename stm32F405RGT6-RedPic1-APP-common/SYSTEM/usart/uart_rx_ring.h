#ifndef UART_RX_RING_H
#define UART_RX_RING_H

#include "stm32f4xx.h"

#define UART_RX_RING_SIZE 512U

#define UART_RX_RING_FLAG_OVERFLOW 0x00000001UL
#define UART_RX_RING_FLAG_ORE      0x00000002UL
#define UART_RX_RING_FLAG_FE       0x00000004UL
#define UART_RX_RING_FLAG_NE       0x00000008UL

void uart_rx_ring_reset(void);
uint8_t uart_rx_ring_pop(uint8_t *byte);
void uart_rx_ring_push_isr(uint8_t byte);
void uart_rx_ring_record_error_isr(uint32_t flags);
uint32_t uart_rx_ring_take_error_flags(void);
uint32_t uart_rx_ring_peek_error_flags(void);

#endif
