/**
 * @file gpio_pm_service.c
 * @brief GPIO低功耗准备服务 —— STOP/STANDBY模式进入前的外设时钟关闭与GPIO状态配置。
 *
 * 本模块负责在进入低功耗模式前：
 *   1. 将各外设引脚重配置为最低漏电状态（模拟输入/输出高/上拉输入）
 *   2. 关闭不再需要的外设时钟（SDIO、ADC1、DMA1/2、GPIOE等）
 *   3. 在唤醒后恢复上述时钟和GPIO配置
 *
 * 设计要点：
 *   - 绝对不能关闭GPIOH时钟（PH0/PH1连接HSE 8MHz晶振，关掉会导致PLL失效、系统降频、看门狗复位）
 *   - SDIO外设必须先清零POWER寄存器再关时钟，否则可能产生总线错误
 *   - DMA流必须先禁用再关时钟，防止中断访问未时钟的外设导致HardFault
 *   - 唤醒恢复时必须在LCD恢复之前先恢复DMA/SDIO/ADC时钟，否则LCD驱动会崩溃
 */
#include "gpio_pm_service.h"
#include "lcd_init.h"
#include "stm32f4xx_conf.h"

/* ===================================================================== */
/*                              常量定义                                   */
/* ===================================================================== */

/** DMA流禁用等待循环次数上限，防止死循环卡死 */
#define GPIO_PM_DMA_DISABLE_WAIT_LOOPS   100000UL

/** SDIO静态标志位掩码，用于清除SDIO->ICR中的所有静态标志 */
#define GPIO_PM_SDIO_STATIC_FLAGS        0x000005FFUL

/* ===================================================================== */
/*                          静态变量（恢复上下文）                            */
/* ===================================================================== */

/**
 * 唤醒后需要恢复的AHB1外设时钟位掩码
 * 记录进入STOP前DMA1、DMA2、GPIOE的使能状态
 */
static uint32_t s_restore_ahb1_mask = 0U;

/**
 * 唤醒后需要恢复的APB2外设时钟位掩码
 * 记录进入STOP前ADC1、SDIO的使能状态
 */
static uint32_t s_restore_apb2_mask = 0U;

/**
 * 进入STOP前ADC1是否处于使能状态（ADON位）
 * 用于唤醒后决定是否需要重新使能ADC1
 */
static uint8_t  s_restore_adc1_enabled = 0U;

/* ===================================================================== */
/*                           内部辅助函数                                   */
/* ===================================================================== */

/**
 * @brief 捕获当前外设时钟使能状态，供唤醒后恢复使用
 *
 * 在进入STOP/STANDBY前调用，记录以下外设的当前状态：
 *   - AHB1: DMA1、DMA2、GPIOE
 *   - APB2: ADC1、SDIO
 *   - ADC1的ADON位（是否正在转换）
 */
static void gpio_pm_capture_restore_state(void)
{
    /* 读取AHB1ENR中DMA1/DMA2/GPIOE的使能位 */
    s_restore_ahb1_mask =
        RCC->AHB1ENR & (RCC_AHB1Periph_DMA1 | RCC_AHB1Periph_DMA2 | RCC_AHB1Periph_GPIOE);

    /* 读取APB2ENR中ADC1/SDIO的使能位 */
    s_restore_apb2_mask = RCC->APB2ENR & (RCC_APB2Periph_ADC1 | RCC_APB2Periph_SDIO);

    s_restore_adc1_enabled = 0U;

    /* 检查ADC1时钟是否使能且ADC处于开启状态（ADON=1） */
    if ((s_restore_apb2_mask & RCC_APB2Periph_ADC1) != 0U &&
        (ADC1->CR2 & ADC_CR2_ADON) != 0U)
    {
        s_restore_adc1_enabled = 1U;
    }
}

/**
 * @brief 安全禁用指定DMA流，带超时保护
 *
 * 先检查DMA流是否已经禁用，若未禁用则发送DISABLE命令并等待。
 * 超时后强制退出，防止DMA控制器异常时死循环。
 *
 * @param stream  DMA流指针（如DMA1_Stream0、DMA2_Stream3等）
 */
static void gpio_pm_disable_dma_stream(DMA_Stream_TypeDef *stream)
{
    uint32_t timeout = GPIO_PM_DMA_DISABLE_WAIT_LOOPS;

    /* 已经禁用则直接返回 */
    if (DMA_GetCmdStatus(stream) == DISABLE)
    {
        return;
    }

    /* 发送禁用命令 */
    DMA_Cmd(stream, DISABLE);

    /* 等待DMA流真正停止（EN位清零） */
    while (DMA_GetCmdStatus(stream) != DISABLE)
    {
        if (timeout-- == 0U)
        {
            break; /* 超时退出，防止卡死 */
        }
    }
}

/**
 * @brief 将USART1引脚配置为最低功耗状态
 *
 * 操作步骤：
 *   1. 禁用USART1外设
 *   2. 关闭USART1时钟
 *   3. PA9(TX) 配置为推挽输出+上拉，输出高电平（空闲态，防止漏电）
 *   4. PA10(RX) 配置为上拉输入（空闲态）
 *
 * 注意：不关闭GPIOA时钟，因为其他引脚可能还在使用
 */
static void gpio_pm_prepare_uart1(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* 禁用USART1外设并关闭时钟 */
    USART_Cmd(USART1, DISABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, DISABLE);

    /* 确保GPIOA时钟开启（用于配置PA9/PA10） */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

    /* PA9(TX) -> 推挽输出高电平（UART空闲态，最低漏电） */
    gpio_init.GPIO_Pin   = GPIO_Pin_9;
    gpio_init.GPIO_Mode  = GPIO_Mode_OUT;
    gpio_init.GPIO_PuPd  = GPIO_PuPd_UP;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOA, &gpio_init);
    GPIO_SetBits(GPIOA, GPIO_Pin_9);

    /* PA10(RX) -> 上拉输入（UART空闲态） */
    gpio_init.GPIO_Pin  = GPIO_Pin_10;
    gpio_init.GPIO_Mode = GPIO_Mode_IN;
    gpio_init.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &gpio_init);
}

/**
 * @brief 将MLX90640热成像传感器的I2C引脚配置为最低功耗状态
 *
 * 操作步骤：
 *   1. 禁用I2C1外设
 *   2. 关闭I2C1时钟
 *   3. PB6(SCL)/PB7(SDA) 配置为模拟输入模式
 *      - 模拟输入模式断开施密特触发器，消除数字输入级的漏电流
 *      - 开漏模式配合无上下拉，I2C总线呈高阻态
 */
static void gpio_pm_prepare_mlx90640_i2c(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* 禁用I2C1外设并关闭时钟 */
    I2C_Cmd(I2C1, DISABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, DISABLE);

    /* 确保GPIOB时钟开启 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    /* PB6(SCL)/PB7(SDA) -> 模拟输入，无上下拉，开漏 */
    gpio_init.GPIO_Pin   = GPIO_Pin_6 | GPIO_Pin_7;
    gpio_init.GPIO_Mode  = GPIO_Mode_AN;
    gpio_init.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio_init.GPIO_OType = GPIO_OType_OD;
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOB, &gpio_init);
}

/**
 * @brief 关闭ADC1时钟并将PB0(电池电压检测引脚)配置为模拟输入
 *
 * ADC1用于电池电压监测，休眠期间不需要采样。
 * PB0配置为模拟输入可断开施密特触发器，消除数字输入级漏电流。
 *
 * 注意：先将PB0配置为模拟输入再关ADC时钟，确保引脚状态正确。
 */
static void gpio_pm_prepare_adc1(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* 确保GPIOB时钟开启 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    /* PB0 -> 模拟输入（电池ADC引脚，断开施密特触发器） */
    gpio_init.GPIO_Pin   = GPIO_Pin_0;
    gpio_init.GPIO_Mode  = GPIO_Mode_AN;
    gpio_init.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOB, &gpio_init);

    /* 如果ADC1时钟已使能，先禁用ADC再关时钟 */
    if ((RCC->APB2ENR & RCC_APB2Periph_ADC1) != 0U)
    {
        ADC_Cmd(ADC1, DISABLE);
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, DISABLE);
    }
}

/**
 * @brief 安全关闭SDIO外设并将相关引脚配置为最低功耗状态
 *
 * SDIO使用GPIOC(PC8~PC12)和GPIOD(PD2)。
 * 关闭顺序很重要：
 *   1. 先将引脚配置为模拟输入（断开施密特触发器）
 *   2. 清零SDIO寄存器（MASK→DCTRL→CMD→CLKCR→POWER→ICR）
 *   3. 最后关闭SDIO时钟
 *
 * 必须先清POWER寄存器再关时钟，否则SDIO控制器可能在时钟关闭瞬间产生总线错误。
 */
static void gpio_pm_prepare_sdio(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* 确保GPIOC/GPIOD时钟开启（用于配置引脚） */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOD, ENABLE);

    /* PC8~PC12 -> 模拟输入（SDIO数据线和CMD线） */
    gpio_init.GPIO_Pin   = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12;
    gpio_init.GPIO_Mode  = GPIO_Mode_AN;
    gpio_init.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOC, &gpio_init);

    /* PD2 -> 模拟输入（SDIO时钟线） */
    gpio_init.GPIO_Pin = GPIO_Pin_2;
    GPIO_Init(GPIOD, &gpio_init);

    /* 如果SDIO时钟已使能，按安全顺序清零寄存器并关时钟 */
    if ((RCC->APB2ENR & RCC_APB2Periph_SDIO) != 0U)
    {
        SDIO->MASK  = 0U;                          /* 关闭所有SDIO中断 */
        SDIO->DCTRL = 0U;                          /* 清零数据控制寄存器 */
        SDIO->CMD   = 0U;                          /* 清零命令寄存器 */
        SDIO->CLKCR = 0U;                          /* 清零时钟控制寄存器（关闭SDIO_CK输出） */
        SDIO->POWER = 0U;                          /* 关闭SDIO电源（必须在CLKCR之后） */
        SDIO->ICR   = GPIO_PM_SDIO_STATIC_FLAGS;   /* 清除所有挂起的中断标志 */
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_SDIO, DISABLE); /* 最后关闭SDIO外设时钟 */
    }
}

/**
 * @brief 关闭DMA1和DMA2时钟
 *
 * 操作顺序：
 *   1. 先禁用各个DMA流（DMA1_Stream0、DMA2_Stream3、DMA2_Stream6）
 *   2. 清除各流的中断标志
 *   3. 最后关闭DMA1/DMA2时钟
 *
 * 必须先禁用流再关时钟，否则DMA中断可能在时钟关闭后访问未时钟的外设导致HardFault。
 *
 * DMA流用途：
 *   - DMA1_Stream0: 可能用于SPI/UART等外设
 *   - DMA2_Stream3: LCD显示DMA传输
 *   - DMA2_Stream6: SDIO数据传输
 */
static void gpio_pm_prepare_dma_clocks(void)
{
    /* 关闭DMA1时钟（如果已使能） */
    if ((RCC->AHB1ENR & RCC_AHB1Periph_DMA1) != 0U)
    {
        /* 禁用DMA1_Stream0并清除所有中断标志 */
        gpio_pm_disable_dma_stream(DMA1_Stream0);
        DMA_ClearFlag(DMA1_Stream0,
                      DMA_FLAG_FEIF0 | DMA_FLAG_DMEIF0 | DMA_FLAG_TEIF0 |
                      DMA_FLAG_HTIF0 | DMA_FLAG_TCIF0);
        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, DISABLE);
    }

    /* 关闭DMA2时钟（如果已使能） */
    if ((RCC->AHB1ENR & RCC_AHB1Periph_DMA2) != 0U)
    {
        /* 禁用DMA2_Stream3（LCD DMA）并清除标志 */
        gpio_pm_disable_dma_stream(DMA2_Stream3);
        DMA_ClearFlag(DMA2_Stream3,
                      DMA_FLAG_FEIF3 | DMA_FLAG_DMEIF3 | DMA_FLAG_TEIF3 |
                      DMA_FLAG_HTIF3 | DMA_FLAG_TCIF3);

        /* 禁用DMA2_Stream6（SDIO DMA）并清除标志 */
        gpio_pm_disable_dma_stream(DMA2_Stream6);
        DMA_ClearFlag(DMA2_Stream6,
                      DMA_FLAG_FEIF6 | DMA_FLAG_DMEIF6 | DMA_FLAG_TEIF6 |
                      DMA_FLAG_HTIF6 | DMA_FLAG_TCIF6);

        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, DISABLE);
    }
}

/**
 * @brief 关闭未使用的GPIO端口时钟
 *
 * 当前仅关闭GPIOE时钟（原理图上PE引脚未连接外设）。
 *
 * 注意：绝对不能关闭GPIOH时钟！PH0/PH1连接HSE 8MHz晶振，
 * 关闭GPIOH会导致HSE停振→PLL失效→系统降频至16MHz HSI→看门狗复位。
 */
static void gpio_pm_prepare_unused_gpio_clocks(void)
{
    if ((RCC->AHB1ENR & RCC_AHB1Periph_GPIOE) != 0U)
    {
        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, DISABLE);
    }
}

/**
 * @brief 唤醒后恢复进入STOP前关闭的外设时钟
 *
 * 根据gpio_pm_capture_restore_state()记录的状态，恢复：
 *   - AHB1: DMA1、DMA2、GPIOE
 *   - APB2: ADC1、SDIO
 *   - 如果ADC1之前处于开启状态，重新使能ADC
 *
 * 注意：必须在LCD恢复之前调用，因为LCD驱动依赖DMA时钟。
 */
static void gpio_pm_restore_extra_clocks(void)
{
    /* 恢复AHB1外设时钟（DMA1/DMA2/GPIOE） */
    if (s_restore_ahb1_mask != 0U)
    {
        RCC_AHB1PeriphClockCmd(s_restore_ahb1_mask, ENABLE);
    }

    /* 恢复APB2外设时钟（ADC1/SDIO） */
    if (s_restore_apb2_mask != 0U)
    {
        RCC_APB2PeriphClockCmd(s_restore_apb2_mask, ENABLE);
    }

    /* 如果ADC1之前处于开启状态，重新使能ADC */
    if (s_restore_adc1_enabled != 0U &&
        (s_restore_apb2_mask & RCC_APB2Periph_ADC1) != 0U)
    {
        ADC_Cmd(ADC1, ENABLE);
    }
}

/* ===================================================================== */
/*                           公共API实现                                   */
/* ===================================================================== */

/**
 * @brief 进入STOP模式前的GPIO和外设低功耗准备
 *
 * 完整的准备序列：
 *   1. 捕获当前时钟状态（供唤醒恢复使用）
 *   2. LCD引脚低功耗配置
 *   3. UART1引脚低功耗配置
 *   4. MLX90640 I2C引脚低功耗配置
 *   5. ADC1关闭 + PB0模拟输入
 *   6. SDIO寄存器清零 + 引脚模拟输入 + 关时钟
 *   7. DMA流禁用 + DMA时钟关闭
 *   8. GPIOE时钟关闭
 *
 * 调用方：low_power_runtime.c → app_pm_prepare_stop() → 本函数
 */
void gpio_pm_prepare_stop(void)
{
    gpio_pm_capture_restore_state();        /* 记录当前时钟使能状态 */
    lcd_prepare_gpio_for_low_power();       /* LCD引脚低功耗配置 */
    gpio_pm_prepare_uart1();                /* UART1引脚低功耗配置 */
    gpio_pm_prepare_mlx90640_i2c();         /* 热成像I2C引脚低功耗配置 */
    gpio_pm_prepare_adc1();                 /* ADC1关闭 + PB0模拟输入 */
    gpio_pm_prepare_sdio();                 /* SDIO寄存器清零 + 引脚模拟输入 + 关时钟 */
    gpio_pm_prepare_dma_clocks();           /* DMA流禁用 + DMA时钟关闭 */
    gpio_pm_prepare_unused_gpio_clocks();   /* GPIOE时钟关闭 */
}

/**
 * @brief 从STOP模式唤醒后的GPIO和外设时钟恢复
 *
 * 恢复顺序：
 *   1. 先恢复DMA/SDIO/ADC/GPIOE时钟（LCD驱动依赖DMA）
 *   2. 再恢复LCD GPIO配置
 *
 * 调用方：low_power_runtime.c → app_pm_restore_stop() → 本函数
 */
void gpio_pm_restore_after_stop(void)
{
    gpio_pm_restore_extra_clocks();         /* 恢复DMA/SDIO/ADC/GPIOE时钟 */
    lcd_restore_gpio_after_low_power();     /* 恢LCD GPIO配置 */
}

/**
 * @brief 进入STANDBY模式前的GPIO和外设低功耗准备
 *
 * 与gpio_pm_prepare_stop()执行相同的操作序列。
 * STANDBY模式下SRAM内容丢失，唤醒后相当于复位，因此不需要恢复函数。
 * 但为了保持一致性，仍然执行相同的低功耗配置。
 *
 * 调用方：low_power_runtime.c → app_pm_prepare_standby() → 本函数
 */
void gpio_pm_prepare_standby(void)
{
    gpio_pm_capture_restore_state();        /* 记录当前时钟使能状态 */
    lcd_prepare_gpio_for_low_power();       /* LCD引脚低功耗配置 */
    gpio_pm_prepare_uart1();                /* UART1引脚低功耗配置 */
    gpio_pm_prepare_mlx90640_i2c();         /* 热成像I2C引脚低功耗配置 */
    gpio_pm_prepare_adc1();                 /* ADC1关闭 + PB0模拟输入 */
    gpio_pm_prepare_sdio();                 /* SDIO寄存器清零 + 引脚模拟输入 + 关时钟 */
    gpio_pm_prepare_dma_clocks();           /* DMA流禁用 + DMA时钟关闭 */
    gpio_pm_prepare_unused_gpio_clocks();   /* GPIOE时钟关闭 */
}
