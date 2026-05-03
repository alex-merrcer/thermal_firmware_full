/**
 * @file    iap_platform.c
 * @brief   IAP 平台抽象层 —— 硬件相关操作封装
 * @note    本模块封装 IAP 引导加载程序所需的硬件平台操作，
 *          包括独立看门狗（IWDG）管理、应用有效性校验、
 *          复位原因检测、串口输出、中断清理和应用跳转。
 *
 * @par 应用有效性校验
 *      通过检查应用起始地址处的栈指针值判断：
 *      栈指针必须指向 SRAM 范围（0x20000000 ~ 0x2001FFFF），
 *      掩码 0x2FFE0000 用于兼容不同 SRAM 大小的 STM32 型号。
 *
 * @par 复位原因检测
 *      通过 RCC 复位标志寄存器判断复位来源：
 *      - IWDG/WWDG 复位 → BOOT_REASON_IWDG
 *      - BOR/POR/PIN 复位 → BOOT_REASON_POWER
 *      - 软件复位 → BOOT_REASON_SOFTWARE
 *      - 其他 → BOOT_REASON_NORMAL
 *
 * @par 应用跳转流程
 *      1. 校验应用有效性
 *      2. 关闭全局中断，停止 SysTick
 *      3. 清除所有 NVIC 中断和挂起位
 *      4. 设置向量表偏移（VTOR）
 *      5. 设置主栈指针（MSP）
 *      6. 跳转到应用复位向量
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "iap_platform.h"
#include "stm32f4xx_iwdg.h"
#include "stm32f4xx_rcc.h"

/* =========================================================================
 *  2. 公共接口实现 —— 看门狗管理
 * ======================================================================= */

/**
 * @brief  初始化独立看门狗（IWDG）
 * @note   配置预分频器和重载值（由 IWDG_PRESCALER 和 IWDG_RELOAD 宏定义），
 *         立即喂狗一次并使能 IWDG。
 */
void iap_init_watchdog(void)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_PRESCALER);
    IWDG_SetReload(IWDG_RELOAD);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

/**
 * @brief  喂狗（重载 IWDG 计数器）
 */
void iap_feed_watchdog(void)
{
    IWDG_ReloadCounter();
}

/* =========================================================================
 *  3. 公共接口实现 —— 应用有效性校验
 * ======================================================================= */

/**
 * @brief  检查指定地址的应用是否有效
 * @note   通过读取应用起始地址处的初始栈指针值，
 *         判断其是否指向 SRAM 范围（0x20000000 ~ 0x2001FFFF）。
 *         掩码 0x2FFE0000 兼容 STM32F4 系列不同 SRAM 大小。
 * @param  app_addr — 应用起始地址（Flash 中）
 * @retval 1 — 有效；0 — 无效
 */
uint8_t is_app_valid(uint32_t app_addr)
{
    uint32_t stack_addr = *(volatile uint32_t *)app_addr;
    return ((stack_addr & 0x2FFE0000U) == 0x20000000U) ? 1U : 0U;
}

/* =========================================================================
 *  4. 公共接口实现 —— 复位原因检测
 * ======================================================================= */

/**
 * @brief  获取系统复位原因
 * @note   通过 RCC 复位标志寄存器判断复位来源。
 *         优先级：IWDG/WWDG > BOR/POR/PIN > 软件复位。
 * @return 复位原因码（BOOT_REASON_xxx）
 */
uint8_t get_reset_reason(void)
{
    /* 看门狗复位 */
    if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET ||
        RCC_GetFlagStatus(RCC_FLAG_WWDGRST) != RESET)
    {
        return BOOT_REASON_IWDG;
    }

    /* 上电/掉电/外部复位 */
    if (RCC_GetFlagStatus(RCC_FLAG_BORRST) != RESET ||
        RCC_GetFlagStatus(RCC_FLAG_PORRST) != RESET ||
        RCC_GetFlagStatus(RCC_FLAG_PINRST) != RESET)
    {
        return BOOT_REASON_POWER;
    }

    /* 软件复位 */
    if (RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET)
    {
        return BOOT_REASON_SOFTWARE;
    }

    return BOOT_REASON_NORMAL;
}

/**
 * @brief  清除所有 RCC 复位标志
 */
void clear_reset_flags(void)
{
    RCC_ClearFlag();
}

/* =========================================================================
 *  5. 公共接口实现 —— 串口输出
 * ======================================================================= */

/**
 * @brief  发送单个字节到串口
 * @param  c — 字符
 * @return 始终返回 0
 */
uint32_t Send_Byte(uint8_t c)
{
    SerialPutChar(c);
    return 0U;
}

/* =========================================================================
 *  6. 公共接口实现 —— 应用跳转
 * ======================================================================= */

/**
 * @brief  跳转前的中断和外设清理
 * @note   关闭全局中断、停止 SysTick、清除所有 NVIC 中断使能和挂起位。
 *         确保跳转到应用前系统处于干净的中断状态。
 */
void iap_cleanup_before_jump(void)
{
    uint32_t index = 0U;

    __disable_irq();

    /* 停止 SysTick 定时器 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /* 清除所有 NVIC 中断使能和挂起位 */
    for (index = 0U; index < 8U; ++index)
    {
        NVIC->ICER[index] = 0xFFFFFFFFU;
        NVIC->ICPR[index] = 0xFFFFFFFFU;
    }
}

/**
 * @brief  跳转到指定地址的应用程序
 * @note   跳转流程：
 *         1. 校验应用有效性（栈指针指向 SRAM）
 *         2. 关闭中断并清理外设状态
 *         3. 设置向量表偏移（SCB->VTOR）
 *         4. 设置主栈指针（MSP）
 *         5. 跳转到应用复位向量
 * @warning 跳转后不会返回（正常情况）
 * @param  app_addr — 应用起始地址（Flash 中）
 */
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

    /* 清理中断和外设 */
    iap_cleanup_before_jump();

    /* 设置向量表和栈指针 */
    SCB->VTOR = app_addr;
    __set_MSP(app_stack);

    /* 跳转到应用复位向量 */
    jump_func = (void (*)(void))app_reset;
    jump_func();

    while (1)
    {
    }
}
