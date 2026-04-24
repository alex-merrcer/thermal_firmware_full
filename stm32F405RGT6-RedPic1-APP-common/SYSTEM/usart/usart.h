#ifndef __USART_H
#define __USART_H

#include "stdio.h"
#include "stm32f4xx_conf.h"
#include "sys.h"

void uart_init(u32 bound);
void uart_reinit_current_baud(void);
u32 uart_get_current_baud(void);

#endif
