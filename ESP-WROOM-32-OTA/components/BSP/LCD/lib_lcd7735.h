#pragma once

#ifndef _LIB_ST7735_H_
#define _LIB_ST7735_H_

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ESP32_SPI 1

#define LCD_DIR_VAERTICAL1   0
#define LCD_DIR_VAERTICAL2   1
#define LCD_DIR_TRANSVERSET1 2
#define LCD_DIR_TRANSVERSET2 3
#define LCD_DIR_TRANSVERSEt1 LCD_DIR_TRANSVERSET1

#define LCD_DIR LCD_DIR_TRANSVERSET1

#if (LCD_DIR == LCD_DIR_VAERTICAL1 || LCD_DIR == LCD_DIR_VAERTICAL2)
#define LCD_ROW_SIZE    128
#define LCD_COLUMN_SIZE 160
#else
#define LCD_ROW_SIZE    160
#define LCD_COLUMN_SIZE 128
#endif

#define LCD_DELAY(t) vTaskDelay((t) / portTICK_PERIOD_MS)

#define LCD_HARDWARE_CS 1

#if ESP32_SPI
#define LCD_SPI_HOST SPI2_HOST
#define LCD_SPI_MODE 0

#define LCD_PIN_CS  GPIO_NUM_15
#define LCD_PIN_RES GPIO_NUM_25
#define LCD_PIN_A0  GPIO_NUM_26
#define LCD_PIN_SDA GPIO_NUM_13
#define LCD_PIN_SCL GPIO_NUM_14
#define LCD_PIN_BL  GPIO_NUM_4

#define SET_LCD_CS  gpio_set_level(LCD_PIN_CS, 1)
#define SET_LCD_RES gpio_set_level(LCD_PIN_RES, 1)
#define SET_LCD_A0  gpio_set_level(LCD_PIN_A0, 1)
#define SET_LCD_SDA gpio_set_level(LCD_PIN_SDA, 1)
#define SET_LCD_SCL gpio_set_level(LCD_PIN_SCL, 1)
#define SET_LCD_BL  gpio_set_level(LCD_PIN_BL, 1)

#define CLR_LCD_CS  gpio_set_level(LCD_PIN_CS, 0)
#define CLR_LCD_RES gpio_set_level(LCD_PIN_RES, 0)
#define CLR_LCD_A0  gpio_set_level(LCD_PIN_A0, 0)
#define CLR_LCD_SDA gpio_set_level(LCD_PIN_SDA, 0)
#define CLR_LCD_SCL gpio_set_level(LCD_PIN_SCL, 0)
#define CLR_LCD_BL  gpio_set_level(LCD_PIN_BL, 0)
#endif

#define WHITE     0xFFFF
#define BLACK     0x0000
#define BLUE      0x001F
#define BRED      0xF81F
#define GRED      0xFFE0
#define GBLUE     0x07FF
#define RED       0xF800
#define MAGENTA   0xF81F
#define GREEN     0x07E0
#define CYAN      0x7FFF
#define YELLOW    0xFFE0
#define BROWN     0xBC40
#define BRRED     0xFC07
#define GRAY      0x8430
#define DARKBLUE  0x01CF
#define LIGHTBLUE 0x7D7C
#define GRAYBLUE  0x5458

#define u16 uint16_t
#define u8  uint8_t

void LcdGpioSpiInit(void);
void lcdInit(void);
void lcdSetAddress(unsigned short x1, unsigned short y1, unsigned short x2, unsigned short y2);
void lcdClear(unsigned short color);
void lcdWriteDataU16(unsigned short data);

void LCD_DisplayOn(void);
void LCD_DisplayOff(void);
void LCD_PanelWake(void);
void LCD_PanelSleep(void);

void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_Fast_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_Fill(unsigned short sx, unsigned short sy, unsigned short ex, unsigned short ey, unsigned short color);
void LCD_DrawLine(unsigned short x1, unsigned short y1, unsigned short x2, unsigned short y2, unsigned short color);
void LCD_DrawRectangle(unsigned short x1, unsigned short y1, unsigned short x2, unsigned short y2, unsigned short color);
void LCD_Draw_Circle(unsigned short x0, unsigned short y0, unsigned char r, unsigned short color);
void LCD_DrawFullCircle(unsigned short Xpos, unsigned short Ypos, unsigned short Radius, unsigned short Color);

void LCD_ShowChar(unsigned short x, unsigned short y, unsigned char num, unsigned char size, unsigned char mode, unsigned short pen_color, unsigned short back_color);
void LCD_ShowString(unsigned short x, unsigned short y, unsigned short width, unsigned short height, unsigned char size, unsigned char *p, unsigned short pen_color, unsigned short back_color);
void LCD_ShowNum(unsigned short x, unsigned short y, unsigned short len, unsigned short size, unsigned long num, unsigned short pen_color, unsigned short back_color);
void LCD_ShowFloatNum1(unsigned short x, unsigned short y, unsigned short len, unsigned short size, float num, unsigned short pen_color, unsigned short back_color);
void LCD_ShowChinese16x16(u16 x, u16 y, u8 *s, u16 fc, u16 bc, u8 sizey, u8 mode);
void LCD_ShowChinese16x16_spiffs(uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t mode);

void LCD_TEST(void);

uint32_t LCD_Pow(uint8_t m, uint8_t n);

#endif
