#ifndef __LCD_INIT_H
#define __LCD_INIT_H

#include "sys.h"

#define USE_HORIZONTAL 2
#define Chip_Selection 1

#if USE_HORIZONTAL==0||USE_HORIZONTAL==1
#define LCD_W 240
#define LCD_H 320
#else
#define LCD_W 320
#define LCD_H 240
#endif

#define LCD_RES_Clr()  GPIO_ResetBits(GPIOA,GPIO_Pin_4)
#define LCD_RES_Set()  GPIO_SetBits(GPIOA,GPIO_Pin_4)

#define LCD_DC_Clr()   GPIO_ResetBits(GPIOA,GPIO_Pin_3)
#define LCD_DC_Set()   GPIO_SetBits(GPIOA,GPIO_Pin_3)

#define LCD_CS_Clr()   GPIO_ResetBits(GPIOA,GPIO_Pin_2)
#define LCD_CS_Set()   GPIO_SetBits(GPIOA,GPIO_Pin_2)

#define LCD_BLK_Clr()  GPIO_ResetBits(GPIOA,GPIO_Pin_1)
#define LCD_BLK_Set()  GPIO_SetBits(GPIOA,GPIO_Pin_1)

void LCD_Disp_Thermal_DMA(uint8_t *data);
void LCD_GPIO_Init(void);
void LCD_Writ_Bus(u8 dat);
void LCD_WR_DATA8(u8 dat);
void LCD_WR_DATA(u16 dat);
void LCD_WR_REG(u8 dat);
void LCD_Address_Set(u16 x1,u16 y1,u16 x2,u16 y2);
void LCD_Init(void);
void lcd_power_sleep(void);
void lcd_power_wake(void);
void lcd_prepare_gpio_for_low_power(void);
void lcd_restore_gpio_after_low_power(void);

#endif
