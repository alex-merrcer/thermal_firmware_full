/**
 * @file    exti_key.c
 * @brief   外部中断按键驱动 —— EXTI + 定时器消抖
 * @note    本模块通过 EXTI 外部中断检测按键下降沿，使用 TIM3 定时器
 *          实现硬件级消抖，消抖完成后读取 GPIO 电平确认按键状态。
 *
 * @par 硬件连接
 *      - KEY1: PB8  → EXTI_Line8  (EXTI9_5_IRQn)
 *      - KEY2: PB9  → EXTI_Line9  (EXTI9_5_IRQn)
 *      - KEY3: PC13 → EXTI_Line13 (EXTI15_10_IRQn)
 *      所有按键配置为上拉输入，下降沿触发。
 *
 * @par 消抖流程
 *      1. EXTI 下降沿中断触发
 *      2. 屏蔽该 EXTI 线（防止重复触发）
 *      3. 启动 TIM3 定时器（20ms 消抖窗口）
 *      4. TIM3 溢出中断中：
 *         - 读取 GPIO 确认按键仍处于按下状态
 *         - 将逻辑键值推入环形队列
 *         - 恢复 EXTI 线使能
 *
 * @par 逻辑键值映射
 *      物理引脚与逻辑键值存在交叉映射：
 *      - KEY1(PB8)  → KEY3_PRES
 *      - KEY2(PB9)  → KEY2_PRES
 *      - KEY3(PC13) → KEY1_PRES
 *
 * @par 事件队列
 *      使用 16 字节环形缓冲区存储按键事件。
 *      队列满时自动丢弃最旧事件（覆盖写入）。
 *      通过 PRIMASK 保护队列的并发访问安全。
 *
 * @par 健康监测
 *      KEY_EXTI_IsHealthy() 检查消抖状态是否超时（>200ms），
 *      用于看门狗检测按键卡死故障。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "exti_key.h"

#include "power_manager.h"

/* =========================================================================
 *  2. 内部宏定义 —— GPIO 与 EXTI 引脚配置
 * ======================================================================= */

/* --- KEY1: PB8 → EXTI_Line8 --- */
#define KEY1_GPIO_PORT    GPIOB
#define KEY1_GPIO_PIN     GPIO_Pin_8
#define KEY1_PIN_SOURCE   GPIO_PinSource8
#define KEY1_EXTI_LINE    EXTI_Line8

/* --- KEY2: PB9 → EXTI_Line9 --- */
#define KEY2_GPIO_PORT    GPIOB
#define KEY2_GPIO_PIN     GPIO_Pin_9
#define KEY2_PIN_SOURCE   GPIO_PinSource9
#define KEY2_EXTI_LINE    EXTI_Line9

/* --- KEY3: PC13 → EXTI_Line13 --- */
#define KEY3_GPIO_PORT    GPIOC
#define KEY3_GPIO_PIN     GPIO_Pin_13
#define KEY3_PIN_SOURCE   GPIO_PinSource13
#define KEY3_EXTI_LINE    EXTI_Line13

/* --- 消抖与队列参数 --- */
#define DEBOUNCE_MS                   20U     /**< 消抖窗口时间（ms）       */
#define KEY_EXTI_HEALTH_TIMEOUT_MS    200U    /**< 健康监测超时（ms）       */
#define KEY_EVENT_QUEUE_SIZE          16U     /**< 按键事件队列深度         */

/* =========================================================================
 *  3. 模块级静态变量
 * ======================================================================= */

/** 按键事件环形缓冲区（ISR 安全） */
static volatile uint8_t  g_key_queue[KEY_EVENT_QUEUE_SIZE];
static volatile uint8_t  g_key_queue_head     = 0U;    /**< 队列写指针         */
static volatile uint8_t  g_key_queue_tail     = 0U;    /**< 队列读指针         */
static volatile uint32_t g_exti_pending_mask  = 0U;    /**< 待消抖的 EXTI 线掩码 */
static volatile uint8_t  g_debouncing         = 0U;    /**< 正在消抖标志       */
static volatile uint32_t g_debounce_start_ms  = 0U;    /**< 消抖开始时间戳     */

/* =========================================================================
 *  4. 弱符号回调 —— 事件入队通知
 * ======================================================================= */

/**
 * @brief  按键事件入队后的回调（弱符号，可由上层覆盖）
 * @note   在 TIM3 中断中按键确认后调用，用于唤醒按键扫描任务。
 */
__weak void KEY_EXTI_OnEventQueuedFromISR(void)
{
}

/* =========================================================================
 *  5. 内部函数实现 —— 环形队列操作
 * ======================================================================= */

/**
 * @brief  向队列推入按键值（ISR 版本，无中断保护）
 * @note   队列满时覆盖最旧事件（推进尾指针）。
 * @param  key_value — 按键值
 * @retval 1 — 始终成功
 */
static uint8_t key_queue_push_isr(uint8_t key_value)
{
    uint8_t next_head = (uint8_t)((g_key_queue_head + 1U) % KEY_EVENT_QUEUE_SIZE);

    /* 队列满时丢弃最旧事件 */
    if (next_head == g_key_queue_tail)
    {
        g_key_queue_tail = (uint8_t)((g_key_queue_tail + 1U) % KEY_EVENT_QUEUE_SIZE);
    }

    g_key_queue[g_key_queue_head] = key_value;
    g_key_queue_head = next_head;
    return 1U;
}

/**
 * @brief  向队列推入按键值（线程安全版本）
 * @note   通过 PRIMASK 保护并发访问。
 * @param  key_value — 按键值
 * @retval 1 — 始终成功
 */
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

/**
 * @brief  从队列弹出按键值（线程安全版本）
 * @param  key_value — 输出：按键值
 * @retval 1 — 弹出成功；0 — 队列为空
 */
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

/* =========================================================================
 *  6. 内部函数实现 —— APB1 定时器时钟计算
 * ======================================================================= */

/**
 * @brief  获取 APB1 定时器时钟频率
 * @note   APB1 预分频 >1 时，定时器时钟为 PCLK1 × 2。
 * @return 定时器时钟频率（Hz）
 */
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

    /* APB1 预分频 >1 时定时器时钟翻倍 */
    return (ppre1_bits == RCC_CFGR_PPRE1_DIV1) ? pclk1_hz : (pclk1_hz * 2U);
}

/* =========================================================================
 *  7. 公共接口实现 —— 消抖定时器重配置
 * ======================================================================= */

/**
 * @brief  重新配置 TIM3 消抖定时器
 * @note   根据当前 APB1 时钟频率计算预分频值，
 *         使定时器以 1kHz 频率计数，溢出周期 = DEBOUNCE_MS。
 *         若正在消抖中则重新启动定时器。
 */
void KEY_EXTI_ReconfigureDebounceTimer(void)
{
    TIM_TimeBaseInitTypeDef tim_time_base_structure;
    uint32_t timer_clock_hz = key_exti_get_apb1_timer_clock_hz();
    uint32_t prescaler = timer_clock_hz / 1000U;

    if (prescaler == 0U)
    {
        prescaler = 1U;
    }

    /* 停止并复位 TIM3 */
    TIM_Cmd(TIM3, DISABLE);
    TIM_DeInit(TIM3);

    /* 配置定时器：向上计数，溢出周期 = DEBOUNCE_MS */
    tim_time_base_structure.TIM_Period        = DEBOUNCE_MS - 1U;
    tim_time_base_structure.TIM_Prescaler     = (uint16_t)(prescaler - 1U);
    tim_time_base_structure.TIM_ClockDivision = TIM_CKD_DIV1;
    tim_time_base_structure.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &tim_time_base_structure);
    TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);
    TIM_SetCounter(TIM3, 0U);

    /* 若正在消抖中，重新启动定时器 */
    if (g_debouncing != 0U)
    {
        g_debounce_start_ms = power_manager_get_tick_ms();
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
        TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
        TIM_Cmd(TIM3, ENABLE);
    }
}

/* =========================================================================
 *  8. 公共接口实现 —— 初始化
 * ======================================================================= */

/**
 * @brief  初始化按键 EXTI 中断和消抖定时器
 * @note   配置内容：
 *         1. GPIO: PB8/PB9/PC13 上拉输入
 *         2. EXTI: 下降沿触发
 *         3. NVIC: EXTI9_5 (优先级 1,1) / EXTI15_10 (优先级 1,2)
 *         4. TIM3: 消抖定时器 (优先级 6,0，低于 EXTI)
 */
void KEY_EXTI_Init(void)
{
    GPIO_InitTypeDef gpio_init_structure;
    EXTI_InitTypeDef exti_init_structure;
    NVIC_InitTypeDef nvic_init_structure;

    /* 使能 GPIO 和 SYSCFG 时钟 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB | RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);

    /* 清空事件队列 */
    g_key_queue_head = 0U;
    g_key_queue_tail = 0U;

    /* --- 配置 GPIO 上拉输入 --- */
    gpio_init_structure.GPIO_Mode  = GPIO_Mode_IN;
    gpio_init_structure.GPIO_PuPd  = GPIO_PuPd_UP;
    gpio_init_structure.GPIO_Speed = GPIO_Speed_100MHz;

    gpio_init_structure.GPIO_Pin = KEY1_GPIO_PIN;
    GPIO_Init(KEY1_GPIO_PORT, &gpio_init_structure);

    gpio_init_structure.GPIO_Pin = KEY2_GPIO_PIN;
    GPIO_Init(KEY2_GPIO_PORT, &gpio_init_structure);

    gpio_init_structure.GPIO_Pin = KEY3_GPIO_PIN;
    GPIO_Init(KEY3_GPIO_PORT, &gpio_init_structure);

    /* --- 配置 EXTI 线路映射 --- */
    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOB, KEY1_PIN_SOURCE);
    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOB, KEY2_PIN_SOURCE);
    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOC, KEY3_PIN_SOURCE);

    /* --- 配置 EXTI 下降沿触发 --- */
    exti_init_structure.EXTI_Line    = KEY1_EXTI_LINE;
    exti_init_structure.EXTI_Mode    = EXTI_Mode_Interrupt;
    exti_init_structure.EXTI_Trigger = EXTI_Trigger_Falling;
    exti_init_structure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti_init_structure);

    exti_init_structure.EXTI_Line = KEY2_EXTI_LINE;
    EXTI_Init(&exti_init_structure);

    exti_init_structure.EXTI_Line = KEY3_EXTI_LINE;
    EXTI_Init(&exti_init_structure);

    /* --- 配置 NVIC 中断优先级 --- */
    /* EXTI9_5: KEY1(PB8) + KEY2(PB9) */
    nvic_init_structure.NVIC_IRQChannel                   = EXTI9_5_IRQn;
    nvic_init_structure.NVIC_IRQChannelPreemptionPriority = 1;
    nvic_init_structure.NVIC_IRQChannelSubPriority        = 1;
    nvic_init_structure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init_structure);

    /* EXTI15_10: KEY3(PC13) */
    nvic_init_structure.NVIC_IRQChannel                   = EXTI15_10_IRQn;
    nvic_init_structure.NVIC_IRQChannelPreemptionPriority = 1;
    nvic_init_structure.NVIC_IRQChannelSubPriority        = 2;
    nvic_init_structure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init_structure);

    /* --- 配置 TIM3 消抖定时器 --- */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    KEY_EXTI_ReconfigureDebounceTimer();

    /* TIM3 中断优先级（低于 EXTI，确保消抖期间不阻塞中断检测） */
    nvic_init_structure.NVIC_IRQChannel                   = TIM3_IRQn;
    nvic_init_structure.NVIC_IRQChannelPreemptionPriority = 6;
    nvic_init_structure.NVIC_IRQChannelSubPriority        = 0;
    nvic_init_structure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init_structure);
}

/* =========================================================================
 *  9. 中断服务函数 —— EXTI9_5 (KEY1 + KEY2)
 * ======================================================================= */

/**
 * @brief  EXTI9_5 中断处理（KEY1=PB8, KEY2=PB9）
 * @note   检测到下降沿后：
 *         1. 清除中断标志
 *         2. 标记消抖状态，屏蔽 EXTI 线
 *         3. 启动 TIM3 消抖定时器
 */
void EXTI9_5_IRQHandler(void)
{
    uint32_t triggered = 0U;

    /* 先收集所有触发的 EXTI 线，再统一处理 */
    if (EXTI_GetITStatus(KEY1_EXTI_LINE) != RESET)
    {
        EXTI_ClearITPendingBit(KEY1_EXTI_LINE);
        triggered |= KEY1_EXTI_LINE;
    }

    if (EXTI_GetITStatus(KEY2_EXTI_LINE) != RESET)
    {
        EXTI_ClearITPendingBit(KEY2_EXTI_LINE);
        triggered |= KEY2_EXTI_LINE;
    }

    if (triggered == 0U)
    {
        return;
    }

    if (g_debouncing == 0U)
    {
        /* 首次触发：进入消抖，屏蔽所有已触发的 EXTI 线 */
        g_debouncing = 1U;
        g_debounce_start_ms = power_manager_get_tick_ms();
        g_exti_pending_mask |= triggered;
        EXTI->IMR &= ~triggered;
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
        TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
        TIM_Cmd(TIM3, ENABLE);
    }
    else
    {
        /* 消抖期间新按键：记录并屏蔽，TIM3 到期后一并处理 */
        g_exti_pending_mask |= triggered;
        EXTI->IMR &= ~triggered;
    }
}

/* =========================================================================
 *  10. 中断服务函数 —— EXTI15_10 (KEY3)
 * ======================================================================= */

/**
 * @brief  EXTI15_10 中断处理（KEY3=PC13）
 * @note   逻辑同 EXTI9_5_IRQHandler。
 */
void EXTI15_10_IRQHandler(void)
{
    /* KEY3 (PC13) 下降沿 */
    if (EXTI_GetITStatus(KEY3_EXTI_LINE) != RESET)
    {
        EXTI_ClearITPendingBit(KEY3_EXTI_LINE);
        if (g_debouncing == 0U)
        {
            g_debouncing = 1U;
            g_debounce_start_ms = power_manager_get_tick_ms();
            g_exti_pending_mask |= KEY3_EXTI_LINE;
            EXTI->IMR &= ~KEY3_EXTI_LINE;          /* 屏蔽 EXTI 线 */
            TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
            TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
            TIM_Cmd(TIM3, ENABLE);
        }
    }
}

/* =========================================================================
 *  11. 中断服务函数 —— TIM3 消抖定时器
 * ======================================================================= */

/**
 * @brief  TIM3 更新中断处理（消抖确认）
 * @note   20ms 消抖窗口到期后：
 *         1. 停止定时器
 *         2. 读取 GPIO 电平确认按键仍处于按下状态
 *         3. 将逻辑键值推入事件队列
 *         4. 恢复 EXTI 线使能
 *
 * @par 逻辑键值映射
 *      KEY1(PB8) → KEY3_PRES, KEY2(PB9) → KEY2_PRES, KEY3(PC13) → KEY1_PRES
 *      注意：物理引脚与逻辑键值存在交叉。
 */
void TIM3_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET)
    {
        uint8_t logical_key = 0U;

        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);

        /* 停止消抖定时器 */
        TIM_Cmd(TIM3, DISABLE);
        TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);

        /* --- 确认 KEY1 (PB8) → KEY3_PRES --- */
        if ((g_exti_pending_mask & KEY1_EXTI_LINE) != 0U)
        {
            if (GPIO_ReadInputDataBit(KEY1_GPIO_PORT, KEY1_GPIO_PIN) == Bit_RESET)
            {
                logical_key = KEY3_PRES;
            }
            EXTI->IMR |= KEY1_EXTI_LINE;               /* 恢复 EXTI 使能 */
            g_exti_pending_mask &= ~KEY1_EXTI_LINE;
        }

        /* --- 确认 KEY2 (PB9) → KEY2_PRES --- */
        if ((g_exti_pending_mask & KEY2_EXTI_LINE) != 0U)
        {
            if (GPIO_ReadInputDataBit(KEY2_GPIO_PORT, KEY2_GPIO_PIN) == Bit_RESET)
            {
                logical_key = KEY2_PRES;
            }
            EXTI->IMR |= KEY2_EXTI_LINE;
            g_exti_pending_mask &= ~KEY2_EXTI_LINE;
        }

        /* --- 确认 KEY3 (PC13) → KEY1_PRES --- */
        if ((g_exti_pending_mask & KEY3_EXTI_LINE) != 0U)
        {
            if (GPIO_ReadInputDataBit(KEY3_GPIO_PORT, KEY3_GPIO_PIN) == Bit_RESET)
            {
                logical_key = KEY1_PRES;
            }
            EXTI->IMR |= KEY3_EXTI_LINE;
            g_exti_pending_mask &= ~KEY3_EXTI_LINE;
        }

        /* 清除消抖状态 */
        g_debouncing = 0U;
        g_debounce_start_ms = 0U;

        /* 按键确认成功：通知电源管理活跃，推入事件队列 */
        if (logical_key != 0U)
        {
            power_manager_notify_activity();
            (void)key_queue_push_isr(logical_key);
            KEY_EXTI_OnEventQueuedFromISR();
        }
    }
}

/* =========================================================================
 *  12. 公共接口实现 —— 健康监测
 * ======================================================================= */

/**
 * @brief  检查按键 EXTI 模块是否健康
 * @note   若消抖持续超过 200ms，视为异常（可能按键卡死或定时器故障）。
 * @retval 1 — 健康；0 — 消抖超时
 */
uint8_t KEY_EXTI_IsHealthy(void)
{
    if (g_debouncing == 0U)
    {
        return 1U;
    }
    return ((power_manager_get_tick_ms() - g_debounce_start_ms) <= KEY_EXTI_HEALTH_TIMEOUT_MS) ? 1U : 0U;
}

/* =========================================================================
 *  13. 公共接口实现 —— 按键事件读取
 * ======================================================================= */

/**
 * @brief  从事件队列中获取按键值
 * @return 按键值（KEY1_PRES/KEY2_PRES/KEY3_PRES），0 表示队列为空
 */
uint8_t KEY_GetValue(void)
{
    uint8_t key_value = 0U;

    (void)key_queue_pop(&key_value);
    return key_value;
}

/**
 * @brief  手动推入按键事件（线程安全）
 * @note   用于远程按键模拟或测试。
 * @param  key_value — 按键值（非零）
 */
void KEY_PushEvent(uint8_t key_value)
{
    if (key_value == 0U)
    {
        return;
    }
    (void)key_queue_push(key_value);
}

/* =========================================================================
 *  14. 公共接口实现 —— 实时按键状态查询
 * ======================================================================= */

/**
 * @brief  查询指定逻辑按键当前是否被按下
 * @note   直接读取 GPIO 电平（低电平 = 按下）。
 *         注意物理引脚与逻辑键值的交叉映射。
 * @param  key_value — 逻辑按键值（KEY1_PRES/KEY2_PRES/KEY3_PRES）
 * @retval 1 — 按下；0 — 未按下
 */
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
