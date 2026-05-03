#include "lcd_init.h"
#include "delay.h"
#include "lcd_dma.h"

/* C6 修复：SPI 忙等超时计数器（168MHz CPU 约 3ms 超时） */
#define LCD_SPI_TIMEOUT_LOOPS  500000UL

static uint8_t s_lcd_sleeping = 0U;

static void lcd_spi_gpio_apply(uint8_t keep_backlight_on)
{
    SPI_InitTypeDef spi_init_structure;
    GPIO_InitTypeDef gpio_init_structure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);

    gpio_init_structure.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    gpio_init_structure.GPIO_Mode = GPIO_Mode_AF;
    gpio_init_structure.GPIO_OType = GPIO_OType_PP;
    gpio_init_structure.GPIO_PuPd = GPIO_PuPd_UP;
    gpio_init_structure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(GPIOA, &gpio_init_structure);

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource5, GPIO_AF_SPI1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_SPI1);

    gpio_init_structure.GPIO_Pin = GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4;
    gpio_init_structure.GPIO_Mode = GPIO_Mode_OUT;
    gpio_init_structure.GPIO_OType = GPIO_OType_PP;
    gpio_init_structure.GPIO_Speed = GPIO_Speed_100MHz;
    gpio_init_structure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &gpio_init_structure);

    GPIO_SetBits(GPIOA, GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4);
    if (keep_backlight_on != 0U)
    {
        GPIO_SetBits(GPIOA, GPIO_Pin_1);
    }
    else
    {
        GPIO_ResetBits(GPIOA, GPIO_Pin_1);
    }

    spi_init_structure.SPI_Direction = SPI_Direction_1Line_Tx;
    spi_init_structure.SPI_Mode = SPI_Mode_Master;
    spi_init_structure.SPI_DataSize = SPI_DataSize_8b;
    spi_init_structure.SPI_CPOL = SPI_CPOL_Low;
    spi_init_structure.SPI_CPHA = SPI_CPHA_1Edge;
    spi_init_structure.SPI_NSS = SPI_NSS_Soft;
    spi_init_structure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;
    spi_init_structure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_Init(SPI1, &spi_init_structure);
    SPI_Cmd(SPI1, ENABLE);
}

static void lcd_apply_orientation(void)
{
    LCD_WR_REG(0x36);
    if (USE_HORIZONTAL == 0)
    {
        LCD_WR_DATA8(0x00);
    }
    else if (USE_HORIZONTAL == 1)
    {
        LCD_WR_DATA8(0xC0);
    }
    else if (USE_HORIZONTAL == 2)
    {
        LCD_WR_DATA8(0x70);
    }
    else
    {
        LCD_WR_DATA8(0xA0);
    }

    LCD_WR_REG(0x3A);
    LCD_WR_DATA8(0x05);
}

void LCD_GPIO_Init(void)
{
    lcd_spi_gpio_apply(0U);
}

void LCD_Writ_Bus(u8 dat)
{
    uint32_t timeout;

    LCD_CS_Clr();

    timeout = LCD_SPI_TIMEOUT_LOOPS;
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
    {
        if (--timeout == 0U) { LCD_CS_Set(); return; }
    }

    SPI_I2S_SendData(SPI1, dat);

    timeout = LCD_SPI_TIMEOUT_LOOPS;
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)
    {
        if (--timeout == 0U) { break; }
    }

    LCD_CS_Set();
}

void LCD_WR_DATA8(u8 dat)
{
    LCD_DC_Set();
    LCD_Writ_Bus(dat);
}

void LCD_WR_DATA(u16 dat)
{
    uint32_t timeout;

    LCD_DC_Set();
    LCD_CS_Clr();

    timeout = LCD_SPI_TIMEOUT_LOOPS;
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
    {
        if (--timeout == 0U) { LCD_CS_Set(); return; }
    }
    SPI_I2S_SendData(SPI1, (dat >> 8) & 0xFF);

    timeout = LCD_SPI_TIMEOUT_LOOPS;
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
    {
        if (--timeout == 0U) { LCD_CS_Set(); return; }
    }
    SPI_I2S_SendData(SPI1, dat & 0xFF);

    timeout = LCD_SPI_TIMEOUT_LOOPS;
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)
    {
        if (--timeout == 0U) { break; }
    }
    LCD_CS_Set();
}

void LCD_WR_REG(u8 dat)
{
    LCD_DC_Clr();
    LCD_Writ_Bus(dat);
    LCD_DC_Set();
}

void LCD_Address_Set(u16 x1,u16 y1,u16 x2,u16 y2)
{
    LCD_WR_REG(0x2A);
    LCD_WR_DATA(x1);
    LCD_WR_DATA(x2);
    LCD_WR_REG(0x2B);
    LCD_WR_DATA(y1);
    LCD_WR_DATA(y2);
    LCD_WR_REG(0x2C);
}

void LCD_Init(void)
{
    LCD_GPIO_Init();
    LCD_RES_Clr();
    delay_ms(100);
    LCD_RES_Set();
    delay_ms(100);

    LCD_WR_REG(0x11);
    delay_ms(120);
    lcd_apply_orientation();

    LCD_WR_REG(0xB2);
    LCD_WR_DATA8(0x0C);
    LCD_WR_DATA8(0x0C);
    LCD_WR_DATA8(0x00);
    LCD_WR_DATA8(0x33);
    LCD_WR_DATA8(0x33);
    LCD_WR_REG(0xB7);
    LCD_WR_DATA8(0x35);

    LCD_WR_REG(0xBB);
    LCD_WR_DATA8(0x35);
    LCD_WR_REG(0xC0);
    LCD_WR_DATA8(0x2C);
    LCD_WR_REG(0xC2);
    LCD_WR_DATA8(0x01);
    LCD_WR_REG(0xC3);
    LCD_WR_DATA8(0x13);
    LCD_WR_REG(0xC4);
    LCD_WR_DATA8(0x20);
    LCD_WR_REG(0xC6);
    LCD_WR_DATA8(0x0F);
    LCD_WR_REG(0xCA);
    LCD_WR_DATA8(0x0F);
    LCD_WR_REG(0xC8);
    LCD_WR_DATA8(0x08);
    LCD_WR_REG(0x55);
    LCD_WR_DATA8(0x90);
    LCD_WR_REG(0xD0);
    LCD_WR_DATA8(0xA4);
    LCD_WR_DATA8(0xA1);

    LCD_WR_REG(0xE0);
    LCD_WR_DATA8(0xD0);
    LCD_WR_DATA8(0x00);
    LCD_WR_DATA8(0x06);
    LCD_WR_DATA8(0x09);
    LCD_WR_DATA8(0x0B);
    LCD_WR_DATA8(0x2A);
    LCD_WR_DATA8(0x3C);
    LCD_WR_DATA8(0x55);
    LCD_WR_DATA8(0x4B);
    LCD_WR_DATA8(0x08);
    LCD_WR_DATA8(0x16);
    LCD_WR_DATA8(0x14);
    LCD_WR_DATA8(0x19);
    LCD_WR_DATA8(0x20);

    LCD_WR_REG(0xE1);
    LCD_WR_DATA8(0xD0);
    LCD_WR_DATA8(0x00);
    LCD_WR_DATA8(0x06);
    LCD_WR_DATA8(0x09);
    LCD_WR_DATA8(0x0B);
    LCD_WR_DATA8(0x29);
    LCD_WR_DATA8(0x36);
    LCD_WR_DATA8(0x54);
    LCD_WR_DATA8(0x4B);
    LCD_WR_DATA8(0x0D);
    LCD_WR_DATA8(0x16);
    LCD_WR_DATA8(0x14);
    LCD_WR_DATA8(0x21);
    LCD_WR_DATA8(0x20);
    LCD_WR_REG(0x29);
    LCD_BLK_Set();

    s_lcd_sleeping = 0U;
}

void lcd_power_sleep(void)
{
    if (s_lcd_sleeping != 0U)
    {
        return;
    }

    LCD_WR_REG(0x28);
    delay_ms(20);
    LCD_WR_REG(0x10);
    delay_ms(120);
    LCD_BLK_Clr();
    s_lcd_sleeping = 1U;
}

void lcd_power_wake(void)
{
    if (s_lcd_sleeping == 0U)
    {
        return;
    }

    LCD_WR_REG(0x11);
    delay_ms(120);
    lcd_apply_orientation();
    LCD_WR_REG(0x29);
    LCD_BLK_Set();
    s_lcd_sleeping = 0U;
}

void lcd_prepare_gpio_for_low_power(void)
{
    GPIO_InitTypeDef gpio_init_structure;

    SPI_Cmd(SPI1, DISABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, DISABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

    gpio_init_structure.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_7;
    gpio_init_structure.GPIO_Mode = GPIO_Mode_AN;
    gpio_init_structure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    gpio_init_structure.GPIO_OType = GPIO_OType_PP;
    gpio_init_structure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOA, &gpio_init_structure);
}

void lcd_restore_gpio_after_low_power(void)
{
    lcd_spi_gpio_apply((s_lcd_sleeping == 0U) ? 1U : 0U);
}
