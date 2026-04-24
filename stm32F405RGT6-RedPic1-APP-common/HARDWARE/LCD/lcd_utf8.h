#ifndef __LCD_UTF8_H
#define __LCD_UTF8_H

#include "lcd.h"

void LCD_DrawMonoBitmap(u16 x,u16 y,const u8 *bitmap,u8 width,u8 height,u16 fc,u16 bc,u8 mode);
void LCD_ShowUTF8String(u16 x,u16 y,const char *p,u16 fc,u16 bc,u8 sizey,u8 mode);

#endif
