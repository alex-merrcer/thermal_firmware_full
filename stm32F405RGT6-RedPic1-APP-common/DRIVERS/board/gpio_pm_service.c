/**
 * @file gpio_pm_service.c
 * @brief GPIO低功耗管理适配层核心实现
 * @details 架构说明：
 *          1. 基于STM32F4标准外设库(SPL)，在切断外设时钟后重配置引脚，避免总线冲突与功耗尖峰
 *          2. 遵循ARM Cortex-M低功耗最佳实践：悬空引脚模拟化、通信引脚电平固定化、关键状态显式恢复
 *          3. 与电源状态机(low_power_runtime)严格对齐，确保休眠/唤醒生命周期闭环
 */
#include "gpio_pm_service.h"
#include "lcd_init.h"
#include "stm32f4xx_conf.h"


/**
 * @brief 休眠前重配置USART1引脚（PA9_TX / PA10_RX）
 * @details 硬件设计意图：
 *          1. 关闭USART1及APB2时钟，防止休眠期间外设寄存器访问引发总线挂起
 *          2. TX(PA9)配置为推挽输出+上拉+高电平：确保ESP32 RX端在休眠期间维持Idle高电平，
 *             避免STM32内部电平漂移被对端误判为起始位(Start Bit)导致虚假唤醒或帧错误
 *          3. RX(PA10)配置为浮空/上拉输入：固定引脚状态，消除悬空漏电流
 */
static void gpio_pm_prepare_uart1(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* 1. 安全关闭外设与时钟（必须先于GPIO重配置） */
    USART_Cmd(USART1, DISABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, DISABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

    /* 2. 配置 TX(PA9) 为输出高电平，阻断对端误触发 */
    gpio_init.GPIO_Pin   = GPIO_Pin_9;
    gpio_init.GPIO_Mode  = GPIO_Mode_OUT;
    gpio_init.GPIO_PuPd  = GPIO_PuPd_UP;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOA, &gpio_init);
    GPIO_SetBits(GPIOA, GPIO_Pin_9); /* 显式拉高，维持总线Idle状态 */

    /* 3. 配置 RX(PA10) 为输入上拉，消除悬空漏电 */
    gpio_init.GPIO_Pin   = GPIO_Pin_10;
    gpio_init.GPIO_Mode  = GPIO_Mode_IN;
    gpio_init.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &gpio_init);
}

/**
 * @brief 休眠前重配置I2C1引脚（PB6_SCL / PB7_SDA，连接MLX90640）
 * @details 硬件设计意图：
 *          1. 关闭I2C1及APB1时钟，释放总线控制权
 *          2. 引脚配置为模拟输入(GPIO_Mode_AN)：STM32F4在模拟模式下会断开内部数字施密特触发器，
 *             这是官方推荐的最低漏电配置（典型漏电流 < 1uA）
 *          3. 注：SPL中GPIO_OType与GPIO_Speed在模拟模式下被硬件忽略，此处保留赋值仅为满足结构体完整性
 */
static void gpio_pm_prepare_mlx90640_i2c(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* 1. 安全关闭I2C外设与时钟 */
    I2C_Cmd(I2C1, DISABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, DISABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    /* 2. 配置 SCL(PB6) / SDA(PB7) 为模拟输入，实现最低休眠漏电 */
    gpio_init.GPIO_Pin   = GPIO_Pin_6 | GPIO_Pin_7;
    gpio_init.GPIO_Mode  = GPIO_Mode_AN;
    gpio_init.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio_init.GPIO_OType = GPIO_OType_OD; /* 硬件忽略，仅占位 */
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz; /* 硬件忽略，仅占位 */
    GPIO_Init(GPIOB, &gpio_init);
}

/* ======================== 公开API实现 ======================== */

/**
 * @brief 准备进入STOP停机模式
 * @details 执行顺序：LCD背光/控制线 -> UART1 -> I2C1
 *          STOP模式下SRAM与寄存器内容保留，因此仅需处理可能产生漏电或误唤醒的引脚。
 */
void gpio_pm_prepare_stop(void)
{
    lcd_prepare_gpio_for_low_power();
    gpio_pm_prepare_uart1();
    gpio_pm_prepare_mlx90640_i2c();
}

/**
 * @brief STOP模式唤醒后恢复GPIO状态
 * @details STOP唤醒后，GPIO寄存器状态由硬件自动保持，无需重新初始化。
 *          此处仅调用LCD恢复函数，因为LCD驱动通常涉及外部电源IC或背光PWM，
 *          需要显式重新使能。UART/I2C的时钟与外设恢复交由各自业务驱动在首次调用时完成。
 */
void gpio_pm_restore_after_stop(void)
{
    lcd_restore_gpio_after_low_power();
}

/**
 * @brief 准备进入STANDBY待机模式
 * @details STANDBY模式下除备份域外所有寄存器丢失，但进入前的引脚状态仍会影响
 *          过渡阶段的漏电流与唤醒信号完整性。当前序列与STOP保持一致，
 *          独立封装是为了满足低功耗状态机的对称性设计，并为未来备份域唤醒引脚(WKUP)预留扩展点。
 */
void gpio_pm_prepare_standby(void)
{
    lcd_prepare_gpio_for_low_power();
    gpio_pm_prepare_uart1();
    gpio_pm_prepare_mlx90640_i2c();
}
