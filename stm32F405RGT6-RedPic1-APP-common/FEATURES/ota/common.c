#include "common.h"
#include "uart_rx_ring.h"

uint32_t SerialKeyPressed(uint8_t *key)
{
    if (key == 0)
    {
        return 0;
    }

    return uart_rx_ring_pop(key);
}

void SerialPutChar(uint8_t c)
{
  uint32_t timeout = 500000UL;
  USART_SendData(USART1, c);
  while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
  {
    if (--timeout == 0U) { return; }
  }
}
