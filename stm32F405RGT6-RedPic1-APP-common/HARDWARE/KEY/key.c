#include "key.h"
#include "sys.h"
#include "delay.h"
#include "exti_key.h"
#include "lcd_dma.h"


/* 初始化三个物理按键输入脚。
 * 当前工程的主要按键事件来自 EXTI + TIM3 消抖链路，这里只负责 GPIO 输入配置。 */
void KEY_Init(void)
{
    GPIO_InitTypeDef gpio_init_structure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOB, ENABLE);

    gpio_init_structure.GPIO_Pin = GPIO_Pin_13;
    gpio_init_structure.GPIO_Mode = GPIO_Mode_IN;
    gpio_init_structure.GPIO_Speed = GPIO_Speed_100MHz;
    gpio_init_structure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOC, &gpio_init_structure);
    GPIO_SetBits(GPIOC, GPIO_Pin_13);

    gpio_init_structure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9;
    gpio_init_structure.GPIO_Mode = GPIO_Mode_IN;
    gpio_init_structure.GPIO_Speed = GPIO_Speed_100MHz;
    gpio_init_structure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &gpio_init_structure);
    GPIO_SetBits(GPIOB, GPIO_Pin_8 | GPIO_Pin_9);
}

/* 轮询式按键扫描接口。
 * 这是旧接口，当前主流程更多依赖 KEY_GetValue() 返回的消抖后事件。 */
u8 KEY_Scan(u8 mode)
{
    static u8 key_up = 1U;

    if (mode != 0U)
    {
        key_up = 1U;
    }

    if (key_up != 0U && (KEY1 == 0 || KEY2 == 0 || KEY3 == 0))
    {
        delay_ms(10);
        key_up = 0U;

        if (KEY1 == 0)
        {
            return KEY1_PRES;
        }
        if (KEY2 == 0)
        {
            return KEY2_PRES;
        }
        if (KEY3 == 0)
        {
            return KEY3_PRES;
        }
    }
    else if (KEY1 == 1 && KEY2 == 1 && KEY3 == 1)
    {
        key_up = 1U;
    }

    return 0U;
}

