#include "stm32f4xx_it.h"
#include "stm32f4xx_usart.h"
#include "rtc_lp_service.h"
#include "uart_rx_ring.h"

void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
    while (1)
    {
    }
}

void MemManage_Handler(void)
{
    while (1)
    {
    }
}

void BusFault_Handler(void)
{
    while (1)
    {
    }
}

void UsageFault_Handler(void)
{
    while (1)
    {
    }
}

//void SVC_Handler(void)
//{
//}

void DebugMon_Handler(void)
{
}

//void PendSV_Handler(void)
//{
//}

//void SysTick_Handler(void)
//{
//}

void USART1_IRQHandler(void)
{
    uint16_t status = USART1->SR;

    if ((status & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0U)
    {
        uint8_t data = (uint8_t)USART1->DR;
        uint32_t error_flags = 0U;

        if ((status & USART_SR_ORE) != 0U)
        {
            error_flags |= UART_RX_RING_FLAG_ORE;
        }
        if ((status & USART_SR_FE) != 0U)
        {
            error_flags |= UART_RX_RING_FLAG_FE;
        }
        if ((status & USART_SR_NE) != 0U)
        {
            error_flags |= UART_RX_RING_FLAG_NE;
        }

        if (error_flags != 0U)
        {
            uart_rx_ring_record_error_isr(error_flags);
        }
        else if ((status & USART_SR_RXNE) != 0U)
        {
            uart_rx_ring_push_isr(data);
        }
    }
}

void RTC_WKUP_IRQHandler(void)
{
    rtc_lp_handle_irq();
}
