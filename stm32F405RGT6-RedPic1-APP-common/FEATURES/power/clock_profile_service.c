/**
 * @file clock_profile_service.c
 * @brief 系统时钟配置、动态调频与STOP唤醒恢复核心实现
 * @details 架构说明：
 *          1. 提供HIGH(168MHz)与MEDIUM(84MHz)两档运行时频率，通过HCLK分频实现动态调频
 *          2. STOP模式唤醒后，严格遵循STM32F4时钟树恢复序列：HSI -> HSE -> PLL -> SYSCLK
 *          3. 频率切换后自动同步所有依赖SystemCoreClock的时基模块，防止定时器/通信波特率漂移
 *          4. 设计契约：本模块非线程安全，应由电源/时钟管理任务单上下文调用
 */
#include "clock_profile_service.h"
#include "delay.h"
#include "ota_service.h"
#include "power_manager.h"
#include "system_stm32f4xx.h"
#include "usart.h"
#include "exti_key.h"

/* 当前激活的时钟档位（静态全局状态） */
static clock_profile_t s_active_profile = CLOCK_PROFILE_HIGH;

/* ======================== 内部辅助函数 ======================== */

/**
 * @brief 同步更新所有依赖系统时钟的模块时基
 * @param reinit_uart 1: 重新初始化UART波特率分频器 0: 保持UART当前配置/休眠状态
 * @note 必须在SystemCoreClock更新后调用，否则延时与定时器将产生比例漂移
 */
static void clock_profile_reconfigure_dependents(uint8_t reinit_uart)
{
    /* 1. 更新CMSIS核心时钟变量（所有HAL/SPL时基计算的基础） */
    SystemCoreClockUpdate();

    /* 2. 重初始化软件延时与业务定时器时基 */
    delay_init((u8)(SystemCoreClock / 1000000UL));
    KEY_EXTI_ReconfigureDebounceTimer();
    ota_service_reconfigure_timebase();
    power_manager_reconfigure_timebase();

    /* 3. 按需恢复UART（STOP唤醒且对端在线时需重新计算波特率分频器） */
    if (reinit_uart != 0U)
    {
        uart_reinit_current_baud();
    }
}

/**
 * @brief STOP唤醒后时钟树恢复标准序列（内部公共实现）
 * @details 恢复流程：HSI(安全时钟) -> HSE(外部晶振) -> PLL(倍频) -> SYSCLK切换
 *          若HSE起振失败，系统自动降级至HSI运行，保证设备不死锁（Fallback机制）
 * @param reinit_uart 是否同步恢复UART外设
 */
static void clock_tree_restore_sequence(uint8_t reinit_uart)
{
    uint32_t startup_counter = 0U;

    /* 防御性恢复：PH0/PH1 承载 HSE，先确保 GPIOH 时钟已使能。 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOH, ENABLE);

    /* 阶段1：使能内部高速时钟(HSI)作为安全过渡时钟 */
    RCC_HSICmd(ENABLE);
    while (RCC_GetFlagStatus(RCC_FLAG_HSIRDY) == RESET)
    {
        /* 阻塞等待HSI稳定（通常<10us，硬件自校准） */
    }

    /* 阶段2：尝试恢复外部高速时钟(HSE) */
    RCC_HSEConfig(RCC_HSE_ON);
    startup_counter = 0U;
    /* HSE起振超时保护：0x4000次循环约16ms，防止晶振损坏/虚焊导致系统死锁 */
    while (RCC_GetFlagStatus(RCC_FLAG_HSERDY) == RESET && startup_counter < 0x4000U)
    {
        startup_counter++;
    }

    /* 阶段3：若HSE成功起振，则配置PLL并切换至主时钟 */
    if (RCC_GetFlagStatus(RCC_FLAG_HSERDY) != RESET)
    {
        RCC_PLLCmd(ENABLE);
        while (RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET)
        {
            /* 等待PLL锁定（通常<200us） */
        }

        /* 配置Flash等待周期：168MHz @ 3.3V 需 5WS (参考RM0090 Table 10)
         * 必须在切换SYSCLK前配置，否则取指延迟不足将触发HardFault */
        FLASH_SetLatency(FLASH_Latency_5);

        /* 切换系统时钟源至PLL */
        RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);
        /* 等待切换完成：0x08U 表示 RCC_CFGR 的 SWS[1:0] 位指示 PLL 作为系统时钟 */
        while (RCC_GetSYSCLKSource() != 0x08U)
        {
        }
    }
    /* 注：若HSE失败，系统保持HSI运行(16MHz)，后续分频配置仍安全生效，设备以降频模式存活 */

    /* 阶段4：根据当前档位恢复AHB总线(HCLK)分频比 */
    if (s_active_profile == CLOCK_PROFILE_MEDIUM)
    {
        RCC_HCLKConfig(RCC_SYSCLK_Div2); /* SYSCLK/2 -> 典型84MHz */
    }
    else
    {
        RCC_HCLKConfig(RCC_SYSCLK_Div1); /* SYSCLK/1 -> 典型168MHz */
    }

    /* 阶段5：同步下游依赖模块 */
    clock_profile_reconfigure_dependents(reinit_uart);
}

/* ======================== 公开API实现 ======================== */

/**
 * @brief 时钟调频服务初始化
 */
void clock_profile_service_init(void)
{
    s_active_profile = CLOCK_PROFILE_HIGH;
    clock_profile_reconfigure_dependents(1U);
}

/**
 * @brief 设置系统时钟性能档位
 * @param profile 目标档位枚举值
 */
void clock_profile_set(clock_profile_t profile)
{
    if (profile == s_active_profile)
    {
        return; /* 档位未变化，直接返回避免不必要的时钟抖动与总线毛刺 */
    }

    /* 动态调整HCLK分频实现调频（PLL输出保持168MHz不变，仅改变AHB分频） */
    if (profile == CLOCK_PROFILE_MEDIUM)
    {
        RCC_HCLKConfig(RCC_SYSCLK_Div2);
    }
    else
    {
        RCC_HCLKConfig(RCC_SYSCLK_Div1);
        profile = CLOCK_PROFILE_HIGH; /* 强制收敛至合法枚举，防御非法入参 */
    }

    s_active_profile = profile;
    clock_profile_reconfigure_dependents(1U);
}

/**
 * @brief 获取当前激活的时钟档位
 * @return clock_profile_t 当前档位枚举值
 */
clock_profile_t clock_profile_get(void)
{
    return s_active_profile;
}

/**
 * @brief STOP模式唤醒后恢复时钟树（含UART波特率重初始化）
 */
void clock_profile_restore_after_stop(void)
{
    clock_tree_restore_sequence(1U);
}

/**
 * @brief STOP模式唤醒后恢复时钟树（保持UART休眠/当前配置）
 */
void clock_profile_restore_after_stop_keep_uart_sleep(void)
{
    clock_tree_restore_sequence(0U);
}
