/**
 * @file rtc_lp_service.c
 * @brief 低功耗RTC唤醒与备份域管理核心实现
 * @details 架构说明：
 *          1. 基于LSI(~32kHz)时钟源，通过动态切换Div16(2kHz)与CK_SPRE(1Hz)实现高精度与长周期覆盖
 *          2. 严格遵循STM32F4 RTC唤醒硬件序列：关闭定时器 -> 等待WUTWF同步 -> 配置计数器 -> 使能
 *          3. EXTI Line 22需同时配置IMR(中断)与EMR(事件)，否则STOP模式下无法唤醒内核
 *          4. 备份域访问必须显式使能PWR时钟与Backup Access权限，防止总线HardFault
 */
#include "rtc_lp_service.h"
#include "stm32f4xx_conf.h"

/* ======================== 配置与常量定义 ======================== */
#define RTC_LP_BACKUP_REG_COUNT         5U      /**< 可用备份寄存器数量(DR0~DR4) */
#define RTC_LP_LSI_FREQ_HZ              32000UL /**< LSI典型频率(实际约30~33kHz，用于近似计算) */
#define RTC_LP_MIN_PERIOD_MS            500UL   /**< 最小唤醒周期限制(防止计数器下溢) */
#define RTC_LP_DIV16_THRESHOLD_MS       30000UL /**< Div16时钟源最大覆盖范围(~32.7s) */
#define RTC_LP_WUTWF_TIMEOUT_COUNT      0xFFFFU /**< WUTWF同步标志等待超时保护(约100ms) */

/* 备份寄存器物理地址映射表 */
static const uint32_t s_backup_regs[RTC_LP_BACKUP_REG_COUNT] =
{
    RTC_BKP_DR0, RTC_BKP_DR1, RTC_BKP_DR2, RTC_BKP_DR3, RTC_BKP_DR4
};

/* ======================== 静态全局状态 ======================== */
static volatile uint8_t s_wakeup_pending = 0U;          /**< 唤醒事件待处理标志(volatile防编译器优化) */
static uint32_t s_programmed_sleep_ms = 1000UL;         /**< 上次编程的目标休眠周期 */
static uint32_t s_last_elapsed_ms = 0U;                 /**< 上次休眠流逝时间(用于时基补偿) */
static rtc_lp_wakeup_reason_t s_wakeup_reason = RTC_LP_WAKE_NONE; /**< 当前唤醒原因 */
static uint8_t s_woke_from_standby = 0U;                /**< STANDBY冷启动标记 */

/* ======================== 内部辅助函数 ======================== */

/**
 * @brief 等待RTC唤醒定时器写入完成标志(WUTWF)
 * @note 硬件约束：修改RTC_WUTR前必须等待WUTWF置1，否则写入被忽略。
 *       企业级规范：增加超时计数器防止时钟异常或总线挂起导致永久死锁。
 */
static void rtc_lp_wait_wutwf(void)
{
    uint32_t timeout = RTC_LP_WUTWF_TIMEOUT_COUNT;
    while (RTC_GetFlagStatus(RTC_FLAG_WUTWF) == RESET)
    {
        if (--timeout == 0U)
        {
            break; /* 超时保护：放弃等待，防止系统死锁 */
        }
    }
}

/* ======================== 公开API实现 ======================== */

/**
 * @brief RTC低功耗服务初始化
 */
void rtc_lp_service_init(void)
{
    EXTI_InitTypeDef exti_init;
    NVIC_InitTypeDef nvic_init;
    RTC_InitTypeDef rtc_init;
    uint32_t rtcsel = 0U;

    /* 阶段1：使能PWR时钟与备份域访问权限 */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
    PWR_BackupAccessCmd(ENABLE);

    /* 阶段2：检测唤醒源（STANDBY标志由硬件保持，需软件清除） */
    s_woke_from_standby = (PWR_GetFlagStatus(PWR_FLAG_SB) != RESET) ? 1U : 0U;
    s_wakeup_reason = (s_woke_from_standby != 0U) ? RTC_LP_WAKE_STANDBY_RESET : RTC_LP_WAKE_NONE;

    /* 阶段3：起振LSI并配置RTC时钟源 */
    RCC_LSICmd(ENABLE);
    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET) { /* 阻塞等待LSI稳定 */ }

    rtcsel = RCC->BDCR & RCC_BDCR_RTCSEL;
    /* 若RTC未使能或时钟源非LSI，则执行备份域复位并重新路由 */
    if (((RCC->BDCR & RCC_BDCR_RTCEN) == 0U) || (rtcsel != RCC_RTCCLKSource_LSI))
    {
        RCC_BackupResetCmd(ENABLE);
        RCC_BackupResetCmd(DISABLE);
        RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);
        RCC_RTCCLKCmd(ENABLE);
    }
    RTC_WaitForSynchro();

    /* 阶段4：配置RTC分频器
     * 异步分频(127) -> 32kHz/(127+1) = 250Hz
     * 同步分频(249) -> 250Hz/(249+1) = 1Hz (CK_SPRE基准)
     * 此配置同时支持Div16(2kHz)与1Hz长周期唤醒 */
    rtc_init.RTC_AsynchPrediv = 127U;
    rtc_init.RTC_SynchPrediv  = 249U;
    rtc_init.RTC_HourFormat   = RTC_HourFormat_24;
    RTC_Init(&rtc_init);

    rtc_lp_disarm(); /* 初始状态解除武装 */

    /* 阶段5：配置EXTI Line 22中断路由
     * 注意：STM32F4的RTC唤醒必须同时使能IMR(中断掩码)与EMR(事件掩码)，
     * 否则STOP模式下内核无法被唤醒。SPL的EXTI_Init仅配置IMR，故需直接操作寄存器。 */
    EXTI_ClearITPendingBit(EXTI_Line22);
    exti_init.EXTI_Line    = EXTI_Line22;
    exti_init.EXTI_Mode    = EXTI_Mode_Interrupt;
    exti_init.EXTI_Trigger = EXTI_Trigger_Rising;
    exti_init.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti_init);
    EXTI->EMR |= EXTI_Line22; /* 显式使能事件掩码(硬件唤醒必需) */
    EXTI->IMR |= EXTI_Line22; /* 显式使能中断掩码 */

    /* 阶段6：配置NVIC抢占优先级 */
    nvic_init.NVIC_IRQChannel                   = RTC_WKUP_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0;
    nvic_init.NVIC_IRQChannelSubPriority        = 1;
    nvic_init.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init);

    /* 阶段7：清除PWR唤醒/待机标志，准备进入低功耗 */
    PWR_WakeUpPinCmd(DISABLE);
    PWR_ClearFlag(PWR_FLAG_WU);
    PWR_ClearFlag(PWR_FLAG_SB);
}

/**
 * @brief 武装RTC唤醒定时器
 * @details 时钟源选择策略：
 *          - period <= 30s: 使用 RTCCLK_Div16 (2kHz tick, 0.5ms分辨率, 最大~32.7s)
 *          - period > 30s:  使用 CK_SPRE_16bits (1Hz tick, 1s分辨率, 最大~18.2小时)
 */
void rtc_lp_arm_ms(uint32_t period_ms)
{
    uint32_t wake_clock = RTC_WakeUpClock_RTCCLK_Div16;
    uint32_t ticks = 0U;

    /* 边界保护：防止计数器下溢 */
    if (period_ms < RTC_LP_MIN_PERIOD_MS)
    {
        period_ms = RTC_LP_MIN_PERIOD_MS;
    }

    rtc_lp_disarm(); /* 安全解除旧配置，等待硬件同步 */

    /* 动态选择时钟源并计算计数器重载值 */
    if (period_ms <= RTC_LP_DIV16_THRESHOLD_MS)
    {
        /* Div16模式：LSI 32kHz / 16 = 2000Hz (0.5ms/tick)
         * 公式：ticks = period_ms * 2，+999实现向上取整 */
        ticks = (period_ms * 2000UL + 999UL) / 1000UL;
        wake_clock = RTC_WakeUpClock_RTCCLK_Div16;
    }
    else
    {
        /* CK_SPRE模式：1Hz (1s/tick)
         * 公式：ticks = period_ms / 1000，+999实现向上取整 */
        ticks = (period_ms + 999UL) / 1000UL;
        wake_clock = RTC_WakeUpClock_CK_SPRE_16bits;
    }

    /* 计数器边界钳位(16位寄存器上限0xFFFF，实际写入值需-1) */
    if (ticks == 0U) ticks = 1U;
    if (ticks > 0x10000UL) ticks = 0x10000UL;

    /* 更新全局状态（用于休眠后时基补偿） */
    s_programmed_sleep_ms = period_ms;
    s_last_elapsed_ms = period_ms; /* 设计契约：采用目标周期近似补偿，避免唤醒后读取RTC亚秒寄存器的复杂同步 */
    s_wakeup_pending = 0U;

    /* 硬件配置序列 */
    RTC_WakeUpClockConfig(wake_clock);
    RTC_SetWakeUpCounter(ticks - 1UL);
    RTC_ITConfig(RTC_IT_WUT, ENABLE);
    RTC_WakeUpCmd(ENABLE);
}

/**
 * @brief 解除RTC唤醒定时器
 */
void rtc_lp_disarm(void)
{
    RTC_ITConfig(RTC_IT_WUT, DISABLE);
    RTC_WakeUpCmd(DISABLE);
    rtc_lp_wait_wutwf(); /* 必须等待同步完成，否则后续配置无效 */
    RTC_ClearITPendingBit(RTC_IT_WUT);
    RTC_ClearFlag(RTC_FLAG_WUTF);
    EXTI_ClearITPendingBit(EXTI_Line22);
}

/**
 * @brief RTC唤醒中断服务函数
 */
void rtc_lp_handle_irq(void)
{
    if (RTC_GetITStatus(RTC_IT_WUT) != RESET)
    {
        /* 严格遵循清除顺序：RTC标志 -> EXTI挂起位 */
        RTC_ClearITPendingBit(RTC_IT_WUT);
        RTC_ClearFlag(RTC_FLAG_WUTF);
        EXTI_ClearITPendingBit(EXTI_Line22);

        s_wakeup_pending = 1U;
        s_wakeup_reason = RTC_LP_WAKE_TIMER;
    }
}

/**
 * @brief 消费并清除唤醒事件标志
 */
uint8_t rtc_lp_consume_wakeup_event(void)
{
    uint8_t pending = s_wakeup_pending;
    s_wakeup_pending = 0U;
    return pending;
}

/**
 * @brief 获取上次休眠流逝时间
 */
uint32_t rtc_lp_get_last_elapsed_ms(void)
{
    return s_last_elapsed_ms;
}

/**
 * @brief 获取上次编程的目标休眠时间
 */
uint32_t rtc_lp_get_last_programmed_ms(void)
{
    return s_programmed_sleep_ms;
}

/**
 * @brief 获取系统唤醒原因
 */
rtc_lp_wakeup_reason_t rtc_lp_get_wakeup_reason(void)
{
    return s_wakeup_reason;
}

/**
 * @brief 判断系统是否从STANDBY冷启动唤醒
 */
uint8_t rtc_lp_woke_from_standby(void)
{
    return s_woke_from_standby;
}

/**
 * @brief 写入RTC备份寄存器
 */
void rtc_lp_backup_write(uint32_t index, uint32_t value)
{
    if (index >= RTC_LP_BACKUP_REG_COUNT) return;

    /* 备份域访问必须显式使能PWR时钟与写权限，防止总线Fault */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    RTC_WriteBackupRegister(s_backup_regs[index], value);
}

/**
 * @brief 读取RTC备份寄存器
 */
uint32_t rtc_lp_backup_read(uint32_t index)
{
    if (index >= RTC_LP_BACKUP_REG_COUNT) return 0U;
    return RTC_ReadBackupRegister(s_backup_regs[index]);
}
