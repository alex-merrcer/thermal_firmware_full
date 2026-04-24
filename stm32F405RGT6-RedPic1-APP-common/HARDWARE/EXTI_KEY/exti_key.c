#include "exti_key.h"

#include "power_manager.h"

#define KEY1_GPIO_PORT    GPIOB
#define KEY1_GPIO_PIN     GPIO_Pin_8
#define KEY1_PIN_SOURCE   GPIO_PinSource8
#define KEY1_EXTI_LINE    EXTI_Line8

#define KEY2_GPIO_PORT    GPIOB
#define KEY2_GPIO_PIN     GPIO_Pin_9
#define KEY2_PIN_SOURCE   GPIO_PinSource9
#define KEY2_EXTI_LINE    EXTI_Line9

#define KEY3_GPIO_PORT    GPIOC
#define KEY3_GPIO_PIN     GPIO_Pin_13
#define KEY3_PIN_SOURCE   GPIO_PinSource13
#define KEY3_EXTI_LINE    EXTI_Line13

#define DEBOUNCE_MS 20U
#define KEY_EXTI_HEALTH_TIMEOUT_MS 200U
#define KEY_EVENT_QUEUE_SIZE 16U

static volatile uint8_t g_key_queue[KEY_EVENT_QUEUE_SIZE];
static volatile uint8_t g_key_queue_head = 0U;
static volatile uint8_t g_key_queue_tail = 0U;
static volatile uint32_t g_exti_pending_mask = 0U;
static volatile uint8_t g_debouncing = 0U;
static volatile uint32_t g_debounce_start_ms = 0U;

__weak void KEY_EXTI_OnEventQueuedFromISR(void)
{
}

static uint8_t key_queue_push_isr(uint8_t key_value)
{
    uint8_t next_head = (uint8_t)((g_key_queue_head + 1U) % KEY_EVENT_QUEUE_SIZE);

    if (next_head == g_key_queue_tail)
    {
        g_key_queue_tail = (uint8_t)((g_key_queue_tail + 1U) % KEY_EVENT_QUEUE_SIZE);
    }

    g_key_queue[g_key_queue_head] = key_value;
    g_key_queue_head = next_head;
    return 1U;
}

static uint8_t key_queue_push(uint8_t key_value)
{
    uint8_t ok = 0U;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    ok = key_queue_push_isr(key_value);
    if (primask == 0U)
    {
        __enable_irq();
    }

    return ok;
}

static uint8_t key_queue_pop(uint8_t *key_value)
{
    uint8_t ok = 0U;
    uint32_t primask = 0U;

    if (key_value == 0)
    {
        return 0U;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    if (g_key_queue_head != g_key_queue_tail)
    {
        *key_value = g_key_queue[g_key_queue_tail];
        g_key_queue_tail = (uint8_t)((g_key_queue_tail + 1U) % KEY_EVENT_QUEUE_SIZE);
        ok = 1U;
    }
    if (primask == 0U)
    {
        __enable_irq();
    }

    return ok;
}

static uint32_t key_exti_get_apb1_timer_clock_hz(void)
{
    uint32_t ppre1_bits = RCC->CFGR & RCC_CFGR_PPRE1;
    uint32_t hclk_hz = SystemCoreClock;
    uint32_t pclk1_hz = hclk_hz;

    switch (ppre1_bits)
    {
    case RCC_CFGR_PPRE1_DIV2:
        pclk1_hz = hclk_hz / 2U;
        break;
    case RCC_CFGR_PPRE1_DIV4:
        pclk1_hz = hclk_hz / 4U;
        break;
    case RCC_CFGR_PPRE1_DIV8:
        pclk1_hz = hclk_hz / 8U;
        break;
    case RCC_CFGR_PPRE1_DIV16:
        pclk1_hz = hclk_hz / 16U;
        break;
    default:
        pclk1_hz = hclk_hz;
        break;
    }

    return (ppre1_bits == RCC_CFGR_PPRE1_DIV1) ? pclk1_hz : (pclk1_hz * 2U);
}

void KEY_EXTI_ReconfigureDebounceTimer(void)
{
    TIM_TimeBaseInitTypeDef tim_time_base_structure;
    uint32_t timer_clock_hz = key_exti_get_apb1_timer_clock_hz();
    uint32_t prescaler = timer_clock_hz / 1000U;

    if (prescaler == 0U)
    {
        prescaler = 1U;
    }

    TIM_Cmd(TIM3, DISABLE);
    TIM_DeInit(TIM3);

    tim_time_base_structure.TIM_Period = DEBOUNCE_MS - 1U;
    tim_time_base_structure.TIM_Prescaler = (uint16_t)(prescaler - 1U);
    tim_time_base_structure.TIM_ClockDivision = TIM_CKD_DIV1;
    tim_time_base_structure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &tim_time_base_structure);
    TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);
    TIM_SetCounter(TIM3, 0U);

    if (g_debouncing != 0U)
    {
        g_debounce_start_ms = power_manager_get_tick_ms();
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
        TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
        TIM_Cmd(TIM3, ENABLE);
    }
}

void KEY_EXTI_Init(void)
{
    GPIO_InitTypeDef gpio_init_structure;
    EXTI_InitTypeDef exti_init_structure;
    NVIC_InitTypeDef nvic_init_structure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB | RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
    g_key_queue_head = 0U;
    g_key_queue_tail = 0U;

    gpio_init_structure.GPIO_Mode = GPIO_Mode_IN;
    gpio_init_structure.GPIO_PuPd = GPIO_PuPd_UP;
    gpio_init_structure.GPIO_Speed = GPIO_Speed_100MHz;

    gpio_init_structure.GPIO_Pin = KEY1_GPIO_PIN;
    GPIO_Init(KEY1_GPIO_PORT, &gpio_init_structure);

    gpio_init_structure.GPIO_Pin = KEY2_GPIO_PIN;
    GPIO_Init(KEY2_GPIO_PORT, &gpio_init_structure);

    gpio_init_structure.GPIO_Pin = KEY3_GPIO_PIN;
    GPIO_Init(KEY3_GPIO_PORT, &gpio_init_structure);

    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOB, KEY1_PIN_SOURCE);
    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOB, KEY2_PIN_SOURCE);
    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOC, KEY3_PIN_SOURCE);

    exti_init_structure.EXTI_Line = KEY1_EXTI_LINE;
    exti_init_structure.EXTI_Mode = EXTI_Mode_Interrupt;
    exti_init_structure.EXTI_Trigger = EXTI_Trigger_Falling;
    exti_init_structure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti_init_structure);

    exti_init_structure.EXTI_Line = KEY2_EXTI_LINE;
    EXTI_Init(&exti_init_structure);

    exti_init_structure.EXTI_Line = KEY3_EXTI_LINE;
    EXTI_Init(&exti_init_structure);

    nvic_init_structure.NVIC_IRQChannel = EXTI9_5_IRQn;
    nvic_init_structure.NVIC_IRQChannelPreemptionPriority = 1;
    nvic_init_structure.NVIC_IRQChannelSubPriority = 1;
    nvic_init_structure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init_structure);

    nvic_init_structure.NVIC_IRQChannel = EXTI15_10_IRQn;
    nvic_init_structure.NVIC_IRQChannelPreemptionPriority = 1;
    nvic_init_structure.NVIC_IRQChannelSubPriority = 2;
    nvic_init_structure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init_structure);

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    KEY_EXTI_ReconfigureDebounceTimer();

    nvic_init_structure.NVIC_IRQChannel = TIM3_IRQn;
    nvic_init_structure.NVIC_IRQChannelPreemptionPriority = 6;
    nvic_init_structure.NVIC_IRQChannelSubPriority = 0;
    nvic_init_structure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init_structure);
}

void EXTI9_5_IRQHandler(void)
{
    if (EXTI_GetITStatus(KEY1_EXTI_LINE) != RESET)
    {
        EXTI_ClearITPendingBit(KEY1_EXTI_LINE);
        if (g_debouncing == 0U)
        {
            g_debouncing = 1U;
            g_debounce_start_ms = power_manager_get_tick_ms();
            g_exti_pending_mask |= KEY1_EXTI_LINE;
            EXTI->IMR &= ~KEY1_EXTI_LINE;
            TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
            TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
            TIM_Cmd(TIM3, ENABLE);
        }
    }

    if (EXTI_GetITStatus(KEY2_EXTI_LINE) != RESET)
    {
        EXTI_ClearITPendingBit(KEY2_EXTI_LINE);
        if (g_debouncing == 0U)
        {
            g_debouncing = 1U;
            g_debounce_start_ms = power_manager_get_tick_ms();
            g_exti_pending_mask |= KEY2_EXTI_LINE;
            EXTI->IMR &= ~KEY2_EXTI_LINE;
            TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
            TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
            TIM_Cmd(TIM3, ENABLE);
        }
    }
}

void EXTI15_10_IRQHandler(void)
{
    if (EXTI_GetITStatus(KEY3_EXTI_LINE) != RESET)
    {
        EXTI_ClearITPendingBit(KEY3_EXTI_LINE);
        if (g_debouncing == 0U)
        {
            g_debouncing = 1U;
            g_debounce_start_ms = power_manager_get_tick_ms();
            g_exti_pending_mask |= KEY3_EXTI_LINE;
            EXTI->IMR &= ~KEY3_EXTI_LINE;
            TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
            TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
            TIM_Cmd(TIM3, ENABLE);
        }
    }
}

void TIM3_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET)
    {
        uint8_t logical_key = 0U;

        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);

        TIM_Cmd(TIM3, DISABLE);
        TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);

        if ((g_exti_pending_mask & KEY1_EXTI_LINE) != 0U)
        {
            if (GPIO_ReadInputDataBit(KEY1_GPIO_PORT, KEY1_GPIO_PIN) == Bit_RESET)
            {
                logical_key = KEY3_PRES;
            }
            EXTI->IMR |= KEY1_EXTI_LINE;
            g_exti_pending_mask &= ~KEY1_EXTI_LINE;
        }

        if ((logical_key == 0U) && ((g_exti_pending_mask & KEY2_EXTI_LINE) != 0U))
        {
            if (GPIO_ReadInputDataBit(KEY2_GPIO_PORT, KEY2_GPIO_PIN) == Bit_RESET)
            {
                logical_key = KEY2_PRES;
            }
            EXTI->IMR |= KEY2_EXTI_LINE;
            g_exti_pending_mask &= ~KEY2_EXTI_LINE;
        }

        if ((logical_key == 0U) && ((g_exti_pending_mask & KEY3_EXTI_LINE) != 0U))
        {
            if (GPIO_ReadInputDataBit(KEY3_GPIO_PORT, KEY3_GPIO_PIN) == Bit_RESET)
            {
                logical_key = KEY1_PRES;
            }
            EXTI->IMR |= KEY3_EXTI_LINE;
            g_exti_pending_mask &= ~KEY3_EXTI_LINE;
        }

        g_debouncing = 0U;
        g_debounce_start_ms = 0U;

        if (logical_key != 0U)
        {
            power_manager_notify_activity();
            (void)key_queue_push_isr(logical_key);
            KEY_EXTI_OnEventQueuedFromISR();
        }
    }
}

uint8_t KEY_EXTI_IsHealthy(void)
{
    if (g_debouncing == 0U)
    {
        return 1U;
    }
    return ((power_manager_get_tick_ms() - g_debounce_start_ms) <= KEY_EXTI_HEALTH_TIMEOUT_MS) ? 1U : 0U;
}

uint8_t KEY_GetValue(void)
{
    uint8_t key_value = 0U;

    (void)key_queue_pop(&key_value);
    return key_value;
}

void KEY_PushEvent(uint8_t key_value)
{
    if (key_value == 0U)
    {
        return;
    }
    (void)key_queue_push(key_value);
}

uint8_t KEY_IsLogicalPressed(uint8_t key_value)
{
    switch (key_value)
    {
    case KEY1_PRES:
        return (GPIO_ReadInputDataBit(KEY3_GPIO_PORT, KEY3_GPIO_PIN) == Bit_RESET) ? 1U : 0U;

    case KEY2_PRES:
        return (GPIO_ReadInputDataBit(KEY2_GPIO_PORT, KEY2_GPIO_PIN) == Bit_RESET) ? 1U : 0U;

    case KEY3_PRES:
        return (GPIO_ReadInputDataBit(KEY1_GPIO_PORT, KEY1_GPIO_PIN) == Bit_RESET) ? 1U : 0U;

    default:
        return 0U;
    }
}
