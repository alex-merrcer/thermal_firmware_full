#include "lcd.h"
#include "lcd_init.h"
#include "lcdfont_boot.h"

static const unsigned char *lcd_boot_get_glyph(u8 num, u8 sizey, u8 *sizex, u16 *glyph_len)
{
    if (num < ' ' || num > '~')
    {
        num = '?';
    }

    num = (u8)(num - ' ');
    if (sizey == 12U)
    {
        *sizex = 6U;
        *glyph_len = 12U;
        return ascii_1206[num];
    }

    if (sizey == 16U)
    {
        *sizex = 8U;
        *glyph_len = 16U;
        return ascii_1608[num];
    }

    *sizex = 0U;
    *glyph_len = 0U;
    return 0;
}

void LCD_Fill(u16 xsta, u16 ysta, u16 xend, u16 yend, u16 color)
{
    u16 x = 0U;
    u16 y = 0U;

    if (xend <= xsta || yend <= ysta)
    {
        return;
    }

    LCD_Address_Set(xsta, ysta, (u16)(xend - 1U), (u16)(yend - 1U));
    for (y = ysta; y < yend; ++y)
    {
        for (x = xsta; x < xend; ++x)
        {
            LCD_WR_DATA(color);
        }
    }
}

void LCD_DrawPoint(u16 x, u16 y, u16 color)
{
    LCD_Address_Set(x, y, x, y);
    LCD_WR_DATA(color);
}

void LCD_DrawRectangle(u16 x1, u16 y1, u16 x2, u16 y2, u16 color)
{
    if (x2 < x1 || y2 < y1)
    {
        return;
    }

    LCD_Fill(x1, y1, (u16)(x2 + 1U), (u16)(y1 + 1U), color);
    LCD_Fill(x1, y2, (u16)(x2 + 1U), (u16)(y2 + 1U), color);

    if (y2 > y1 + 1U)
    {
        LCD_Fill(x1, (u16)(y1 + 1U), (u16)(x1 + 1U), y2, color);
        LCD_Fill(x2, (u16)(y1 + 1U), (u16)(x2 + 1U), y2, color);
    }
}

void LCD_ShowChar(u16 x, u16 y, u8 num, u16 fc, u16 bc, u8 sizey, u8 mode)
{
    const unsigned char *glyph = 0;
    u8 sizex = 0U;
    u16 glyph_len = 0U;
    u16 row = 0U;
    u8 bit = 0U;
    u16 x0 = x;
    u16 y0 = y;
    u8 value = 0U;

    glyph = lcd_boot_get_glyph(num, sizey, &sizex, &glyph_len);
    if (glyph == 0 || sizex == 0U || glyph_len == 0U)
    {
        return;
    }

    if (mode == 0U)
    {
        LCD_Address_Set(x, y, (u16)(x + sizex - 1U), (u16)(y + sizey - 1U));
    }

    for (row = 0U; row < glyph_len; ++row)
    {
        value = glyph[row];
        for (bit = 0U; bit < 8U && bit < sizex; ++bit)
        {
            if (mode == 0U)
            {
                LCD_WR_DATA((value & (u8)(0x01U << bit)) != 0U ? fc : bc);
            }
            else
            {
                LCD_DrawPoint((u16)(x0 + bit), y0, (value & (u8)(0x01U << bit)) != 0U ? fc : bc);
            }
        }
        ++y0;
    }
}

void LCD_ShowString(u16 x, u16 y, const u8 *p, u16 fc, u16 bc, u8 sizey, u8 mode)
{
    u8 sizex = (sizey == 16U) ? 8U : 6U;

    if (p == 0)
    {
        return;
    }

    while (*p != '\0')
    {
        LCD_ShowChar(x, y, *p, fc, bc, sizey, mode);
        x = (u16)(x + sizex);
        ++p;
    }
}

u32 mypow(u8 m, u8 n)
{
    u32 result = 1U;
    while (n-- > 0U)
    {
        result *= m;
    }
    return result;
}

void LCD_ShowIntNum(u16 x, u16 y, u16 num, u8 len, u16 fc, u16 bc, u8 sizey)
{
    u8 t = 0U;
    u8 temp = 0U;
    u8 enshow = 0U;
    u8 sizex = (sizey == 16U) ? 8U : 6U;

    for (t = 0U; t < len; ++t)
    {
        temp = (u8)((num / mypow(10U, (u8)(len - t - 1U))) % 10U);
        if (enshow == 0U && t < (u8)(len - 1U))
        {
            if (temp == 0U)
            {
                LCD_ShowChar((u16)(x + t * sizex), y, ' ', fc, bc, sizey, 0U);
                continue;
            }
            enshow = 1U;
        }

        LCD_ShowChar((u16)(x + t * sizex), y, (u8)(temp + '0'), fc, bc, sizey, 0U);
    }
}
