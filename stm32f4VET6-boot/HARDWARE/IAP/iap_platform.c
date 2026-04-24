#include "iap_platform.h"
#include "stm32f4xx_iwdg.h"
#include "stm32f4xx_rcc.h"

void iap_init_watchdog(void)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_PRESCALER);
    IWDG_SetReload(IWDG_RELOAD);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

void iap_feed_watchdog(void)
{
    IWDG_ReloadCounter();
}

uint8_t is_app_valid(uint32_t app_addr)
{
    uint32_t stack_addr = *(volatile uint32_t *)app_addr;
    return ((stack_addr & 0x2FFE0000U) == 0x20000000U) ? 1U : 0U;
}

uint8_t get_reset_reason(void)
{
    if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET ||
        RCC_GetFlagStatus(RCC_FLAG_WWDGRST) != RESET)
    {
        return BOOT_REASON_IWDG;
    }

    if (RCC_GetFlagStatus(RCC_FLAG_BORRST) != RESET ||
        RCC_GetFlagStatus(RCC_FLAG_PORRST) != RESET ||
        RCC_GetFlagStatus(RCC_FLAG_PINRST) != RESET)
    {
        return BOOT_REASON_POWER;
    }

    if (RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET)
    {
        return BOOT_REASON_SOFTWARE;
    }

    return BOOT_REASON_NORMAL;
}

void clear_reset_flags(void)
{
    RCC_ClearFlag();
}

uint32_t Send_Byte(uint8_t c)
{
    SerialPutChar(c);
    return 0U;
}

void iap_cleanup_before_jump(void)
{
    uint32_t index = 0U;

    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    for (index = 0U; index < 8U; ++index)
    {
        NVIC->ICER[index] = 0xFFFFFFFFU;
        NVIC->ICPR[index] = 0xFFFFFFFFU;
    }
}

void jump_to_app(uint32_t app_addr)
{
    uint32_t app_stack = 0U;
    uint32_t app_reset = 0U;
    void (*jump_func)(void) = 0;

    app_stack = *(volatile uint32_t *)app_addr;
    if (is_app_valid(app_addr) == 0U)
    {
        return;
    }

    app_reset = *(volatile uint32_t *)(app_addr + 4U);

    iap_cleanup_before_jump();
    SCB->VTOR = app_addr;
    __set_MSP(app_stack);

    jump_func = (void (*)(void))app_reset;
    jump_func();

    while (1)
    {
    }
}
