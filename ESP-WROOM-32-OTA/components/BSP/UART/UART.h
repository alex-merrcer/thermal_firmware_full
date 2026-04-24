#ifndef __UART_H__
#define __UART_H__

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
/* 引脚和串口定义 */

#define USART_TX_GPIO_PIN GPIO_NUM_1    
#define USART_RX_GPIO_PIN GPIO_NUM_3

#define USART2_TX_GPIO_PIN GPIO_NUM_17
#define USART2_RX_GPIO_PIN GPIO_NUM_16
#define STM32_LINK_UART_BAUD 115200
/* 串口接收相关定义 */
#define RX_BUF_SIZE 1024 /* 环形缓冲区大小 */

void UART0_Init(void);
void UART2_Init(void);
void process_rx_byte(uint8_t byte);
void check_timeout();
#endif
