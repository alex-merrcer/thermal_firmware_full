/**
 * @file    ui_renderer.c
 * @brief   UI 渲染引擎 —— 产品主题绘制原语与本地化
 * @note    本模块提供所有 UI 页面共享的渲染原语，采用深蓝科技主题配色。
 *
 * @par 主题配色
 *      - 背景色:       #05121E (深蓝黑)
 *      - 面板色:       #0A2030 (深蓝灰)
 *      - 面板边框:     #304E60 (蓝灰)
 *      - 强调色:       #FF7800 (橙色)
 *      - 青色:         #5ADCE6 (科技青)
 *      - 成功色:       #8ED85C (绿)
 *      - 警告色:       #FFC65A (黄)
 *      - 错误色:       #E85C5C (红)
 *
 * @par 本地化机制
 *      ui_renderer_localize() 函数通过 strcmp 映射将英文文本转换为
 *      中文 UTF-8 编码的字节序列。覆盖菜单项、状态文本、按键提示等。
 *
 * @par 渲染原语
 *      - 页眉绘制（标题、状态图标、路径导航）
 *      - 列表项、开关项、选项项、数值行
 *      - 电池/WiFi/蓝牙图标
 *      - UTF-8 文本宽度计算与自适应裁剪
 *      - 产品背景（网格点阵、电路线条）
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "ui_renderer.h"

#include <stdio.h>
#include <string.h>

#include "app_display_runtime.h"
#include "esp_host_service.h"
#include "lcd_init.h"
#include "lcd.h"
#include "lcd_utf8.h"

/* =========================================================================
 *  2. 内部宏定义 —— 颜色与布局常量
 * ======================================================================= */

/** RGB888 → RGB565 颜色转换宏 */
#define UI_RGB565(r, g, b) ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | \
                                       (((uint16_t)(g) & 0xFCU) << 3) | \
                                       (((uint16_t)(b) & 0xF8U) >> 3)))

/* --- 文本布局坐标 --- */
#define UI_TEXT_LEFT_X              12U     /**< 文本左起始 X          */
#define UI_VALUE_X                  180U    /**< 数值左起始 X          */
#define UI_ITEM_LEFT_X              12U     /**< 列表项左起始 X        */
#define UI_ITEM_VALUE_X             232U    /**< 列表项数值 X          */
#define UI_UTF8_FONT_SIZE           16U     /**< UTF-8 字体尺寸        */

/* --- 页眉布局常量 --- */
#define UI_HEADER_TITLE_X           8U      /**< 页眉标题 X            */
#define UI_HEADER_TITLE_Y           7U      /**< 页眉标题 Y            */
#define UI_HEADER_STATUS_RIGHT_MARGIN 8U    /**< 状态图标右边距        */
#define UI_HEADER_STATUS_GAP        8U      /**< 状态图标间距          */

/* --- 电池图标尺寸 --- */
#define UI_HEADER_BATTERY_WIDTH     16U     /**< 电池主体宽度          */
#define UI_HEADER_BATTERY_HEIGHT    10U     /**< 电池主体高度          */
#define UI_HEADER_BATTERY_TIP_WIDTH 2U      /**< 电池正极宽度          */
#define UI_HEADER_BATTERY_TIP_HEIGHT 4U     /**< 电池正极高度          */

/* --- WiFi 图标尺寸 --- */
#define UI_HEADER_WIFI_WIDTH        16U     /**< WiFi 图标宽度         */
#define UI_HEADER_WIFI_HEIGHT       12U     /**< WiFi 图标高度         */

/* --- 蓝牙图标尺寸 --- */
#define UI_HEADER_BT_WIDTH          10U     /**< 蓝牙图标宽度          */
#define UI_HEADER_BT_HEIGHT         14U     /**< 蓝牙图标高度          */

/* --- 产品主题色 --- */
#define UI_PRODUCT_BG_COLOR         UI_RGB565(5U, 18U, 30U)       /**< 主背景色      */
#define UI_PRODUCT_GRID_COLOR       UI_RGB565(13U, 34U, 48U)      /**< 网格点颜色    */
#define UI_PRODUCT_HEADER_COLOR     UI_RGB565(3U, 13U, 22U)       /**< 页眉背景色    */
#define UI_PRODUCT_PANEL_COLOR      UI_RGB565(10U, 32U, 48U)      /**< 面板背景色    */
#define UI_PRODUCT_PANEL_EDGE_COLOR UI_RGB565(48U, 78U, 96U)      /**< 面板边框色    */
#define UI_PRODUCT_ACCENT_COLOR     UI_RGB565(255U, 120U, 0U)     /**< 强调色（橙）  */
#define UI_PRODUCT_ACCENT_EDGE_COLOR UI_RGB565(255U, 166U, 64U)   /**< 强调边框色    */
#define UI_PRODUCT_CYAN_COLOR       UI_RGB565(90U, 220U, 230U)    /**< 科技青色      */
#define UI_PRODUCT_SUBTEXT_COLOR    UI_RGB565(170U, 176U, 184U)   /**< 次要文本色    */
#define UI_PRODUCT_DIM_COLOR        UI_RGB565(105U, 118U, 128U)   /**< 暗淡文本色    */
#define UI_PRODUCT_SUCCESS_COLOR    UI_RGB565(142U, 216U, 92U)    /**< 成功色（绿）  */
#define UI_PRODUCT_WARN_COLOR       UI_RGB565(255U, 198U, 90U)    /**< 警告色（黄）  */
#define UI_PRODUCT_ERROR_COLOR      UI_RGB565(232U, 92U, 92U)     /**< 错误色（红）  */

/* --- 行布局常量 --- */
#define UI_PRODUCT_ROW_LEFT         12U                         /**< 行左边界      */
#define UI_PRODUCT_ROW_RIGHT        (LCD_W - 12U)               /**< 行右边界      */
#define UI_PRODUCT_ROW_TEXT_LEFT    20U                         /**< 行文本左起    */
#define UI_PRODUCT_ROW_VALUE_RIGHT  (LCD_W - 20U)               /**< 行数值右对齐  */

/* --- 页面介绍栏布局 --- */
#define UI_PRODUCT_INTRO_BAR_LEFT   14U                         /**< 介绍栏左边界  */
#define UI_PRODUCT_INTRO_BAR_RIGHT  18U                         /**< 介绍栏右边界  */
#define UI_PRODUCT_INTRO_BAR_TOP    36U                         /**< 介绍栏上边界  */
#define UI_PRODUCT_INTRO_BAR_BOTTOM 68U                         /**< 介绍栏下边界  */
#define UI_PRODUCT_INTRO_TITLE_X    28U                         /**< 介绍标题 X    */
#define UI_PRODUCT_INTRO_TITLE_Y    34U                         /**< 介绍标题 Y    */
#define UI_PRODUCT_INTRO_SUBTITLE_X 30U                         /**< 介绍副标题 X  */
#define UI_PRODUCT_INTRO_SUBTITLE_Y 56U                         /**< 介绍副标题 Y  */

/* =========================================================================
 *  3. 前向声明
 * ======================================================================= */

static uint16_t ui_renderer_utf8_text_pixel_width(const char *text, uint16_t font_size);
static uint16_t ui_renderer_draw_header_status_right(uint16_t header_color);

/* =========================================================================
 *  4. 内部函数实现 —— 主题颜色映射
 * ======================================================================= */

/**
 * @brief  将标准颜色映射为产品主题色
 * @param  color — 原始颜色值（GREEN/YELLOW/RED/LGRAY）
 * @return 对应的主题色
 */
static uint16_t ui_renderer_theme_value_color(uint16_t color)
{
    if (color == GREEN)
    {
        return UI_PRODUCT_SUCCESS_COLOR;
    }
    if (color == YELLOW)
    {
        return UI_PRODUCT_WARN_COLOR;
    }
    if (color == RED)
    {
        return UI_PRODUCT_ERROR_COLOR;
    }
    if (color == LGRAY)
    {
        return UI_PRODUCT_SUBTEXT_COLOR;
    }

    return WHITE;
}

/* =========================================================================
 *  5. 内部函数实现 —— 文本绘制辅助
 * ======================================================================= */

/**
 * @brief  右对齐绘制文本
 * @param  right_x — 右边界 X 坐标
 * @param  y       — Y 坐标
 * @param  text    — 文本字符串（支持本地化）
 * @param  fc      — 前景色
 * @param  bc      — 背景色
 */
static void ui_renderer_draw_text_right(uint16_t right_x,
                                        uint16_t y,
                                        const char *text,
                                        uint16_t fc,
                                        uint16_t bc)
{
    const char *display_text = ui_renderer_localize(text);
    uint16_t width = 0U;
    uint16_t x = right_x;

    if (display_text == 0 || display_text[0] == '\0')
    {
        return;
    }

    width = ui_renderer_utf8_text_pixel_width(display_text, UI_UTF8_FONT_SIZE);
    if (width < right_x)
    {
        x = (uint16_t)(right_x - width);
    }

    LCD_ShowUTF8String(x, y, display_text, fc, bc, UI_UTF8_FONT_SIZE, 0);
}

/* =========================================================================
 *  6. 内部函数实现 —— 装饰元素
 * ======================================================================= */

/**
 * @brief  绘制右箭头（chevron）
 * @param  x     — 左上角 X
 * @param  y     — 左上角 Y
 * @param  color — 箭头颜色
 */
static void ui_renderer_draw_product_chevron(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_DrawLine(x, y, (uint16_t)(x + 6U), (uint16_t)(y + 6U), color);
    LCD_DrawLine((uint16_t)(x + 6U), (uint16_t)(y + 6U), x, (uint16_t)(y + 12U), color);
}

/**
 * @brief  绘制产品背景网格点阵
 * @param  top    — 网格区域上边界
 * @param  bottom — 网格区域下边界
 */
static void ui_renderer_draw_product_grid(uint16_t top, uint16_t bottom)
{
    uint16_t x = 0U;
    uint16_t y = 0U;

    for (y = top; y <= bottom; y = (uint16_t)(y + 8U))
    {
        for (x = 0U; x < LCD_W; x = (uint16_t)(x + 8U))
        {
            LCD_DrawPoint(x, y, UI_PRODUCT_GRID_COLOR);
        }
    }
}

/**
 * @brief  绘制装饰性电路线条
 */
static void ui_renderer_draw_product_circuit_lines(void)
{
    uint16_t c = UI_PRODUCT_GRID_COLOR;

    /* 上方电路路径 */
    LCD_DrawLine(188U, 40U, 228U, 40U, c);
    LCD_DrawLine(228U, 40U, 240U, 30U, c);
    LCD_DrawLine(240U, 30U, 308U, 30U, c);

    /* 下方电路路径 */
    LCD_DrawLine(172U, 56U, 220U, 56U, c);
    LCD_DrawLine(220U, 56U, 236U, 46U, c);
    LCD_DrawLine(236U, 46U, 304U, 46U, c);

    /* 节点焊点 */
    LCD_Fill(226U, 38U, 229U, 41U, c);
    LCD_Fill(238U, 28U, 241U, 31U, c);
}

/**
 * @brief  绘制品牌文本（页眉左侧）
 */
static void ui_renderer_draw_brand_text(void)
{
    /* "热成像" — 强调色 */
    LCD_ShowUTF8String(8U,
                       UI_HEADER_TITLE_Y,
                       "\xE7\x83\xAD\xE6\x88\x90\xE5\x83\x8F",
                       UI_PRODUCT_ACCENT_COLOR,
                       UI_PRODUCT_HEADER_COLOR,
                       UI_UTF8_FONT_SIZE,
                       0);
    /* "终端系统" — 白色 */
    LCD_ShowUTF8String(56U,
                       UI_HEADER_TITLE_Y,
                       "\xE7\xBB\x88\xE7\xAB\xAF\xE7\xB3\xBB\xE7\xBB\x9F",
                       WHITE,
                       UI_PRODUCT_HEADER_COLOR,
                       UI_UTF8_FONT_SIZE,
                       0);
}

/* =========================================================================
 *  7. 内部数据 —— 图标位图
 * ======================================================================= */

/** WiFi 图标位图（16×12 像素，单色） */
static const u8 s_ui_wifi_icon_bits[] =
{
    0x03, 0xC0,
    0x0C, 0x30,
    0x10, 0x08,
    0x03, 0xC0,
    0x04, 0x20,
    0x08, 0x10,
    0x01, 0x80,
    0x02, 0x40,
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00
};

/** 蓝牙图标位图（10×14 像素，单色） */
static const u8 s_ui_bt_icon_bits[] =
{
    0x18, 0x00,
    0x1C, 0x00,
    0x1A, 0x00,
    0x19, 0x00,
    0x1A, 0x00,
    0x1C, 0x00,
    0x18, 0x00,
    0x1C, 0x00,
    0x1A, 0x00,
    0x19, 0x00,
    0x1A, 0x00,
    0x1C, 0x00,
    0x18, 0x00,
    0x00, 0x00
};

/* =========================================================================
 *  8. 内部函数实现 —— 图标绘制
 * ======================================================================= */

/**
 * @brief  绘制蓝牙连接状态徽章（小圆点）
 * @param  x     — 图标左上角 X
 * @param  y     — 图标左上角 Y
 * @param  color — 徽章颜色
 */
static void ui_renderer_draw_bluetooth_state_badge(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_Fill((uint16_t)(x + 7U),
             (uint16_t)(y + 9U),
             (uint16_t)(x + 9U),
             (uint16_t)(y + 11U),
             color);
}

/**
 * @brief  绘制产品状态栏（页眉背景 + 品牌文本 + 状态图标）
 */
static void ui_renderer_draw_product_status_bar(void)
{
    LCD_Fill(0U, 0U, LCD_W - 1U, UI_HEADER_HEIGHT - 1U, UI_PRODUCT_HEADER_COLOR);
    ui_renderer_draw_brand_text();
    (void)ui_renderer_draw_header_status_right(UI_PRODUCT_HEADER_COLOR);
    LCD_DrawLine(0U, UI_HEADER_HEIGHT, LCD_W - 1U, UI_HEADER_HEIGHT, UI_PRODUCT_PANEL_EDGE_COLOR);
}

/* =========================================================================
 *  9. 内部函数实现 —— UTF-8 文本工具
 * ======================================================================= */

/**
 * @brief  计算 UTF-8 文本的像素宽度
 * @note   ASCII 字符占 font_size/2 宽度，CJK 字符占 font_size 宽度。
 * @param  text      — UTF-8 文本
 * @param  font_size — 字体尺寸
 * @return 像素宽度
 */
static uint16_t ui_renderer_utf8_text_pixel_width(const char *text, uint16_t font_size)
{
    const unsigned char *cursor = (const unsigned char *)text;
    uint16_t width = 0U;

    if (text == 0)
    {
        return 0U;
    }

    while (*cursor != '\0')
    {
        /* ASCII 字符（单字节） */
        if (*cursor < 0x80U)
        {
            width = (uint16_t)(width + (font_size / 2U));
            ++cursor;
            continue;
        }

        /* 多字节 UTF-8 字符（CJK 等） */
        width = (uint16_t)(width + font_size);
        if ((*cursor & 0xE0U) == 0xC0U && cursor[1] != '\0')
        {
            cursor += 2;    /* 2 字节序列 */
        }
        else if ((*cursor & 0xF0U) == 0xE0U &&
                 cursor[1] != '\0' &&
                 cursor[2] != '\0')
        {
            cursor += 3;    /* 3 字节序列 */
        }
        else if ((*cursor & 0xF8U) == 0xF0U &&
                 cursor[1] != '\0' &&
                 cursor[2] != '\0' &&
                 cursor[3] != '\0')
        {
            cursor += 4;    /* 4 字节序列 */
        }
        else
        {
            ++cursor;       /* 无效序列，跳过 */
        }
    }

    return width;
}

/**
 * @brief  获取 UTF-8 首字节对应的字符长度
 * @param  lead_byte — UTF-8 首字节
 * @return 字符字节长度（1~4）
 */
static uint8_t ui_renderer_utf8_char_len(unsigned char lead_byte)
{
    if (lead_byte < 0x80U)
    {
        return 1U;
    }
    if ((lead_byte & 0xE0U) == 0xC0U)
    {
        return 2U;
    }
    if ((lead_byte & 0xF0U) == 0xE0U)
    {
        return 3U;
    }
    if ((lead_byte & 0xF8U) == 0xF0U)
    {
        return 4U;
    }

    return 1U;
}

/**
 * @brief  将 UTF-8 文本裁剪到指定像素宽度内
 * @note   逐字符累加宽度，超出时截断并添加 '\0'。
 * @param  text       — 输入 UTF-8 文本
 * @param  max_width  — 最大像素宽度
 * @param  buffer     — 输出缓冲区
 * @param  buffer_len — 缓冲区长度
 */
static void ui_renderer_fit_utf8_text(const char *text,
                                      uint16_t max_width,
                                      char *buffer,
                                      uint16_t buffer_len)
{
    const unsigned char *cursor = (const unsigned char *)text;
    uint16_t width = 0U;
    uint16_t write_index = 0U;

    if (buffer == 0 || buffer_len == 0U)
    {
        return;
    }

    buffer[0] = '\0';
    if (text == 0)
    {
        return;
    }

    while (*cursor != '\0')
    {
        uint8_t char_len = ui_renderer_utf8_char_len(*cursor);
        uint16_t char_width = (*cursor < 0x80U) ? (UI_UTF8_FONT_SIZE / 2U) : UI_UTF8_FONT_SIZE;
        uint8_t i = 0U;

        /* 检查宽度或缓冲区是否溢出 */
        if ((uint16_t)(width + char_width) > max_width ||
            (uint16_t)(write_index + char_len + 1U) >= buffer_len)
        {
            break;
        }

        /* 复制字符字节 */
        for (i = 0U; i < char_len; ++i)
        {
            if (cursor[i] == '\0')
            {
                buffer[write_index] = '\0';
                return;
            }
            buffer[write_index++] = (char)cursor[i];
        }
        width = (uint16_t)(width + char_width);
        cursor += char_len;
    }

    buffer[write_index] = '\0';
}

/* =========================================================================
 *  10. 内部函数实现 —— 电池图标
 * ======================================================================= */

/**
 * @brief  根据电量百分比返回填充颜色
 * @param  percent — 电量百分比（0~100）
 * @return 颜色值（红/黄/绿）
 */
static uint16_t ui_renderer_battery_fill_color(uint8_t percent)
{
    if (percent < 30U)
    {
        return RED;
    }
    if (percent < 60U)
    {
        return YELLOW;
    }

    return GREEN;
}

/**
 * @brief  绘制电池图标
 * @param  x         — 左上角 X
 * @param  y         — 左上角 Y
 * @param  percent   — 电量百分比（0~100）
 * @param  back_color — 背景色（用于清空内部区域）
 */
static void ui_renderer_draw_battery_icon(uint16_t x, uint16_t y, uint8_t percent, uint16_t back_color)
{
    uint16_t body_left   = x;
    uint16_t body_top    = y;
    uint16_t body_right  = (uint16_t)(x + UI_HEADER_BATTERY_WIDTH - 1U);
    uint16_t body_bottom = (uint16_t)(y + UI_HEADER_BATTERY_HEIGHT - 1U);

    /* 正极凸起 */
    uint16_t tip_left   = (uint16_t)(body_right + 1U);
    uint16_t tip_top    = (uint16_t)(y + ((UI_HEADER_BATTERY_HEIGHT - UI_HEADER_BATTERY_TIP_HEIGHT) / 2U));
    uint16_t tip_bottom = (uint16_t)(tip_top + UI_HEADER_BATTERY_TIP_HEIGHT - 1U);

    /* 内部填充区域（留 2px 边距） */
    uint16_t inner_left   = (uint16_t)(body_left + 2U);
    uint16_t inner_top    = (uint16_t)(body_top + 2U);
    uint16_t inner_right  = (uint16_t)(body_right - 2U);
    uint16_t inner_bottom = (uint16_t)(body_bottom - 2U);
    uint16_t inner_width  = 0U;
    uint16_t fill_width   = 0U;
    uint16_t fill_color   = ui_renderer_battery_fill_color(percent);

    /* 绘制电池外壳和正极 */
    LCD_DrawRectangle(body_left, body_top, body_right, body_bottom, WHITE);
    LCD_DrawRectangle(tip_left,
                      tip_top,
                      (uint16_t)(tip_left + UI_HEADER_BATTERY_TIP_WIDTH - 1U),
                      tip_bottom,
                      WHITE);

    /* 清空内部区域 */
    LCD_Fill(inner_left, inner_top, inner_right, inner_bottom, back_color);

    /* 按百分比填充电量条 */
    inner_width = (uint16_t)(inner_right - inner_left + 1U);
    if (percent > 0U && inner_width > 0U)
    {
        fill_width = (uint16_t)(((uint32_t)inner_width * percent + 99UL) / 100UL);
        if (fill_width == 0U)
        {
            fill_width = 1U;
        }
        if (fill_width > inner_width)
        {
            fill_width = inner_width;
        }

        LCD_Fill(inner_left,
                 inner_top,
                 (uint16_t)(inner_left + fill_width - 1U),
                 inner_bottom,
                 fill_color);
    }
}

/* =========================================================================
 *  11. 内部函数实现 —— 状态图标绘制
 * ======================================================================= */

/**
 * @brief  绘制 WiFi 图标
 * @param  x     — 左上角 X
 * @param  y     — 左上角 Y
 * @param  color — 图标颜色
 */
static void ui_renderer_draw_wifi_icon(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_DrawMonoBitmap(x,
                       y,
                       s_ui_wifi_icon_bits,
                       UI_HEADER_WIFI_WIDTH,
                       UI_HEADER_WIFI_HEIGHT,
                       color,
                       UI_PRODUCT_HEADER_COLOR,
                       1U);
}

/**
 * @brief  绘制蓝牙图标
 * @param  x     — 左上角 X
 * @param  y     — 左上角 Y
 * @param  color — 图标颜色
 */
static void ui_renderer_draw_bluetooth_icon(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_DrawMonoBitmap(x,
                       y,
                       s_ui_bt_icon_bits,
                       UI_HEADER_BT_WIDTH,
                       UI_HEADER_BT_HEIGHT,
                       color,
                       UI_PRODUCT_HEADER_COLOR,
                       1U);
}

/**
 * @brief  绘制蓝牙状态图标（含连接徽章）
 * @param  x         — 左上角 X
 * @param  y         — 左上角 Y
 * @param  connected — 连接状态（1=已连接）
 */
static void ui_renderer_draw_bluetooth_status_icon(uint16_t x,
                                                   uint16_t y,
                                                   uint8_t connected)
{
    uint16_t icon_color = (connected != 0U) ? UI_PRODUCT_CYAN_COLOR : UI_PRODUCT_DIM_COLOR;

    ui_renderer_draw_bluetooth_icon(x, y, icon_color);
    if (connected != 0U)
    {
        ui_renderer_draw_bluetooth_state_badge(x, y, UI_PRODUCT_SUCCESS_COLOR);
    }
}

/* =========================================================================
 *  12. 内部函数实现 —— 页眉状态栏右侧图标
 * ======================================================================= */

/**
 * @brief  绘制页眉右侧状态图标（电池百分比 + 电池图标 + WiFi + 蓝牙）
 * @param  header_color — 页眉背景色
 * @return 最左侧图标的 X 坐标（供标题裁剪使用）
 */
static uint16_t ui_renderer_draw_header_status_right(uint16_t header_color)
{
    esp_host_status_t host_status;
    char percent_text[8];
    uint16_t right_x = (uint16_t)(LCD_W - UI_HEADER_STATUS_RIGHT_MARGIN);
    uint16_t percent_width = 0U;
    uint16_t percent_x = 0U;
    uint16_t battery_left = 0U;
    uint16_t icon_left = 0U;

    /* 获取 ESP32 主机状态 */
    esp_host_get_status_copy(&host_status);

    /* 绘制电池百分比文本 */
    snprintf(percent_text, sizeof(percent_text), "%u%%", battery_monitor_get_percent());
    percent_width = ui_renderer_utf8_text_pixel_width(percent_text, UI_UTF8_FONT_SIZE);
    percent_x = (uint16_t)(right_x - percent_width);
    LCD_ShowString(percent_x,
                   UI_HEADER_TITLE_Y,
                   (const u8 *)percent_text,
                   WHITE,
                   header_color,
                   UI_UTF8_FONT_SIZE,
                   0);

    /* 绘制电池图标 */
    battery_left = (uint16_t)(percent_x - UI_HEADER_STATUS_GAP -
                              UI_HEADER_BATTERY_WIDTH - UI_HEADER_BATTERY_TIP_WIDTH);
    ui_renderer_draw_battery_icon(battery_left, 9U, battery_monitor_get_percent(), header_color);
    icon_left = battery_left;

    /* WiFi 已连接时绘制图标 */
    if (host_status.wifi_connected != 0U)
    {
        uint16_t wifi_left = (uint16_t)(icon_left - UI_HEADER_STATUS_GAP - UI_HEADER_WIFI_WIDTH);
        ui_renderer_draw_wifi_icon(wifi_left, 8U, WHITE);
        icon_left = wifi_left;
    }

    /* 蓝牙已启用时绘制图标 */
    if (host_status.ble_enabled != 0U)
    {
        uint16_t bt_left = (uint16_t)(icon_left - UI_HEADER_STATUS_GAP - UI_HEADER_BT_WIDTH);
        ui_renderer_draw_bluetooth_status_icon(bt_left, 7U, host_status.ble_connected);
        icon_left = bt_left;
    }

    return icon_left;
}

/* =========================================================================
 *  13. 内部函数实现 —— 页眉路径构建
 * ======================================================================= */

/**
 * @brief  构建页眉路径文本（父页面/子页面）
 * @param  path_buffer     — 输出缓冲区
 * @param  path_buffer_len — 缓冲区长度
 * @param  parent_title    — 父页面标题
 * @param  child_title     — 子页面标题
 */
static void ui_renderer_build_header_path(char *path_buffer,
                                          uint16_t path_buffer_len,
                                          const char *parent_title,
                                          const char *child_title)
{
    char composed_buffer[96];
    const char *display_parent = ui_renderer_localize(parent_title);
    const char *display_child = ui_renderer_localize(child_title);

    if (path_buffer == 0 || path_buffer_len == 0U)
    {
        return;
    }

    path_buffer[0] = '\0';

    /* 拼接父页面标题 */
    if (display_parent != 0 && display_parent[0] != '\0')
    {
        snprintf(path_buffer, path_buffer_len, "%s", display_parent);
    }

    /* 拼接子页面标题（用 "/" 分隔） */
    if (display_child != 0 && display_child[0] != '\0')
    {
        if (path_buffer[0] != '\0')
        {
            snprintf(composed_buffer, sizeof(composed_buffer), "%s/%s", path_buffer, display_child);
            snprintf(path_buffer, path_buffer_len, "%s", composed_buffer);
        }
        else
        {
            snprintf(path_buffer, path_buffer_len, "%s", display_child);
        }
    }
}

/* =========================================================================
 *  14. 内部函数实现 —— 页眉绘制核心
 * ======================================================================= */

/**
 * @brief  页眉绘制核心实现
 * @param  title            — 标题文本（支持本地化）
 * @param  header_color     — 页眉背景色
 * @param  show_status_icons — 是否显示状态图标
 */
static void ui_renderer_draw_header_core(const char *title,
                                         uint16_t header_color,
                                         uint8_t show_status_icons)
{
    char fitted_title[96];
    const char *display_title = ui_renderer_localize(title);
    uint16_t title_right_limit = (uint16_t)(LCD_W - UI_HEADER_STATUS_RIGHT_MARGIN);

    app_display_runtime_lock();

    /* 填充页眉背景 */
    LCD_Fill(0, 0, LCD_W - 1U, UI_HEADER_HEIGHT - 1U, header_color);

    /* 绘制状态图标并获取标题右边界 */
    if (show_status_icons != 0U)
    {
        title_right_limit = ui_renderer_draw_header_status_right(header_color);
    }

    /* 绘制标题文本（自适应裁剪） */
    if (display_title != 0 && display_title[0] != '\0' &&
        title_right_limit > (UI_HEADER_TITLE_X + 4U))
    {
        ui_renderer_fit_utf8_text(display_title,
                                  (uint16_t)(title_right_limit - UI_HEADER_TITLE_X - 4U),
                                  fitted_title,
                                  sizeof(fitted_title));
        LCD_ShowUTF8String(UI_HEADER_TITLE_X,
                           UI_HEADER_TITLE_Y,
                           fitted_title,
                           WHITE,
                           header_color,
                           UI_UTF8_FONT_SIZE,
                           0);
    }

    app_display_runtime_unlock();
}

/* =========================================================================
 *  15. 公共接口实现 —— 本地化映射
 * ======================================================================= */

/**
 * @brief  将英文文本映射为中文 UTF-8 字节序列
 * @note   通过 strcmp 逐一匹配，返回对应的 UTF-8 编码字符串。
 *         未匹配时返回原始文本。
 * @param  text — 英文文本
 * @return 中文 UTF-8 字节序列指针，或原始文本
 */
const char *ui_renderer_localize(const char *text)
{
    if (text == 0)
    {
        return 0;
    }

    /* --- 主菜单项 --- */
    if (strcmp(text, "Main Menu") == 0) return "\xE4\xB8\xBB\xE8\x8F\x9C\xE5\x8D\x95";
    if (strcmp(text, "Thermal") == 0) return "\xE7\x83\xAD\xE6\x88\x90\xE5\x83\x8F";
    if (strcmp(text, "Update") == 0) return "\xE7\xB3\xBB\xE7\xBB\x9F\xE6\x9B\xB4\xE6\x96\xB0";
    if (strcmp(text, "WiFi") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE7\xBD\x91\xE7\xBB\x9C";
    if (strcmp(text, "Wireless") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "WiFi(KEY6)") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE7\xBD\x91\xE7\xBB\x9C\x20\xE9\x94\xAE\x36";
    if (strcmp(text, "WiFi...") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE7\xBD\x91\xE7\xBB\x9C\x2E\x2E\x2E";
    if (strcmp(text, "Bluetooth") == 0) return "\xE8\x93\x9D\xE7\x89\x99\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "Bluetooth...") == 0) return "\xE8\x93\x9D\xE7\x89\x99\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD";
    if (strcmp(text, "Server") == 0) return "\xE4\xBA\x91\xE7\xAB\xAF\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "Server...") == 0) return "\xE4\xBA\x91\xE7\xAB\xAF\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD";
    if (strcmp(text, "Power") == 0) return "\xE7\x94\xB5\xE6\xBA\x90\xE7\xAE\xA1\xE7\x90\x86";
    if (strcmp(text, "System") == 0) return "\xE7\xB3\xBB\xE7\xBB\x9F\xE8\xAE\xBE\xE7\xBD\xAE";

    /* --- 功能按钮 --- */
    if (strcmp(text, "Check Now") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\xE6\x9B\xB4\xE6\x96\xB0";
    if (strcmp(text, "Start Update") == 0) return "\xE5\xBC\x80\xE5\xA7\x8B\xE5\x8D\x87\xE7\xBA\xA7";
    if (strcmp(text, "Restore Last") == 0) return "\xE6\x81\xA2\xE5\xA4\x8D\xE4\xB8\x8A\xE7\x89\x88";
    if (strcmp(text, "Restore last") == 0) return "\xE6\x81\xA2\xE5\xA4\x8D\xE4\xB8\x8A\xE7\x89\x88";
    if (strcmp(text, "Restore Previous Version") == 0) return "\xE6\x81\xA2\xE5\xA4\x8D\xE4\xB8\x8A\xE4\xB8\x80\xE4\xB8\xAA\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Version Info") == 0) return "\xE7\x89\x88\xE6\x9C\xAC\xE4\xBF\xA1\xE6\x81\xAF";
    if (strcmp(text, "Pause Send Temp") == 0) return "\xE7\x83\xAD\xE6\x88\x90\xE5\x83\x8F\xE6\xB8\xA9\xE5\xBA\xA6\xE4\xB8\x8A\xE6\x8A\xA5";
    if (strcmp(text, "KEY2 Snapshot") == 0) return "\x4B\x45\x59\x32\xE6\x88\xAA\xE5\x9B\xBE";
    if (strcmp(text, "Debug Mode") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\xE6\xA8\xA1\xE5\xBC\x8F";
    if (strcmp(text, "Debug Tools") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\xE5\xB7\xA5\xE5\x85\xB7";
    if (strcmp(text, "Debug Page") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\xE9\xA1\xB5\xE9\x9D\xA2";
    if (strcmp(text, "SD Card") == 0) return "\x53\x44\xE5\x8D\xA1\xE8\xAE\xBE\xE7\xBD\xAE";
    if (strcmp(text, "Check SD Card") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\x53\x44\xE5\x8D\xA1";
    if (strcmp(text, "Mount / Info") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\x53\x44\xE5\x8D\xA1";
    if (strcmp(text, "Write Test") == 0) return "\xE5\x86\x99\xE5\x85\xA5\xE6\xB5\x8B\xE8\xAF\x95";
    if (strcmp(text, "Read Test") == 0) return "\xE8\xAF\xBB\xE5\x8F\x96\xE6\xB5\x8B\xE8\xAF\x95";
    if (strcmp(text, "View Latest") == 0) return "\xE6\x9F\xA5\xE7\x9C\x8B\xE6\x88\xAA\xE5\x9B\xBE";
    if (strcmp(text, "Clear Shots") == 0) return "\xE6\xB8\x85\xE7\xA9\xBA\xE6\x88\xAA\xE5\x9B\xBE";
    if (strcmp(text, "Perf Baseline") == 0) return "\xE6\x80\xA7\xE8\x83\xBD\xE5\x9F\xBA\xE7\xBA\xBF";
    if (strcmp(text, "Debug Screen") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\xE5\xB1\x8F\xE5\xB9\x95";
    if (strcmp(text, "Remote Keys") == 0) return "\xE9\x81\xA5\xE6\x8E\xA7\xE6\x8C\x89\xE9\x94\xAE";

    /* --- 电源管理菜单 --- */
    if (strcmp(text, "Power Mode") == 0) return "\xE7\x94\xB5\xE6\xBA\x90\xE6\xA8\xA1\xE5\xBC\x8F";
    if (strcmp(text, "Clock Policy") == 0) return "\xE6\x97\xB6\xE9\x92\x9F\xE7\xAD\x96\xE7\x95\xA5";
    if (strcmp(text, "Screen Off") == 0) return "\xE7\x86\x84\xE5\xB1\x8F\xE6\x97\xB6\xE9\x97\xB4";
    if (strcmp(text, "Stop Wake") == 0) return "\x53\x54\x4F\x50\xE5\x94\xA4\xE9\x86\x92";
    if (strcmp(text, "Standby") == 0) return "\xE8\x87\xAA\xE5\x8A\xA8\xE5\xBE\x85\xE6\x9C\xBA";
    if (strcmp(text, "ESP Save") == 0) return "\x45\x53\x50\xE4\xBC\x91\xE7\x9C\xA0";
    if (strcmp(text, "Standby Test") == 0) return "\xE5\xBE\x85\xE6\x9C\xBA\xE6\xB5\x8B\xE8\xAF\x95";

    /* --- 状态标签 --- */
    if (strcmp(text, "Current") == 0) return "\xE5\xBD\x93\xE5\x89\x8D";
    if (strcmp(text, "Target") == 0) return "\xE7\x9B\xAE\xE6\xA0\x87";
    if (strcmp(text, "Mode") == 0) return "\xE6\xA8\xA1\xE5\xBC\x8F";
    if (strcmp(text, "Battery") == 0) return "\xE7\x94\xB5\xE6\xB1\xA0";
    if (strcmp(text, "Battery Level") == 0) return "\xE7\x94\xB5\xE9\x87\x8F";
    if (strcmp(text, "Connection") == 0) return "\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "WiFi Status") == 0) return "\x57\x49\x46\x49\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE5\x86\xB5";
    if (strcmp(text, "SD Card Status") == 0) return "\x53\x44\xE5\x8D\xA1\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "Mount Status") == 0) return "\xE6\x8C\x82\xE8\xBD\xBD\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "Capacity") == 0) return "\xE5\xAE\xB9\xE9\x87\x8F";
    if (strcmp(text, "Free Space") == 0) return "\xE5\x89\xA9\xE4\xBD\x99\xE7\xA9\xBA\xE9\x97\xB4";
    if (strcmp(text, "Shot Count") == 0) return "\xE6\x88\xAA\xE5\x9B\xBE\xE6\x95\xB0\xE9\x87\x8F";
    if (strcmp(text, "Version") == 0) return "\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Slot") == 0) return "\xE5\x88\x86\xE5\x8C\xBA";
    if (strcmp(text, "Current Partition") == 0) return "\xE5\xBD\x93\xE5\x89\x8D\xE5\x88\x86\xE5\x8C\xBA";
    if (strcmp(text, "Old Partition") == 0) return "\xE6\x97\xA7\xE7\x89\x88\xE5\x88\x86\xE5\x8C\xBA";
    if (strcmp(text, "Reset") == 0) return "\xE5\xA4\x8D\xE4\xBD\x8D";
    if (strcmp(text, "Current Version") == 0) return "\xE5\xBD\x93\xE5\x89\x8D\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Latest Version") == 0) return "\xE6\x9C\x80\xE6\x96\xB0\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Previous Version") == 0) return "\xE4\xB8\x8A\xE4\xB8\x80\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Need WiFi") == 0) return "\xE9\x9C\x80\xE8\xA6\x81\xE7\xBD\x91\xE7\xBB\x9C";
    if (strcmp(text, "Reason") == 0) return "\xE5\x8E\x9F\xE5\x9B\xA0";
    if (strcmp(text, "Restore") == 0) return "\xE6\x81\xA2\xE5\xA4\x8D";
    if (strcmp(text, "Info") == 0) return "\xE4\xBF\xA1\xE6\x81\xAF";
    if (strcmp(text, "Detail") == 0) return "\xE8\xAF\xA6\xE6\x83\x85";
    if (strcmp(text, "Newest") == 0) return "\xE6\x9C\x80\xE6\x96\xB0";

    /* --- 按键操作提示 --- */
    if (strcmp(text, "KEY1/KEY3 Move  KEY2 Enter") == 0) return "\xE9\x94\xAE\x31\x2F\x33\xE7\xA7\xBB\xE5\x8A\xA8\x20\xE9\x94\xAE\x32\xE8\xBF\x9B\xE5\x85\xA5";
    if (strcmp(text, "KEY1/KEY3 Move  KEY2 Select") == 0) return "\xE9\x94\xAE\x31\x2F\x33\xE7\xA7\xBB\xE5\x8A\xA8\x20\xE9\x94\xAE\x32\xE9\x80\x89\xE6\x8B\xA9";
    if (strcmp(text, "KEY2 Toggle  Hold Home") == 0) return "\xE9\x94\xAE\x32\xE5\x88\x87\xE6\x8D\xA2\x20\xE9\x95\xBF\xE6\x8C\x89\xE9\x94\xAE\x32\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "KEY2 Cycle  Hold Home") == 0) return "\xE9\x94\xAE\x32\xE5\x88\x87\xE6\x8D\xA2\x20\xE9\x95\xBF\xE6\x8C\x89\xE9\x94\xAE\x32\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "KEY1/KEY3 Move  KEY2 Change") == 0) return "\xE9\x94\xAE\x31\x2F\x33\xE7\xA7\xBB\xE5\x8A\xA8\x20\xE9\x94\xAE\x32\xE4\xBF\xAE\xE6\x94\xB9";
    if (strcmp(text, "KEY1 Back  KEY2 Confirm") == 0) return "\xE9\x94\xAE\x31\xE8\xBF\x94\xE5\x9B\x9E\x20\xE9\x94\xAE\x32\xE7\xA1\xAE\xE8\xAE\xA4";
    if (strcmp(text, "KEY1/KEY2 Back  Hold Home") == 0) return "\xE9\x94\xAE\x31\x2F\x32\xE8\xBF\x94\xE5\x9B\x9E\x20\xE9\x95\xBF\xE6\x8C\x89\xE9\x94\xAE\x32\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "KEY1 Back  KEY2 Enable") == 0) return "\xE9\x94\xAE\x31\xE8\xBF\x94\xE5\x9B\x9E\x20\xE9\x94\xAE\x32\xE5\xBC\x80\xE5\x90\xAF";
    if (strcmp(text, "KEY1/KEY3 Page  KEY2 Reset") == 0) return "\xE9\x94\xAE\x31\x2F\x33\xE7\xBF\xBB\xE9\xA1\xB5\x20\xE9\x94\xAE\x32\xE6\xB8\x85\xE9\x9B\xB6";

    /* --- 操作状态提示 --- */
    if (strcmp(text, "Checking") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Please wait") == 0) return "\xE8\xAF\xB7\xE7\xA8\x8D\xE5\x80\x99";
    if (strcmp(text, "Task busy") == 0) return "\xE4\xBB\xBB\xE5\x8A\xA1\xE5\xBF\x99";
    if (strcmp(text, "Restarting") == 0) return "\xE6\xAD\xA3\xE5\x9C\xA8\xE9\x87\x8D\xE5\x90\xAF";
    if (strcmp(text, "Start update") == 0) return "\xE5\xBC\x80\xE5\xA7\x8B\xE5\x8D\x87\xE7\xBA\xA7";
    if (strcmp(text, "New version") == 0) return "\xE5\x8F\x91\xE7\x8E\xB0\xE6\x96\xB0\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Up to date") == 0) return "\xE5\xB7\xB2\xE6\x98\xAF\xE6\x9C\x80\xE6\x96\xB0";
    if (strcmp(text, "WiFi not ready") == 0) return "\xE7\xBD\x91\xE7\xBB\x9C\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA";
    if (strcmp(text, "Try again") == 0) return "\xE8\xAF\xB7\xE9\x87\x8D\xE8\xAF\x95";
    if (strcmp(text, "Device busy") == 0) return "\xE8\xAE\xBE\xE5\xA4\x87\xE5\xBF\x99";
    if (strcmp(text, "Check failed") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\xE5\xA4\xB1\xE8\xB4\xA5";
    if (strcmp(text, "WiFi error") == 0) return "\xE7\xBD\x91\xE7\xBB\x9C\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "WiFi timeout") == 0) return "\xE7\xBD\x91\xE7\xBB\x9C\xE8\xB6\x85\xE6\x97\xB6";
    if (strcmp(text, "Check timeout") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\xE8\xB6\x85\xE6\x97\xB6";
    if (strcmp(text, "Enabling WiFi") == 0) return "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\xBC\x80\xE5\x90\xAF\xE7\xBD\x91\xE7\xBB\x9C";
    if (strcmp(text, "Enabling...") == 0) return "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\xBC\x80\xE5\x90\xAF";
    if (strcmp(text, "Version ready") == 0) return "\xE7\x89\x88\xE6\x9C\xAC\xE5\xB0\xB1\xE7\xBB\xAA";
    if (strcmp(text, "Up to date version") == 0) return "\xE5\xB7\xB2\xE6\x98\xAF\xE6\x9C\x80\xE6\x96\xB0\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Found new version") == 0) return "\xE5\x8F\x91\xE7\x8E\xB0\xE6\x96\xB0\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Turn on now?") == 0) return "\xE7\x8E\xB0\xE5\x9C\xA8\xE5\xBC\x80\xE5\x90\xAF";
    if (strcmp(text, "Required to update") == 0) return "\xE5\xBF\x85\xE9\xA1\xBB\xE5\x8D\x87\xE7\xBA\xA7";
    if (strcmp(text, "Required to check") == 0) return "\xE5\xBF\x85\xE9\xA1\xBB\xE6\xA3\x80\xE6\x9F\xA5";

    /* --- 设备状态文本 --- */
    if (strcmp(text, "PRESS KEY6") == 0) return "\xE6\x8C\x89\xE9\x94\xAE\x36\xE5\x94\xA4\xE9\x86\x92";
    if (strcmp(text, "KEY6") == 0) return "\xE9\x94\xAE\x36";
    if (strcmp(text, "WORKING") == 0) return "\xE5\xA4\x84\xE7\x90\x86\xE4\xB8\xAD";
    if (strcmp(text, "SETUP") == 0) return "\x57\x49\x46\x49\xE6\x9C\xAA\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "CONNECTED") == 0) return "\xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "CONNECTING") == 0) return "\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD";
    if (strcmp(text, "NOT CONNECTED") == 0) return "\xE6\x9C\xAA\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "OFFLINE") == 0) return "\xE6\x9C\xAA\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "PENDING") == 0) return "\xE5\xBE\x85\xE7\xA1\xAE\xE8\xAE\xA4";
    if (strcmp(text, "DISABLED") == 0) return "\xE5\xB7\xB2\xE7\xA6\x81\xE7\x94\xA8";
    if (strcmp(text, "READY") == 0) return "\xE5\xB7\xB2\xE5\xB0\xB1\xE7\xBB\xAA";
    if (strcmp(text, "NOT READY") == 0) return "\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA";
    if (strcmp(text, "MOUNTED") == 0) return "\xE5\xB7\xB2\xE6\x8C\x82\xE8\xBD\xBD";
    if (strcmp(text, "NOT MOUNTED") == 0) return "\xE6\x9C\xAA\xE6\x8C\x82\xE8\xBD\xBD";
    if (strcmp(text, "Enable build") == 0) return "\xE6\x89\x93\xE5\xBC\x80\xE7\xBC\x96\xE8\xAF\x91";
    if (strcmp(text, "Screen only") == 0) return "\xE4\xBB\x85\xE5\xB1\x8F\xE5\xB9\x95\xE9\xA1\xB5";
    if (strcmp(text, "Perf baseline off") == 0) return "\xE6\x80\xA7\xE8\x83\xBD\xE5\x85\xB3\xE9\x97\xAD";
    if (strcmp(text, "Hold KEY2 to Home") == 0) return "\xE9\x95\xBF\xE6\x8C\x89\xE9\x94\xAE\x32\xE5\x9B\x9E\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "Enter") == 0) return "\xE8\xBF\x9B\xE5\x85\xA5";
    if (strcmp(text, "Off") == 0) return "\xE5\x85\xB3\xE9\x97\xAD";
    if (strcmp(text, "WAIT") == 0) return "\xE7\xAD\x89\xE5\xBE\x85";
    if (strcmp(text, "ERR") == 0) return "\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "OK") == 0) return "\xE6\xAD\xA3\xE5\xB8\xB8";
    if (strcmp(text, "ON") == 0) return "\xE5\xBC\x80\xE5\x90\xAF";
    if (strcmp(text, "OFF") == 0) return "\xE5\x85\xB3\xE9\x97\xAD";
    if (strcmp(text, "Yes") == 0) return "\xE6\x98\xAF";
    if (strcmp(text, "No") == 0) return "\xE5\x90\xA6";

    /* --- 错误信息 --- */
    if (strcmp(text, "ESP32 busy") == 0) return "\x45\x53\x50\x33\x32\xE5\xBF\x99";
    if (strcmp(text, "No WiFi") == 0) return "\xE6\x97\xA0\x57\x69\x46\x69";
    if (strcmp(text, "Not ready") == 0) return "\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA";
    if (strcmp(text, "Init fail") == 0) return "\xE5\x88\x9D\xE5\xA7\x8B\xE5\x8C\x96\xE5\xA4\xB1\xE8\xB4\xA5";
    if (strcmp(text, "Mount fail") == 0) return "\xE6\x8C\x82\xE8\xBD\xBD\xE5\xA4\xB1\xE8\xB4\xA5";
    if (strcmp(text, "FS error") == 0) return "\xE6\x96\x87\xE4\xBB\xB6\xE7\xB3\xBB\xE7\xBB\x9F\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "IO error") == 0) return "\xE8\xAF\xBB\xE5\x86\x99\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "No snapshot") == 0) return "\xE6\xB2\xA1\xE6\x9C\x89\xE6\x88\xAA\xE5\x9B\xBE";
    if (strcmp(text, "Storage err") == 0) return "\xE5\xAD\x98\xE5\x82\xA8\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "Meta failed") == 0) return "\xE5\x85\x83\xE6\x95\xB0\xE6\x8D\xAE\xE5\xA4\xB1\xE8\xB4\xA5";
    if (strcmp(text, "No package") == 0) return "\xE6\x97\xA0\xE5\x8D\x87\xE7\xBA\xA7\xE5\x8C\x85";
    if (strcmp(text, "Product err") == 0) return "\xE4\xBA\xA7\xE5\x93\x81\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "HW rev err") == 0) return "\xE7\xA1\xAC\xE4\xBB\xB6\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "Protocol err") == 0) return "\xE5\x8D\x8F\xE8\xAE\xAE\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "Partition err") == 0) return "\xE5\x88\x86\xE5\x8C\xBA\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "Version err") == 0) return "\xE7\x89\x88\xE6\x9C\xAC\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "No update") == 0) return "\xE6\xB2\xA1\xE6\x9C\x89\xE6\x9B\xB4\xE6\x96\xB0";
    if (strcmp(text, "UART timeout") == 0) return "\xE4\xB8\xB2\xE5\x8F\xA3\xE8\xB6\x85\xE6\x97\xB6";

    /* --- 性能诊断标签 --- */
    if (strcmp(text, "Perf Snapshot") == 0) return "\xE6\x80\xA7\xE8\x83\xBD\xE5\xBF\xAB\xE7\x85\xA7";
    if (strcmp(text, "Perf Timing") == 0) return "\xE6\x80\xA7\xE8\x83\xBD\xE6\x97\xB6\xE5\xBA\x8F";
    if (strcmp(text, "Perf Counters") == 0) return "\xE6\x80\xA7\xE8\x83\xBD\xE8\xAE\xA1\xE6\x95\xB0";
    if (strcmp(text, "Perf Health") == 0) return "\xE6\x80\xA7\xE8\x83\xBD\xE5\x81\xA5\xE5\xBA\xB7";
    if (strcmp(text, "FPS") == 0) return "\xE7\x83\xAD\xE5\x9B\xBE\xE5\xB8\xA7\xE7\x8E\x87";
    if (strcmp(text, "MinT") == 0) return "\xE6\x9C\x80\xE4\xBD\x8E\xE6\xB8\xA9\xE5\xBA\xA6";
    if (strcmp(text, "MaxT") == 0) return "\xE6\x9C\x80\xE9\xAB\x98\xE6\xB8\xA9\xE5\xBA\xA6";
    if (strcmp(text, "CtrT") == 0) return "\xE4\xB8\xAD\xE5\xBF\x83\xE6\xB8\xA9\xE5\xBA\xA6";
    if (strcmp(text, "Frame L/A/M") == 0) return "\xE5\xB8\xA7\xE6\x97\xB6\x4C\x2F\x41\x2F\x4D";
    if (strcmp(text, "Temp  L/A/M") == 0) return "\xE9\x87\x87\xE9\x9B\x86\x4C\x2F\x41\x2F\x4D";
    if (strcmp(text, "Gray  L/A/M") == 0) return "\xE7\x81\xB0\xE5\xBA\xA6\x4C\x2F\x41\x2F\x4D";
    if (strcmp(text, "DMA   L/A/M") == 0) return "\xE6\x98\xBE\xE7\xA4\xBA\x4C\x2F\x41\x2F\x4D";
    if (strcmp(text, "KeyQ Drop") == 0) return "\xE6\x8C\x89\xE9\x94\xAE\xE4\xB8\xA2\xE5\x8C\x85";
    if (strcmp(text, "UIQ Drop") == 0) return "\xE7\x95\x8C\xE9\x9D\xA2\xE4\xB8\xA2\xE5\x8C\x85";
    if (strcmp(text, "SvcQ Fail") == 0) return "\xE6\x9C\x8D\xE5\x8A\xA1\xE5\xA4\xB1\xE8\xB4\xA5";
    if (strcmp(text, "UART Err") == 0) return "\xE4\xB8\xB2\xE5\x8F\xA3\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "Wdg Fault") == 0) return "\xE7\x9C\x8B\xE9\x97\xA8\xE7\x8B\x97\xE6\x95\x85";
    if (strcmp(text, "Miss Prog") == 0) return "\xE8\xBF\x9B\xE5\xBA\xA6\xE7\xBC\xBA\xE5\xA4\xB1";
    if (strcmp(text, "Therm Act") == 0) return "\xE7\x83\xAD\xE5\x83\x8F\xE8\xBF\x90\xE8\xA1\x8C";
    if (strcmp(text, "Screen") == 0) return "\xE5\xB1\x8F\xE5\xB9\x95\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "Status") == 0) return "\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "Switch") == 0) return "\xE5\xBC\x80\xE5\x85\xB3";
    if (strcmp(text, "Action") == 0) return "\xE6\x93\x8D\xE4\xBD\x9C";
    if (strcmp(text, "Scope") == 0) return "\xE8\x8C\x83\xE5\x9B\xB4";

    /* --- 页面描述文本 --- */
    if (strcmp(text, "Infrared Thermal") == 0) return "\xE5\x8A\x9F\xE8\x83\xBD\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "Function Home") == 0) return "\xE5\x8A\x9F\xE8\x83\xBD\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "Live Measure") == 0) return "\xE5\xAE\x9E\xE6\x97\xB6\xE6\xB5\x8B\xE6\xB8\xA9\xE7\x94\xBB\xE9\x9D\xA2";
    if (strcmp(text, "Check Version") == 0) return "\xE7\x89\x88\xE6\x9C\xAC\xE6\xA3\x80\xE6\x9F\xA5";
    if (strcmp(text, "Connection Status") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE7\x8A\xB6\xE6\x80\x81\x2F\xE4\xBA\x91\xE7\xAB\xAF\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "Power Profile") == 0) return "\xE6\x81\xAF\xE5\xB1\x8F\x20\x2F\x20\xE5\xBE\x85\xE6\x9C\xBA\x20\x2F\x20\xE4\xBC\x91\xE7\x9C\xA0";
    if (strcmp(text, "Select Feature") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\x2F\xE6\x98\xBE\xE7\xA4\xBA\x2F\xE5\x8F\x82\xE6\x95\xB0";
    if (strcmp(text, "Device Entry") == 0) return "\xE8\xAE\xBE\xE5\xA4\x87\xE5\x8A\x9F\xE8\x83\xBD\xE5\x85\xA5\xE5\x8F\xA3";
    if (strcmp(text, "Live Thermal View") == 0) return "\xE5\xAE\x9E\xE6\x97\xB6\xE6\xB5\x8B\xE6\xB8\xA9\xE7\x94\xBB\xE9\x9D\xA2";
    if (strcmp(text, "Check Version / Upgrade") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\xE7\x89\x88\xE6\x9C\xAC\x20\x2F\x20\xE5\x9B\xBA\xE4\xBB\xB6\xE5\x8D\x87\xE7\xBA\xA7";
    if (strcmp(text, "Wi-Fi / BLE / Cloud") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE7\xBD\x91\xE7\xBB\x9C\x20\x2F\x20\xE8\x93\x9D\xE7\x89\x99\x20\x2F\x20\xE4\xBA\x91\xE7\xAB\xAF";
    if (strcmp(text, "Mode / Screen / Save") == 0) return "\xE6\x81\xAF\xE5\xB1\x8F\x20\x2F\x20\xE5\xBE\x85\xE6\x9C\xBA\x20\x2F\x20\xE4\xBC\x91\xE7\x9C\xA0";
    if (strcmp(text, "Screen / Standby / Save") == 0) return "\xE6\x81\xAF\xE5\xB1\x8F\x20\x2F\x20\xE5\xBE\x85\xE6\x9C\xBA\x20\x2F\x20\xE4\xBC\x91\xE7\x9C\xA0";
    if (strcmp(text, "Debug / Display / Params") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\x20\x2F\x20\xE6\x98\xBE\xE7\xA4\xBA\x20\x2F\x20\xE5\x8F\x82\xE6\x95\xB0";
    if (strcmp(text, "Check Version / Firmware OTA") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\xE7\x89\x88\xE6\x9C\xAC\x20\x2F\x20\xE5\x9B\xBA\xE4\xBB\xB6\xE5\x8D\x87\xE7\xBA\xA7";
    if (strcmp(text, "Wi-Fi / Bluetooth / Server") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE7\xBD\x91\xE7\xBB\x9C\x20\x2F\x20\xE8\x93\x9D\xE7\x89\x99\x20\x2F\x20\xE4\xBA\x91\xE7\xAB\xAF\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "Check SD Card / View Latest") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\x53\x44\xE5\x8D\xA1\x20\x2F\x20\xE6\x9F\xA5\xE7\x9C\x8B\xE6\x88\xAA\xE5\x9B\xBE";
    if (strcmp(text, "Shot List") == 0) return "\xE6\x88\xAA\xE5\x9B\xBE\xE5\x88\x97\xE8\xA1\xA8";
    if (strcmp(text, "Debug / Advanced Tools") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\x20\x2F\x20\xE5\xB7\xA5\xE5\x85\xB7";
    if (strcmp(text, "WiFi Connection") == 0) return "\x57\x69\x46\x69\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "Bluetooth Connection") == 0) return "\xE8\x93\x9D\xE7\x89\x99\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "Cloud Connection") == 0) return "\xE4\xBA\x91\xE7\xAB\xAF\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE6\x80\x81";

    /* 未匹配时返回原始文本 */
    return text;
}

/* =========================================================================
 *  16. 公共接口实现 —— 简单文本绘制
 * ======================================================================= */

/**
 * @brief  在指定位置绘制文本（支持本地化）
 * @param  x  — X 坐标
 * @param  y  — Y 坐标
 * @param  text — 文本字符串
 * @param  fc — 前景色
 * @param  bc — 背景色
 */
static void ui_renderer_draw_text(uint16_t x,
                                  uint16_t y,
                                  const char *text,
                                  uint16_t fc,
                                  uint16_t bc)
{
    const char *display_text = ui_renderer_localize(text);

    if (display_text != 0 && display_text[0] != '\0')
    {
        LCD_ShowUTF8String(x, y, display_text, fc, bc, UI_UTF8_FONT_SIZE, 0);
    }
}

/* =========================================================================
 *  17. 公共接口实现 —— 页眉绘制变体
 * ======================================================================= */

/**
 * @brief  绘制简单页眉（无状态图标）
 * @param  title        — 标题文本
 * @param  header_color — 背景色
 */
void ui_renderer_draw_header(const char *title, uint16_t header_color)
{
    ui_renderer_draw_header_core(title, header_color, 0U);
}

/**
 * @brief  绘制带状态图标的页眉
 * @param  title        — 标题文本
 * @param  header_color — 背景色
 */
void ui_renderer_draw_header_status(const char *title, uint16_t header_color)
{
    ui_renderer_draw_header_core(title, header_color, 1U);
}

/**
 * @brief  绘制带提示文本的页眉
 * @param  title        — 标题文本
 * @param  hint         — 提示文本（当前未使用）
 * @param  header_color — 背景色
 */
void ui_renderer_draw_header_hint(const char *title, const char *hint, uint16_t header_color)
{
    (void)hint;
    ui_renderer_draw_header_core(title, header_color, 0U);
}

/**
 * @brief  绘制路径导航页眉（父页面/子页面）
 * @param  parent_title — 父页面标题
 * @param  child_title  — 子页面标题
 * @param  header_color — 背景色
 */
void ui_renderer_draw_header_path(const char *parent_title,
                                  const char *child_title,
                                  uint16_t header_color)
{
    char path_buffer[96];

    ui_renderer_build_header_path(path_buffer, sizeof(path_buffer), parent_title, child_title);
    ui_renderer_draw_header_core((path_buffer[0] != '\0') ? path_buffer : parent_title,
                                 header_color,
                                 0U);
}

/**
 * @brief  绘制带状态图标的路径导航页眉
 * @param  parent_title — 父页面标题
 * @param  child_title  — 子页面标题
 * @param  header_color — 背景色
 */
void ui_renderer_draw_header_path_status(const char *parent_title,
                                         const char *child_title,
                                         uint16_t header_color)
{
    char path_buffer[96];

    ui_renderer_build_header_path(path_buffer, sizeof(path_buffer), parent_title, child_title);
    ui_renderer_draw_header_core((path_buffer[0] != '\0') ? path_buffer : parent_title,
                                 header_color,
                                 1U);
}

/**
 * @brief  绘制带提示的路径导航页眉
 * @param  parent_title — 父页面标题
 * @param  child_title  — 子页面标题
 * @param  hint         — 提示文本（当前未使用）
 * @param  header_color — 背景色
 */
void ui_renderer_draw_header_path_hint(const char *parent_title,
                                       const char *child_title,
                                       const char *hint,
                                       uint16_t header_color)
{
    (void)hint;
    ui_renderer_draw_header_path(parent_title, child_title, header_color);
}

/* =========================================================================
 *  18. 公共接口实现 —— 产品背景与页面介绍
 * ======================================================================= */

/**
 * @brief  绘制产品主题背景（页眉 + 网格点阵 + 电路线条）
 */
void ui_renderer_draw_product_background(void)
{
    app_display_runtime_lock();
    LCD_Fill(0U, 0U, LCD_W - 1U, LCD_H - 1U, UI_PRODUCT_BG_COLOR);
    ui_renderer_draw_product_status_bar();
    ui_renderer_draw_product_grid((uint16_t)(UI_HEADER_HEIGHT + 4U), LCD_H - 1U);
    ui_renderer_draw_product_circuit_lines();
    app_display_runtime_unlock();
}

/**
 * @brief  绘制页面介绍栏（左侧色条 + 标题 + 副标题）
 * @param  title        — 标题文本
 * @param  subtitle     — 副标题文本
 * @param  accent_color — 强调色（色条颜色）
 */
void ui_renderer_draw_page_intro(const char *title,
                                 const char *subtitle,
                                 uint16_t accent_color)
{
    const char *display_title = ui_renderer_localize(title);
    const char *display_subtitle = ui_renderer_localize(subtitle);

    app_display_runtime_lock();

    /* 左侧强调色条 */
    LCD_Fill(UI_PRODUCT_INTRO_BAR_LEFT,
             UI_PRODUCT_INTRO_BAR_TOP,
             UI_PRODUCT_INTRO_BAR_RIGHT,
             UI_PRODUCT_INTRO_BAR_BOTTOM,
             accent_color);

    /* 标题文本 */
    if (display_title != 0 && display_title[0] != '\0')
    {
        LCD_ShowUTF8String(UI_PRODUCT_INTRO_TITLE_X,
                           UI_PRODUCT_INTRO_TITLE_Y,
                           display_title,
                           WHITE,
                           UI_PRODUCT_BG_COLOR,
                           UI_UTF8_FONT_SIZE,
                           0);
    }

    /* 副标题文本 */
    if (display_subtitle != 0 && display_subtitle[0] != '\0')
    {
        LCD_ShowUTF8String(UI_PRODUCT_INTRO_SUBTITLE_X,
                           UI_PRODUCT_INTRO_SUBTITLE_Y,
                           display_subtitle,
                           UI_PRODUCT_SUBTEXT_COLOR,
                           UI_PRODUCT_BG_COLOR,
                           UI_UTF8_FONT_SIZE,
                           0);
    }

    app_display_runtime_unlock();
}

/**
 * @brief  绘制完整产品页面（背景 + 介绍栏）
 * @param  title        — 标题文本
 * @param  subtitle     — 副标题文本
 * @param  accent_color — 强调色
 */
void ui_renderer_draw_product_page(const char *title,
                                   const char *subtitle,
                                   uint16_t accent_color)
{
    ui_renderer_draw_product_background();
    ui_renderer_draw_page_intro(title, subtitle, accent_color);
}

/* =========================================================================
 *  19. 公共接口实现 —— 页脚与区域清除
 * ======================================================================= */

/**
 * @brief  绘制页脚（白色背景 + 两行深蓝文本）
 * @param  line1 — 第一行文本
 * @param  line2 — 第二行文本
 */
void ui_renderer_draw_footer(const char *line1, const char *line2)
{
    app_display_runtime_lock();
    LCD_Fill(0, UI_FOOTER_LINE1_Y, LCD_W - 1U, LCD_H - 1U, WHITE);

    ui_renderer_draw_text(8, UI_FOOTER_LINE1_Y, line1, DARKBLUE, WHITE);
    ui_renderer_draw_text(8, UI_FOOTER_LINE2_Y, line2, DARKBLUE, WHITE);
    app_display_runtime_unlock();
}

/**
 * @brief  清除页面主体区域
 * @param  color — 填充颜色
 */
void ui_renderer_clear_body(uint16_t color)
{
    app_display_runtime_lock();
    LCD_Fill(0, UI_HEADER_HEIGHT, LCD_W - 1U, LCD_H - 1U, color);
    app_display_runtime_unlock();
}

/**
 * @brief  清除指定行区域
 * @param  y     — 行 Y 坐标
 * @param  color — 填充颜色
 */
void ui_renderer_clear_row(uint16_t y, uint16_t color)
{
    app_display_runtime_lock();
    LCD_Fill(8, y, LCD_W - 8U, (uint16_t)(y + UI_ROW_HEIGHT - 2U), color);
    app_display_runtime_unlock();
}

/* =========================================================================
 *  20. 公共接口实现 —— 列表组件绘制
 * ======================================================================= */

/**
 * @brief  绘制数值行（标签 + 右对齐数值）
 * @param  y           — 行 Y 坐标
 * @param  label       — 标签文本
 * @param  value       — 数值文本
 * @param  value_color — 数值颜色
 * @param  back_color  — 背景色（未使用，保留接口兼容）
 */
void ui_renderer_draw_value_row(uint16_t y,
                                const char *label,
                                const char *value,
                                uint16_t value_color,
                                uint16_t back_color)
{
    uint16_t row_top = y;
    uint16_t row_bottom = (uint16_t)(y + UI_ROW_HEIGHT - 2U);
    (void)back_color;

    app_display_runtime_lock();

    /* 面板背景与边框 */
    LCD_Fill(UI_PRODUCT_ROW_LEFT, row_top, UI_PRODUCT_ROW_RIGHT, row_bottom, UI_PRODUCT_PANEL_COLOR);
    LCD_DrawRectangle(UI_PRODUCT_ROW_LEFT, row_top, UI_PRODUCT_ROW_RIGHT, row_bottom, UI_PRODUCT_PANEL_EDGE_COLOR);

    /* 标签（左对齐） */
    ui_renderer_draw_text(UI_PRODUCT_ROW_TEXT_LEFT, (uint16_t)(y + 4U), label, WHITE, UI_PRODUCT_PANEL_COLOR);

    /* 数值（右对齐，主题色映射） */
    ui_renderer_draw_text_right(UI_PRODUCT_ROW_VALUE_RIGHT,
                                (uint16_t)(y + 4U),
                                value,
                                ui_renderer_theme_value_color(value_color),
                                UI_PRODUCT_PANEL_COLOR);

    app_display_runtime_unlock();
}

/**
 * @brief  绘制列表项（标签 + 右箭头）
 * @param  y          — 行 Y 坐标
 * @param  label      — 标签文本
 * @param  selected   — 是否选中（选中时使用强调色背景）
 * @param  accent     — 强调标志（未使用）
 * @param  back_color — 背景色（未使用）
 */
void ui_renderer_draw_list_item(uint16_t y,
                                const char *label,
                                uint8_t selected,
                                uint8_t accent,
                                uint16_t back_color)
{
    uint16_t row_top = y;
    uint16_t row_bottom = (uint16_t)(y + UI_ROW_HEIGHT - 2U);
    uint16_t row_color = UI_PRODUCT_PANEL_COLOR;
    uint16_t edge_color = UI_PRODUCT_PANEL_EDGE_COLOR;
    uint16_t text_color = WHITE;
    uint16_t arrow_color = UI_PRODUCT_SUBTEXT_COLOR;
    (void)accent;
    (void)back_color;

    /* 选中状态使用强调色 */
    if (selected != 0U)
    {
        row_color = UI_PRODUCT_ACCENT_COLOR;
        edge_color = UI_PRODUCT_ACCENT_EDGE_COLOR;
        text_color = WHITE;
        arrow_color = WHITE;
    }

    app_display_runtime_lock();

    /* 面板背景与边框 */
    LCD_Fill(UI_PRODUCT_ROW_LEFT, row_top, UI_PRODUCT_ROW_RIGHT, row_bottom, row_color);
    LCD_DrawRectangle(UI_PRODUCT_ROW_LEFT, row_top, UI_PRODUCT_ROW_RIGHT, row_bottom, edge_color);

    /* 标签文本 */
    ui_renderer_draw_text((uint16_t)(UI_ITEM_LEFT_X + 8U), (uint16_t)(y + 4U), label, text_color, row_color);

    /* 右箭头 */
    ui_renderer_draw_product_chevron((uint16_t)(UI_PRODUCT_ROW_RIGHT - 18U),
                                     (uint16_t)(y + 5U),
                                     arrow_color);

    app_display_runtime_unlock();
}

/**
 * @brief  绘制开关项（标签 + ON/OFF 状态文本）
 * @param  y          — 行 Y 坐标
 * @param  label      — 标签文本
 * @param  enabled    — 开关状态（1=开启）
 * @param  selected   — 是否选中
 * @param  back_color — 背景色（未使用）
 */
void ui_renderer_draw_toggle_item(uint16_t y,
                                  const char *label,
                                  uint8_t enabled,
                                  uint8_t selected,
                                  uint16_t back_color)
{
    const char *value_text = (enabled != 0U) ? "ON" : "OFF";
    uint16_t row_top = y;
    uint16_t row_bottom = (uint16_t)(y + UI_ROW_HEIGHT - 2U);
    uint16_t row_color = UI_PRODUCT_PANEL_COLOR;
    uint16_t edge_color = UI_PRODUCT_PANEL_EDGE_COLOR;
    uint16_t text_color = WHITE;
    uint16_t value_color = (enabled != 0U) ? UI_PRODUCT_SUCCESS_COLOR : UI_PRODUCT_DIM_COLOR;
    (void)back_color;

    /* 选中状态使用强调色 */
    if (selected != 0U)
    {
        row_color = UI_PRODUCT_ACCENT_COLOR;
        edge_color = UI_PRODUCT_ACCENT_EDGE_COLOR;
        text_color = WHITE;
        value_color = WHITE;
    }

    app_display_runtime_lock();

    /* 面板背景与边框 */
    LCD_Fill(UI_PRODUCT_ROW_LEFT, row_top, UI_PRODUCT_ROW_RIGHT, row_bottom, row_color);
    LCD_DrawRectangle(UI_PRODUCT_ROW_LEFT, row_top, UI_PRODUCT_ROW_RIGHT, row_bottom, edge_color);

    /* 标签文本 */
    ui_renderer_draw_text((uint16_t)(UI_ITEM_LEFT_X + 8U), (uint16_t)(y + 4U), label, text_color, row_color);

    /* 状态文本（右对齐） */
    ui_renderer_draw_text_right(UI_PRODUCT_ROW_VALUE_RIGHT,
                                (uint16_t)(y + 4U),
                                value_text,
                                value_color,
                                row_color);

    app_display_runtime_unlock();
}

/**
 * @brief  绘制选项项（标签 + 右对齐选项值）
 * @param  y          — 行 Y 坐标
 * @param  label      — 标签文本
 * @param  value      — 选项值文本
 * @param  selected   — 是否选中
 * @param  back_color — 背景色（未使用）
 */
void ui_renderer_draw_option_item(uint16_t y,
                                  const char *label,
                                  const char *value,
                                  uint8_t selected,
                                  uint16_t back_color)
{
    uint16_t row_top = y;
    uint16_t row_bottom = (uint16_t)(y + UI_ROW_HEIGHT - 2U);
    uint16_t row_color = UI_PRODUCT_PANEL_COLOR;
    uint16_t edge_color = UI_PRODUCT_PANEL_EDGE_COLOR;
    uint16_t text_color = WHITE;
    uint16_t value_color = UI_PRODUCT_SUBTEXT_COLOR;
    (void)back_color;

    /* 选中状态使用强调色 */
    if (selected != 0U)
    {
        row_color = UI_PRODUCT_ACCENT_COLOR;
        edge_color = UI_PRODUCT_ACCENT_EDGE_COLOR;
        text_color = WHITE;
        value_color = WHITE;
    }

    app_display_runtime_lock();

    /* 面板背景与边框 */
    LCD_Fill(UI_PRODUCT_ROW_LEFT, row_top, UI_PRODUCT_ROW_RIGHT, row_bottom, row_color);
    LCD_DrawRectangle(UI_PRODUCT_ROW_LEFT, row_top, UI_PRODUCT_ROW_RIGHT, row_bottom, edge_color);

    /* 标签文本 */
    ui_renderer_draw_text((uint16_t)(UI_ITEM_LEFT_X + 8U), (uint16_t)(y + 4U), label, text_color, row_color);

    /* 选项值（右对齐） */
    ui_renderer_draw_text_right(UI_PRODUCT_ROW_VALUE_RIGHT,
                                (uint16_t)(y + 4U),
                                value,
                                value_color,
                                row_color);

    app_display_runtime_unlock();
}

/* =========================================================================
 *  21. 公共接口实现 —— 状态文本映射
 * ======================================================================= */

/**
 * @brief  将电池电量等级映射为中文文本
 * @param  level — 电量等级枚举
 * @return 中文 UTF-8 字节序列
 */
const char *ui_renderer_battery_level_text(battery_level_t level)
{
    switch (level)
    {
    case BATTERY_LEVEL_FULL:
        return "\xE6\xBB\xA1\xE7\x94\xB5";         /* "满电" */
    case BATTERY_LEVEL_HIGH:
        return "\xE9\xAB\x98\xE7\x94\xB5";         /* "高电" */
    case BATTERY_LEVEL_MEDIUM:
        return "\xE4\xB8\xAD\xE7\x94\xB5";         /* "中电" */
    case BATTERY_LEVEL_LOW:
        return "\xE4\xBD\x8E\xE7\x94\xB5";         /* "低电" */
    case BATTERY_LEVEL_ALERT:
    default:
        return "\xE5\x91\x8A\xE8\xAD\xA6";         /* "告警" */
    }
}

/**
 * @brief  将电源状态映射为中文文本
 * @param  state — 电源状态枚举
 * @return 中文 UTF-8 字节序列
 */
const char *ui_renderer_power_state_text(power_state_t state)
{
    switch (state)
    {
    case POWER_STATE_ACTIVE_THERMAL:
        return "\xE7\x83\xAD\xE5\x83\x8F";         /* "热像" */
    case POWER_STATE_SCREEN_OFF_IDLE:
        return "\xE7\x86\x84\xE5\xB1\x8F";         /* "熄屏" */
    case POWER_STATE_ACTIVE_UI:
    default:
        return "\xE7\x95\x8C\xE9\x9D\xA2";         /* "界面" */
    }
}

/**
 * @brief  将电源策略映射为中文文本
 * @param  policy — 电源策略枚举
 * @return 中文 UTF-8 字节序列
 */
const char *ui_renderer_power_policy_text(power_policy_t policy)
{
    switch (policy)
    {
    case POWER_POLICY_PERFORMANCE:
        return "\xE6\x80\xA7\xE8\x83\xBD";         /* "性能" */
    case POWER_POLICY_ECO:
        return "\xE7\x9C\x81\xE7\x94\xB5";         /* "省电" */
    case POWER_POLICY_BALANCED:
    default:
        return "\xE5\x9D\x87\xE8\xA1\xA1";         /* "均衡" */
    }
}

/**
 * @brief  将时钟策略映射为中文文本
 * @param  policy — 时钟策略枚举
 * @return 中文 UTF-8 字节序列
 */
const char *ui_renderer_clock_policy_text(clock_profile_policy_t policy)
{
    switch (policy)
    {
    case CLOCK_PROFILE_POLICY_HIGH_ONLY:
        return "\xE5\x9B\xBA\xE5\xAE\x9A\x31\x36\x38\x4D\x48\x7A";     /* "固定168MHz" */
    case CLOCK_PROFILE_POLICY_MEDIUM_ONLY:
        return "\xE5\x9B\xBA\xE5\xAE\x9A\x38\x34\x4D\x48\x7A";         /* "固定84MHz" */
    case CLOCK_PROFILE_POLICY_AUTO:
    default:
        return "\xE8\x87\xAA\xE5\x8A\xA8\xE5\x88\x87\xE6\x8D\xA2";     /* "自动切换" */
    }
}

/**
 * @brief  将时钟频率档位映射为中文文本
 * @param  profile — 时钟频率档位枚举
 * @return 中文 UTF-8 字节序列
 */
const char *ui_renderer_clock_profile_text(clock_profile_t profile)
{
    switch (profile)
    {
    case CLOCK_PROFILE_MEDIUM:
        return "\xE4\xB8\xAD\xE9\xA2\x91";         /* "中频" */
    case CLOCK_PROFILE_HIGH:
    default:
        return "\xE9\xAB\x98\xE9\xA2\x91";         /* "高频" */
    }
}
