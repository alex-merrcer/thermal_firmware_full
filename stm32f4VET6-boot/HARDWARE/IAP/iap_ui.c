#include "iap_ui.h"

#include <stdio.h>

#include "lcd_init.h"

#define IAP_UI_BG_COLOR              WHITE
#define IAP_UI_CARD_COLOR            LGRAYBLUE
#define IAP_UI_TEXT_COLOR            BLACK
#define IAP_UI_ACCENT_TEXT_COLOR     WHITE
#define IAP_UI_SUBTEXT_COLOR         GRAY
#define IAP_UI_PROGRESS_BG           LGRAY
#define IAP_UI_PROGRESS_TEXT_COLOR   BLACK
#define IAP_UI_SPLASH_ACCENT_COLOR   GBLUE

#define IAP_UI_HEADER_HEIGHT         48U
#define IAP_UI_CARD_LEFT             22U
#define IAP_UI_CARD_TOP              60U
#define IAP_UI_CARD_RIGHT            (LCD_W - 23U)
#define IAP_UI_CARD_BOTTOM           212U
#define IAP_UI_STATUS_Y              96U
#define IAP_UI_PROGRESS_LEFT         36U
#define IAP_UI_PROGRESS_TOP          144U
#define IAP_UI_PROGRESS_RIGHT        (LCD_W - 37U)
#define IAP_UI_PROGRESS_BOTTOM       170U
#define IAP_UI_PERCENT_Y             184U

typedef struct
{
    uint16_t codepoint;
    uint8_t bitmap[32];
} iap_ui_zh_glyph_t;

typedef enum
{
    IAP_UI_SCREEN_NONE = 0U,
    IAP_UI_SCREEN_SPLASH,
    IAP_UI_SCREEN_PREPARE,
    IAP_UI_SCREEN_PROGRESS,
    IAP_UI_SCREEN_SUCCESS,
    IAP_UI_SCREEN_FAILURE
} iap_ui_screen_t;

static uint8_t s_ymodem_last_percent = 0xFFU;
static uint8_t s_ymodem_progress_started = 0U;
static uint8_t s_iap_ui_backlight_on = 0U;
static uint8_t s_iap_ui_display_initialized = 0U;
static iap_ui_screen_t s_iap_ui_screen = IAP_UI_SCREEN_NONE;

static const iap_ui_zh_glyph_t s_iap_ui_zh_glyphs[] =
{
    { 0x6B63, { 0x00,0x00,0x7F,0xFE,0x00,0x80,0x00,0x80,0x10,0x80,0x10,0x80,0x10,0x80,0x10,0xFE,
                0x10,0x80,0x10,0x80,0x10,0x80,0x10,0x80,0x10,0x80,0xFF,0xFF,0x00,0x00,0x00,0x00 } },
    { 0x5728, { 0x02,0x00,0x02,0x00,0xFF,0xFF,0x04,0x00,0x08,0x20,0x08,0x20,0x10,0x20,0x33,0xFE,
                0x30,0x20,0x50,0x20,0x90,0x20,0x10,0x20,0x10,0x20,0x17,0xFF,0x10,0x00,0x00,0x00 } },
    { 0x5347, { 0x00,0x10,0x03,0xD0,0xFC,0x10,0x08,0x10,0x08,0x10,0x08,0x10,0xFF,0xFF,0x08,0x10,
                0x08,0x10,0x08,0x10,0x08,0x10,0x10,0x10,0x30,0x10,0x60,0x10,0xC0,0x10,0x00,0x00 } },
    { 0x7EA7, { 0x00,0x00,0x08,0x00,0x13,0xFC,0x10,0x84,0x24,0x88,0x48,0x90,0x78,0xDE,0x10,0xC2,
                0x30,0xE6,0x3E,0xA4,0x01,0xBC,0x1D,0x18,0x63,0x1C,0x02,0x66,0x04,0xC3,0x00,0x00 } },
    { 0x6210, { 0x00,0x50,0x00,0x4C,0x00,0x40,0x3F,0xFF,0x20,0x40,0x20,0x40,0x3E,0x42,0x22,0x44,
                0x22,0x6C,0x22,0x38,0x22,0x31,0x5E,0x71,0x40,0xD9,0x81,0x8E,0x00,0x00,0x00,0x00 } },
    { 0x529F, { 0x00,0x40,0x00,0x40,0xFE,0x40,0x13,0xFC,0x10,0x44,0x10,0x44,0x10,0x44,0x10,0x44,
                0x10,0xC4,0x16,0x84,0x39,0x84,0xC1,0x04,0x06,0x04,0x0C,0x3C,0x00,0x00,0x00,0x00 } },
    { 0x5931, { 0x00,0x80,0x08,0x80,0x10,0x80,0x1F,0xFE,0x20,0x80,0x40,0x80,0x00,0x80,0x7F,0xFF,
                0x01,0x80,0x01,0x40,0x03,0x40,0x06,0x20,0x0C,0x10,0x30,0x0C,0xC0,0x03,0x00,0x00 } },
    { 0x8D25, { 0x00,0x20,0x7E,0x60,0x42,0x40,0x52,0x7F,0x52,0xC4,0x53,0xC4,0x53,0x44,0x52,0x44,
                0x52,0x28,0x52,0x28,0x18,0x38,0x2C,0x10,0x26,0x68,0x42,0xC6,0x83,0x03,0x00,0x00 } },
    { 0x8FDB, { 0x41,0x10,0x21,0x10,0x31,0x10,0x17,0xFC,0x01,0x10,0x01,0x10,0xE1,0x10,0x2F,0xFE,
                0x21,0x10,0x21,0x10,0x23,0x10,0x22,0x10,0x64,0x10,0xD0,0x00,0x8F,0xFE,0x00,0x00 } },
    { 0x884C, { 0x08,0x00,0x11,0xFE,0x20,0x00,0x40,0x00,0x84,0x00,0x08,0x00,0x13,0xFF,0x10,0x08,
                0x30,0x08,0x50,0x08,0x10,0x08,0x10,0x08,0x10,0x08,0x10,0x08,0x10,0xF8,0x00,0x00 } },
    { 0x7248, { 0x48,0x07,0x49,0xF8,0x49,0x00,0x49,0x00,0x49,0x00,0x7D,0xFE,0x41,0x42,0x41,0x42,
                0x79,0x44,0x49,0x24,0x49,0x28,0x4B,0x10,0x4A,0x38,0x8A,0x46,0x8D,0x83,0x00,0x00 } },
    { 0x672C, { 0x00,0x00,0x01,0x00,0x01,0x00,0xFF,0xFE,0x03,0x80,0x03,0x80,0x05,0x40,0x09,0x40,
                0x09,0x20,0x11,0x10,0x21,0x18,0x41,0x0C,0x9F,0xF6,0x01,0x00,0x01,0x00,0x00,0x00 } }
};

static const uint16_t s_iap_ui_title_upgrade[] = { 0x7248, 0x672C, 0x5347, 0x7EA7 };
static const uint16_t s_iap_ui_status_running[] = { 0x6B63, 0x5728, 0x8FDB, 0x884C };
static const uint16_t s_iap_ui_status_success[] = { 0x5347, 0x7EA7, 0x6210, 0x529F };
static const uint16_t s_iap_ui_status_failure[] = { 0x5347, 0x7EA7, 0x5931, 0x8D25 };

static uint8_t iap_ui_ascii_char_width(uint8_t sizey)
{
    return (sizey >= 16U) ? 8U : 6U;
}

static uint16_t iap_ui_ascii_text_width(const char *text, uint8_t sizey)
{
    uint16_t width = 0U;

    if (text == 0)
    {
        return 0U;
    }

    width = (uint16_t)(strlen(text) * iap_ui_ascii_char_width(sizey));
    return width;
}

static void iap_ui_draw_ascii_center(uint16_t y,
                                     const char *text,
                                     uint16_t fc,
                                     uint16_t bc,
                                     uint8_t sizey)
{
    uint16_t width = 0U;
    uint16_t x = 0U;

    if (text == 0 || text[0] == '\0')
    {
        return;
    }

    width = iap_ui_ascii_text_width(text, sizey);
    if (width >= LCD_W)
    {
        x = 0U;
    }
    else
    {
        x = (uint16_t)((LCD_W - width) / 2U);
    }

    LCD_ShowString(x, y, (const u8 *)text, fc, bc, sizey, 0);
}

static const uint8_t *iap_ui_find_zh_bitmap(uint16_t codepoint)
{
    uint16_t index = 0U;

    for (index = 0U; index < (uint16_t)(sizeof(s_iap_ui_zh_glyphs) / sizeof(s_iap_ui_zh_glyphs[0])); ++index)
    {
        if (s_iap_ui_zh_glyphs[index].codepoint == codepoint)
        {
            return s_iap_ui_zh_glyphs[index].bitmap;
        }
    }

    return 0;
}

static void iap_ui_draw_mono_bitmap(uint16_t x,
                                    uint16_t y,
                                    const uint8_t *bitmap,
                                    uint8_t width,
                                    uint8_t height,
                                    uint16_t fc,
                                    uint16_t bc)
{
    uint8_t bytes_per_row = 0U;
    uint8_t row = 0U;
    uint8_t col = 0U;

    if (bitmap == 0 || width == 0U || height == 0U)
    {
        return;
    }

    bytes_per_row = (uint8_t)((width + 7U) / 8U);
    for (row = 0U; row < height; ++row)
    {
        for (col = 0U; col < width; ++col)
        {
            uint16_t byte_index = (uint16_t)(row * bytes_per_row) + (uint16_t)(col / 8U);
            uint8_t mask = (uint8_t)(0x80U >> (col % 8U));
            uint16_t color = ((bitmap[byte_index] & mask) != 0U) ? fc : bc;

            LCD_DrawPoint((uint16_t)(x + col), (uint16_t)(y + row), color);
        }
    }
}

static void iap_ui_draw_zh_text(uint16_t x,
                                uint16_t y,
                                const uint16_t *codepoints,
                                uint8_t count,
                                uint16_t fc,
                                uint16_t bc)
{
    uint8_t index = 0U;

    if (codepoints == 0 || count == 0U)
    {
        return;
    }

    for (index = 0U; index < count; ++index)
    {
        const uint8_t *bitmap = iap_ui_find_zh_bitmap(codepoints[index]);

        if (bitmap != 0)
        {
            iap_ui_draw_mono_bitmap((uint16_t)(x + ((uint16_t)index * 16U)),
                                    y,
                                    bitmap,
                                    16U,
                                    16U,
                                    fc,
                                    bc);
        }
    }
}

static void iap_ui_draw_zh_text_center(uint16_t y,
                                       const uint16_t *codepoints,
                                       uint8_t count,
                                       uint16_t fc,
                                       uint16_t bc)
{
    uint16_t width = 0U;
    uint16_t x = 0U;

    if (codepoints == 0 || count == 0U)
    {
        return;
    }

    width = (uint16_t)(count * 16U);
    if (width < LCD_W)
    {
        x = (uint16_t)((LCD_W - width) / 2U);
    }

    iap_ui_draw_zh_text(x, y, codepoints, count, fc, bc);
}

static uint8_t iap_ui_text_has_error(const char *line1, const char *line2)
{
    if ((line1 != 0 && (strstr(line1, "timeout") != 0 ||
                        strstr(line1, "fail") != 0 ||
                        strstr(line1, "Fail") != 0 ||
                        strstr(line1, "bad") != 0 ||
                        strstr(line1, "Error") != 0 ||
                        strstr(line1, "rejected") != 0)) ||
        (line2 != 0 && (strstr(line2, "timeout") != 0 ||
                        strstr(line2, "fail") != 0 ||
                        strstr(line2, "Fail") != 0 ||
                        strstr(line2, "bad") != 0 ||
                        strstr(line2, "Error") != 0 ||
                        strstr(line2, "rejected") != 0)))
    {
        return 1U;
    }

    return 0U;
}

static void iap_ui_draw_progress_bar(uint8_t percent, uint16_t accent_color)
{
    uint16_t inner_left = (uint16_t)(IAP_UI_PROGRESS_LEFT + 3U);
    uint16_t inner_top = (uint16_t)(IAP_UI_PROGRESS_TOP + 3U);
    uint16_t inner_right = (uint16_t)(IAP_UI_PROGRESS_RIGHT - 3U);
    uint16_t inner_bottom = (uint16_t)(IAP_UI_PROGRESS_BOTTOM - 3U);
    uint16_t inner_width = 0U;
    uint16_t fill_width = 0U;

    LCD_DrawRectangle(IAP_UI_PROGRESS_LEFT,
                      IAP_UI_PROGRESS_TOP,
                      IAP_UI_PROGRESS_RIGHT,
                      IAP_UI_PROGRESS_BOTTOM,
                      IAP_UI_TEXT_COLOR);
    LCD_Fill(inner_left,
             inner_top,
             (uint16_t)(inner_right + 1U),
             (uint16_t)(inner_bottom + 1U),
             IAP_UI_PROGRESS_BG);

    if (percent > 100U)
    {
        percent = 100U;
    }

    inner_width = (uint16_t)(inner_right - inner_left + 1U);
    if (percent > 0U && inner_width > 0U)
    {
        fill_width = (uint16_t)(((uint32_t)inner_width * percent) / 100UL);
        if (fill_width == 0U)
        {
            fill_width = 1U;
        }

        LCD_Fill(inner_left,
                 inner_top,
                 (uint16_t)(inner_left + fill_width),
                 (uint16_t)(inner_bottom + 1U),
                 accent_color);
    }
}

static void iap_ui_draw_progress_percent(uint8_t percent)
{
    char percent_text[8];
    uint16_t region_width = iap_ui_ascii_text_width("100%", 16U);
    uint16_t region_x = 0U;

    if (region_width < LCD_W)
    {
        region_x = (uint16_t)((LCD_W - region_width) / 2U);
    }

    LCD_Fill((region_x > 4U) ? (uint16_t)(region_x - 4U) : 0U,
             IAP_UI_PERCENT_Y,
             (uint16_t)(region_x + region_width + 4U),
             (uint16_t)(IAP_UI_PERCENT_Y + 16U),
             IAP_UI_BG_COLOR);

    snprintf(percent_text, sizeof(percent_text), "%u%%", percent);
    iap_ui_draw_ascii_center(IAP_UI_PERCENT_Y,
                             percent_text,
                             IAP_UI_PROGRESS_TEXT_COLOR,
                             IAP_UI_BG_COLOR,
                             16U);
}

static void iap_ui_present_if_needed(void)
{
    if (s_iap_ui_backlight_on == 0U)
    {
        LCD_BLK_Set();
        s_iap_ui_backlight_on = 1U;
    }
}

void iap_ui_boot_prepare(uint8_t warm_handoff)
{
    if (s_iap_ui_display_initialized != 0U)
    {
        return;
    }

    if (warm_handoff != 0U)
    {
        LCD_GPIO_Rebind(1U);
        s_iap_ui_backlight_on = 1U;
    }
    else
    {
        LCD_Init();
        s_iap_ui_backlight_on = 0U;
    }

    s_iap_ui_display_initialized = 1U;
    s_iap_ui_screen = IAP_UI_SCREEN_NONE;
    s_ymodem_last_percent = 0xFFU;
    s_ymodem_progress_started = 0U;
}

static void iap_ui_draw_upgrade_frame(uint16_t accent_color)
{
    LCD_Fill(0U, 0U, LCD_W, LCD_H, IAP_UI_BG_COLOR);
    LCD_Fill(0U, 0U, LCD_W, IAP_UI_HEADER_HEIGHT, accent_color);
    LCD_DrawRectangle(IAP_UI_CARD_LEFT,
                      IAP_UI_CARD_TOP,
                      IAP_UI_CARD_RIGHT,
                      IAP_UI_CARD_BOTTOM,
                      IAP_UI_CARD_COLOR);
    iap_ui_draw_zh_text_center(16U,
                               s_iap_ui_title_upgrade,
                               (uint8_t)(sizeof(s_iap_ui_title_upgrade) / sizeof(s_iap_ui_title_upgrade[0])),
                               IAP_UI_ACCENT_TEXT_COLOR,
                               accent_color);
}

static void iap_ui_render_upgrade_screen(const uint16_t *status_text,
                                         uint8_t status_count,
                                         uint8_t percent,
                                         uint8_t show_percent,
                                         uint16_t accent_color)
{
    iap_ui_draw_upgrade_frame(accent_color);
    iap_ui_draw_zh_text_center(IAP_UI_STATUS_Y,
                               status_text,
                               status_count,
                               IAP_UI_TEXT_COLOR,
                               IAP_UI_BG_COLOR);
    iap_ui_draw_ascii_center(122U, "RedPic1 OTA", IAP_UI_SUBTEXT_COLOR, IAP_UI_BG_COLOR, 16U);
    iap_ui_draw_progress_bar(percent, accent_color);

    if (show_percent != 0U)
    {
        iap_ui_draw_progress_percent(percent);
    }

    iap_ui_present_if_needed();
}

void iap_ui_show_boot_splash(void)
{
    if (s_iap_ui_screen == IAP_UI_SCREEN_SPLASH)
    {
        iap_ui_present_if_needed();
        return;
    }

    LCD_Fill(0U, 0U, LCD_W, LCD_H, IAP_UI_BG_COLOR);
    LCD_Fill(0U, 0U, LCD_W, 56U, IAP_UI_SPLASH_ACCENT_COLOR);
    LCD_DrawRectangle(26U, 72U, LCD_W - 27U, 190U, IAP_UI_CARD_COLOR);
    iap_ui_draw_ascii_center(18U, "RedPic1", IAP_UI_ACCENT_TEXT_COLOR, IAP_UI_SPLASH_ACCENT_COLOR, 16U);
    iap_ui_draw_ascii_center(102U, "Thermal Imager", IAP_UI_TEXT_COLOR, IAP_UI_BG_COLOR, 16U);
    iap_ui_draw_ascii_center(132U, "Embedded Product", IAP_UI_SUBTEXT_COLOR, IAP_UI_BG_COLOR, 16U);
    s_iap_ui_screen = IAP_UI_SCREEN_SPLASH;
    iap_ui_present_if_needed();
}

void iap_ui_show_upgrade_prepare(void)
{
    if (s_iap_ui_screen == IAP_UI_SCREEN_PREPARE)
    {
        iap_ui_present_if_needed();
        return;
    }

    iap_ui_render_upgrade_screen(s_iap_ui_status_running,
                                 (uint8_t)(sizeof(s_iap_ui_status_running) / sizeof(s_iap_ui_status_running[0])),
                                 0U,
                                 0U,
                                 GBLUE);
    s_iap_ui_screen = IAP_UI_SCREEN_PREPARE;
}

void iap_ui_show_upgrade_success(void)
{
    if (s_iap_ui_screen == IAP_UI_SCREEN_SUCCESS)
    {
        iap_ui_present_if_needed();
        return;
    }

    iap_ui_render_upgrade_screen(s_iap_ui_status_success,
                                 (uint8_t)(sizeof(s_iap_ui_status_success) / sizeof(s_iap_ui_status_success[0])),
                                 100U,
                                 1U,
                                 GREEN);
    s_iap_ui_screen = IAP_UI_SCREEN_SUCCESS;
}

void iap_ui_show_upgrade_failure(void)
{
    uint8_t percent = (s_ymodem_last_percent == 0xFFU) ? 0U : s_ymodem_last_percent;

    if (s_iap_ui_screen == IAP_UI_SCREEN_FAILURE &&
        percent == s_ymodem_last_percent)
    {
        iap_ui_present_if_needed();
        return;
    }

    iap_ui_render_upgrade_screen(s_iap_ui_status_failure,
                                 (uint8_t)(sizeof(s_iap_ui_status_failure) / sizeof(s_iap_ui_status_failure[0])),
                                 percent,
                                 1U,
                                 RED);
    s_iap_ui_screen = IAP_UI_SCREEN_FAILURE;
}

void ota_ctrl_show_status_lines(u16 color,
                                const char *line1,
                                const char *line2,
                                const char *line3,
                                const char *line4)
{
    (void)line3;
    (void)line4;

    if (color == RED || iap_ui_text_has_error(line1, line2) != 0U)
    {
        iap_ui_show_upgrade_failure();
        return;
    }

    if (s_iap_ui_screen == IAP_UI_SCREEN_NONE)
    {
        iap_ui_show_upgrade_prepare();
    }
}

void ota_ctrl_show_status_text(const char *line1, const char *line2)
{
    if (iap_ui_text_has_error(line1, line2) != 0U)
    {
        iap_ui_show_upgrade_failure();
        return;
    }

    if (s_iap_ui_screen == IAP_UI_SCREEN_NONE)
    {
        iap_ui_show_upgrade_prepare();
    }
}

void ota_ctrl_show_stage(uint8_t stage,
                         uint8_t percent,
                         uint16_t detail_code,
                         uint32_t current_value,
                         uint32_t total_value)
{
    (void)detail_code;
    (void)current_value;
    (void)total_value;

    if (stage == OTA_CTRL_STAGE_DONE && percent >= 100U)
    {
        iap_ui_show_upgrade_success();
        return;
    }

    if (stage == OTA_CTRL_STAGE_TRANSFER && percent != OTA_CTRL_PERCENT_UNKNOWN)
    {
        iap_ui_render_upgrade_screen(s_iap_ui_status_running,
                                     (uint8_t)(sizeof(s_iap_ui_status_running) / sizeof(s_iap_ui_status_running[0])),
                                     percent,
                                     1U,
                                     GBLUE);
        s_iap_ui_screen = IAP_UI_SCREEN_PROGRESS;
        return;
    }

    if (s_iap_ui_screen == IAP_UI_SCREEN_NONE)
    {
        iap_ui_show_upgrade_prepare();
    }
}

void ota_ctrl_show_ready_info(const ota_ctrl_frame_t *frame)
{
    (void)frame;
    if (s_iap_ui_screen == IAP_UI_SCREEN_NONE)
    {
        iap_ui_show_upgrade_prepare();
    }
}

void ota_ctrl_show_error_code(uint8_t stage, uint16_t error_code)
{
    (void)stage;
    (void)error_code;
    iap_ui_show_upgrade_failure();
}

void ota_ctrl_show_ack_reject_reason(uint16_t reason_code)
{
    (void)reason_code;
    iap_ui_show_upgrade_failure();
}

void iap_show_version_lines(const BootInfoTypeDef *boot_info)
{
    (void)boot_info;
}

void iap_show_resume_decision(uint8_t accepted,
                              uint16_t reason_code,
                              uint32_t saved_offset,
                              uint32_t total_size)
{
    (void)accepted;
    (void)reason_code;
    (void)saved_offset;
    (void)total_size;
}

void iap_reset_ymodem_progress(void)
{
    s_ymodem_last_percent = 0xFFU;
    s_ymodem_progress_started = 0U;
}

void iap_show_ymodem_progress(uint32_t current, uint32_t total)
{
    uint32_t safe_current = current;
    uint8_t percent = 0U;
    uint8_t was_started = s_ymodem_progress_started;

    if (total > 0U)
    {
        if (safe_current > total)
        {
            safe_current = total;
        }
        percent = (uint8_t)((safe_current * 100U) / total);
    }

    iap_feed_watchdog();

    if (s_ymodem_progress_started != 0U &&
        percent == s_ymodem_last_percent &&
        (total == 0U || safe_current < total))
    {
        return;
    }

    s_ymodem_progress_started = 1U;
    s_ymodem_last_percent = percent;

    if (was_started != 0U)
    {
        iap_ui_draw_progress_bar(percent, GBLUE);
        iap_ui_draw_progress_percent(percent);
        s_iap_ui_screen = IAP_UI_SCREEN_PROGRESS;
        iap_ui_present_if_needed();
        return;
    }

    iap_ui_render_upgrade_screen(s_iap_ui_status_running,
                                 (uint8_t)(sizeof(s_iap_ui_status_running) / sizeof(s_iap_ui_status_running[0])),
                                 percent,
                                 1U,
                                 GBLUE);
    s_iap_ui_screen = IAP_UI_SCREEN_PROGRESS;
}
