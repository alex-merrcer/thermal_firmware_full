/**
 * @file power_manager.c
 * @brief 系统电源管理、活动追踪与软件时基核心实现
 * @details 架构说明：
 *          1. 基于 TIM5 维护 10ms 软件滴答，为全系统提供统一时间基准
 *          2. 状态机决策优先级：热成像锁 > 策略+超时+有效锁 > 默认UI活跃
 *          3. 采用 PRIMASK 全局中断屏蔽实现临界区，兼容 RTOS 启动前与 ISR 上下文
 *          4. 支持 STOP 唤醒时间补偿，防止业务定时器与 RTC 时钟漂移
 * @note 设计契约：本模块非线程安全依赖型，所有公开API均内置临界区保护，可跨任务/中断安全调用
 */
#include "power_manager.h"
#include "stm32f4xx_tim.h"
#include "sys.h"

/* ======================== 配置与常量定义 ======================== */
#define POWER_MANAGER_DEFAULT_TIMEOUT_MS 15000UL /**< 默认息屏超时时间(15秒) */
#define POWER_MANAGER_TICK_MS            10UL    /**< 软件滴答周期(10ms) */

/* ======================== 静态全局状态 ======================== */
/* 注：以下变量跨主循环/中断/临界区访问，使用 volatile 防止编译器激进优化导致状态不同步 */
static volatile uint32_t s_tick_count = 0U;               /**< 软件滴答计数器(1 tick = 10ms) */
static volatile uint32_t s_last_activity_tick = 0U;       /**< 最后一次活动发生的 tick 计数 */
static volatile power_lock_mask_t s_lock_mask = 0U;       /**< 当前持有的业务锁掩码 */
static volatile power_state_t s_state = POWER_STATE_ACTIVE_UI; /**< 当前电源状态 */
static volatile power_policy_t s_policy = POWER_POLICY_BALANCED; /**< 当前电源策略 */
static volatile uint32_t s_screen_off_timeout_ticks = (POWER_MANAGER_DEFAULT_TIMEOUT_MS / POWER_MANAGER_TICK_MS); /**< 息屏超时阈值(ticks) */

/* ======================== 临界区辅助函数 ======================== */

/**
 * @brief 保存中断状态并进入临界区
 * @return 进入前的 PRIMASK 寄存器值
 * @note 采用 PRIMASK 而非 FreeRTOS taskENTER_CRITICAL 的原因：
 *       1. 本模块需在 RTOS 调度器启动前完成初始化（early-boot阶段）
 *       2. 需兼容中断上下文调用，避免 RTOS 临界区断言失败
 *       3. 临界区极短（仅位运算/赋值），全局关中断对实时性影响可忽略
 */
static uint32_t power_manager_irq_save(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

/**
 * @brief 恢复中断状态并退出临界区
 * @param primask 由 power_manager_irq_save 返回的原始 PRIMASK 状态
 * @note 必须与 power_manager_irq_save 严格配对调用，否则会导致中断永久关闭或状态错乱
 */
static void power_manager_irq_restore(uint32_t primask)
{
    if (primask == 0U)
    {
        __enable_irq();
    }
}

/* ======================== 状态机与定时器配置 ======================== */

/**
 * @brief 计算下一目标电源状态（必须在临界区内调用）
 * @return power_state_t 决策后的目标状态枚举值
 * @details 决策优先级规则：
 *          1. 若持有 THERMAL 锁，强制锁定 ACTIVE_THERMAL（采集期间禁止降频/休眠）
 *          2. 若策略非 PERFORMANCE，且无有效阻塞锁，且超时到达，则进入 SCREEN_OFF_IDLE
 *          3. 默认回退至 ACTIVE_UI
 * @note 刻意屏蔽 ESP_HOST 锁：Wi-Fi/蓝牙后台保活属于低功耗常驻业务，
 *       不应阻止系统息屏节能。此过滤策略符合物联网设备典型功耗模型。
 */
static power_state_t power_manager_compute_state_locked(void)
{
    power_state_t next_state = POWER_STATE_ACTIVE_UI;

    /* 过滤掉允许息屏的后台保活锁 */
    power_lock_mask_t effective_lock_mask =
        (power_lock_mask_t)(s_lock_mask & (power_lock_mask_t)(~POWER_LOCK_ESP_HOST));

    if ((s_lock_mask & POWER_LOCK_THERMAL) != 0U)
    {
        next_state = POWER_STATE_ACTIVE_THERMAL;
    }
    else if ((s_policy != POWER_POLICY_PERFORMANCE) &&
             (effective_lock_mask == 0U) &&
             ((s_tick_count - s_last_activity_tick) >= s_screen_off_timeout_ticks))
    {
        next_state = POWER_STATE_SCREEN_OFF_IDLE;
    }

    return next_state;
}

/**
 * @brief 获取 APB1 定时器实际时钟频率
 * @return APB1定时器时钟频率(Hz)
 * @details STM32F4 硬件特性（参考 RM0090 Section 6.3.13）：
 *          当 APB1 分频系数为 1 时，定时器时钟 = PCLK1；
 *          当分频系数为 2/4/8/16 时，定时器时钟自动 ×2。
 *          此函数严格遵循该规则，确保 TIM5 分频计算在任何主频下均准确。
 */
static uint32_t power_manager_get_apb1_timer_clock_hz(void)
{
    uint32_t ppre1_bits = RCC->CFGR & RCC_CFGR_PPRE1;
    uint32_t hclk_hz = SystemCoreClock;
    uint32_t pclk1_hz = hclk_hz;

    switch (ppre1_bits)
    {
        case RCC_CFGR_PPRE1_DIV2:  pclk1_hz = hclk_hz / 2U;  break;
        case RCC_CFGR_PPRE1_DIV4:  pclk1_hz = hclk_hz / 4U;  break;
        case RCC_CFGR_PPRE1_DIV8:  pclk1_hz = hclk_hz / 8U;  break;
        case RCC_CFGR_PPRE1_DIV16: pclk1_hz = hclk_hz / 16U; break;
        default:                   pclk1_hz = hclk_hz;       break;
    }

    /* 应用 STM32 APB 定时器时钟倍频规则 */
    return (ppre1_bits == RCC_CFGR_PPRE1_DIV1) ? pclk1_hz : (pclk1_hz * 2U);
}

/**
 * @brief 初始化 TIM5 作为 10ms 软件时基定时器
 * @details 数学推导：目标频率 100Hz (10ms)
 *          Prescaler = APB1_Timer_CLK / 10000
 *          Period    = 100 - 1
 *          实际溢出频率 = APB1_Timer_CLK / (Prescaler+1) / (Period+1) = 10000 / 100 = 100Hz
 * @note NVIC 抢占优先级设为 2：低于通信/故障中断(0~1)，高于后台业务中断(3~)，
 *       确保时基稳定且不阻塞关键实时任务。
 */
static void power_manager_tim5_init(void)
{
    TIM_TimeBaseInitTypeDef tim_time_base;
    NVIC_InitTypeDef nvic_init;
    uint32_t timer_clock_hz = power_manager_get_apb1_timer_clock_hz();
    uint32_t prescaler = (timer_clock_hz / 10000UL);

    if (prescaler == 0U)
    {
        prescaler = 1U; /* 防御性保护：防止极低主频下分频器为0导致硬件异常 */
    }

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM5, ENABLE);
    TIM_Cmd(TIM5, DISABLE);
    TIM_DeInit(TIM5);

    tim_time_base.TIM_Period        = 100U - 1U;
    tim_time_base.TIM_Prescaler     = (uint16_t)(prescaler - 1U);
    tim_time_base.TIM_ClockDivision = TIM_CKD_DIV1;
    tim_time_base.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM5, &tim_time_base);

    TIM_ITConfig(TIM5, TIM_IT_Update, ENABLE);

    nvic_init.NVIC_IRQChannel                   = TIM5_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 2;
    nvic_init.NVIC_IRQChannelSubPriority        = 0;
    nvic_init.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init);

    TIM_SetCounter(TIM5, 0U);
    TIM_Cmd(TIM5, ENABLE);
}

/* ======================== 公开API实现 ======================== */

/**
 * @brief 电源管理器初始化
 * @note 重置所有状态机变量、超时阈值与锁掩码，并启动 TIM5 时基。
 *       必须在系统早期初始化阶段（RTOS启动前或首个任务中）调用。
 */
void power_manager_init(void)
{
    uint32_t primask = power_manager_irq_save();
    s_tick_count = 0U;
    s_last_activity_tick = 0U;
    s_lock_mask = 0U;
    s_state = POWER_STATE_ACTIVE_UI;
    s_policy = POWER_POLICY_BALANCED;
    s_screen_off_timeout_ticks = (POWER_MANAGER_DEFAULT_TIMEOUT_MS / POWER_MANAGER_TICK_MS);
    power_manager_irq_restore(primask);

    power_manager_tim5_init();
}

/**
 * @brief 通知系统发生用户/业务活动
 * @note 重置息屏超时计时器。应在按键按下、屏幕触摸、重要数据到达等场景调用。
 *       内置临界区保护，可安全跨上下文调用。
 */
void power_manager_notify_activity(void)
{
    uint32_t primask = power_manager_irq_save();
    s_last_activity_tick = s_tick_count;
    power_manager_irq_restore(primask);
}

/**
 * @brief 申请电源业务锁
 * @param mask 锁掩码（支持按位或组合，如 POWER_LOCK_THERMAL | POWER_LOCK_OTA）
 * @note 持有锁期间，状态机将阻止系统进入息屏空闲或低功耗模式。
 *       必须与 power_manager_release_lock 严格配对，否则会导致系统无法休眠。
 */
void power_manager_acquire_lock(power_lock_mask_t mask)
{
    uint32_t primask = power_manager_irq_save();
    s_lock_mask |= mask;
    power_manager_irq_restore(primask);
}

/**
 * @brief 释放电源业务锁
 * @param mask 锁掩码
 * @note 仅清除指定掩码位，不影响其他并发持有的锁。
 */
void power_manager_release_lock(power_lock_mask_t mask)
{
    uint32_t primask = power_manager_irq_save();
    s_lock_mask &= (power_lock_mask_t)(~mask);
    power_manager_irq_restore(primask);
}

/**
 * @brief 获取当前持有的电源锁掩码
 * @return power_lock_mask_t 当前锁状态位图
 * @note 返回值为临界区快照，适用于状态查询与调试日志。
 */
power_lock_mask_t power_manager_get_lock_mask(void)
{
    power_lock_mask_t lock_mask = 0U;
    uint32_t primask = power_manager_irq_save();
    lock_mask = s_lock_mask;
    power_manager_irq_restore(primask);
    return lock_mask;
}

/**
 * @brief 设置电源策略
 * @param policy 目标策略枚举值
 * @note 策略变更后立即重新计算电源状态。越界入参将自动回退至 BALANCED。
 */
void power_manager_set_policy(power_policy_t policy)
{
    uint32_t primask = power_manager_irq_save();
    if ((uint32_t)policy >= (uint32_t)POWER_POLICY_COUNT)
    {
        policy = POWER_POLICY_BALANCED; /* 越界保护：回退至均衡策略 */
    }
    s_policy = policy;
    s_state = power_manager_compute_state_locked();
    power_manager_irq_restore(primask);
}

/**
 * @brief 获取当前电源策略
 * @return power_policy_t 当前策略枚举值
 */
power_policy_t power_manager_get_policy(void)
{
    power_policy_t policy = POWER_POLICY_BALANCED;
    uint32_t primask = power_manager_irq_save();
    policy = s_policy;
    power_manager_irq_restore(primask);
    return policy;
}

/**
 * @brief 电源状态机步进函数
 * @note 需在主循环或调度任务中周期性调用（建议 >= 10Hz）。
 *       根据当前锁、策略与超时时间动态更新 s_state。
 */
void power_manager_step(void)
{
    uint32_t primask = power_manager_irq_save();
    s_state = power_manager_compute_state_locked();
    power_manager_irq_restore(primask);
}

/**
 * @brief 获取当前电源状态
 * @return power_state_t 当前状态枚举值
 */
power_state_t power_manager_get_state(void)
{
    power_state_t state = POWER_STATE_ACTIVE_UI;
    uint32_t primask = power_manager_irq_save();
    state = s_state;
    power_manager_irq_restore(primask);
    return state;
}

/**
 * @brief 获取系统软件滴答时间
 * @return 自初始化以来的流逝时间(ms)
 * @note 精度为 10ms。适用于业务超时判断、延时计算与活动追踪。
 */
uint32_t power_manager_get_tick_ms(void)
{
    uint32_t tick_count = 0U;
    uint32_t primask = power_manager_irq_save();
    tick_count = s_tick_count;
    power_manager_irq_restore(primask);
    return tick_count * POWER_MANAGER_TICK_MS;
}

/**
 * @brief 补偿 STOP/STANDBY 休眠期间流逝的时间
 * @param elapsed_ms 实际休眠时长(毫秒)
 * @details 采用四舍五入算法将毫秒转换为 tick 数：(elapsed + TICK/2) / TICK
 *          防止长期累积误差。补偿后业务定时器与超时逻辑可无缝衔接，无需重新初始化。
 * @note 由低功耗运行时模块在 STOP/STANDBY 唤醒后调用。
 */
void power_manager_advance_sleep_time(uint32_t elapsed_ms)
{
    uint32_t extra_ticks = (elapsed_ms + (POWER_MANAGER_TICK_MS / 2UL)) / POWER_MANAGER_TICK_MS;
    uint32_t primask = power_manager_irq_save();
    s_tick_count += extra_ticks;
    power_manager_irq_restore(primask);
}

/**
 * @brief 低功耗开关兼容接口（内部映射为策略切换）
 * @param enabled 1:启用节能 0:强制高性能
 * @note 设计契约：启用时若当前为高性能则降级至均衡；禁用时强制锁定高性能。
 *       用于兼容旧版 UI 设置或外部指令，新业务建议直接使用 set_policy。
 */
void power_manager_set_low_power_enabled(uint8_t enabled)
{
    power_policy_t current_policy = power_manager_get_policy();
    if (enabled != 0U)
    {
        if (current_policy == POWER_POLICY_PERFORMANCE)
        {
            power_manager_set_policy(POWER_POLICY_BALANCED);
            return;
        }
    }
    else
    {
        power_manager_set_policy(POWER_POLICY_PERFORMANCE);
        return;
    }
}

/**
 * @brief 获取低功耗模式开关状态
 * @return 1:已启用(非高性能) 0:已禁用(高性能锁定)
 */
uint8_t power_manager_is_low_power_enabled(void)
{
    return (power_manager_get_policy() != POWER_POLICY_PERFORMANCE) ? 1U : 0U;
}

/**
 * @brief 设置屏幕无操作超时时间
 * @param timeout_ms 超时阈值(毫秒)
 * @note 低于 10ms 的非法值将自动重置为默认值(15s)。设置后立即重新评估电源状态。
 */
void power_manager_set_screen_off_timeout_ms(uint32_t timeout_ms)
{
    uint32_t primask = power_manager_irq_save();
    if (timeout_ms < POWER_MANAGER_TICK_MS)
    {
        timeout_ms = POWER_MANAGER_DEFAULT_TIMEOUT_MS; /* 非法值保护 */
    }
    s_screen_off_timeout_ticks = timeout_ms / POWER_MANAGER_TICK_MS;
    if (s_screen_off_timeout_ticks == 0U)
    {
        s_screen_off_timeout_ticks = POWER_MANAGER_DEFAULT_TIMEOUT_MS / POWER_MANAGER_TICK_MS;
    }
    s_state = power_manager_compute_state_locked();
    power_manager_irq_restore(primask);
}

/**
 * @brief 获取屏幕无操作超时时间
 * @return 超时时间(毫秒)
 */
uint32_t power_manager_get_screen_off_timeout_ms(void)
{
    uint32_t timeout_ticks = 0U;
    uint32_t primask = power_manager_irq_save();
    timeout_ticks = s_screen_off_timeout_ticks;
    power_manager_irq_restore(primask);
    return timeout_ticks * POWER_MANAGER_TICK_MS;
}

/**
 * @brief 重新配置时基定时器
 * @note 系统主频变更（如 STOP 唤醒或动态调频）后必须调用，
 *       内部会重新计算 TIM5 分频器以维持 10ms 精度。
 */
void power_manager_reconfigure_timebase(void)
{
    power_manager_tim5_init();
}

/* ======================== 中断服务函数 ======================== */

/**
 * @brief TIM5 更新中断服务函数（10ms 周期触发）
 * @note 仅递增滴答计数器，不执行复杂逻辑以保证中断实时性。
 *       s_tick_count 为 volatile uint32_t，Cortex-M 单指令递增天然原子，无需额外临界区。
 */
void TIM5_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM5, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM5, TIM_IT_Update);
        s_tick_count++;
    }
}
