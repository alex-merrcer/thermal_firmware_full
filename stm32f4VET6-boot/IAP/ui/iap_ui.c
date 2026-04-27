#include "iap_ui.h"

#include "lcd_init.h"
#include "iap_ui_boot_assets.h"

#define IAP_UI_BG_COLOR                  WHITE
#define IAP_UI_CARD_COLOR                LGRAYBLUE
#define IAP_UI_TEXT_COLOR                BLACK
#define IAP_UI_ACCENT_TEXT_COLOR         WHITE
#define IAP_UI_SUBTEXT_COLOR             GRAY
#define IAP_UI_PROGRESS_BG               LGRAY
#define IAP_UI_PROGRESS_TEXT_COLOR       BLACK
#define IAP_UI_SPLASH_ACCENT_COLOR       GBLUE

#define IAP_UI_HEADER_HEIGHT             48U
#define IAP_UI_CARD_LEFT                 22U
#define IAP_UI_CARD_TOP                  60U
#define IAP_UI_CARD_RIGHT                (LCD_W - 23U)
#define IAP_UI_CARD_BOTTOM               212U
#define IAP_UI_STATUS_Y                  96U
#define IAP_UI_PROGRESS_LEFT             36U
#define IAP_UI_PROGRESS_TOP              144U
#define IAP_UI_PROGRESS_RIGHT            (LCD_W - 37U)
#define IAP_UI_PROGRESS_BOTTOM           170U
#define IAP_UI_PERCENT_Y                 184U

#define IAP_UI_RGB565(r, g, b)          ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | \
                                                    (((uint16_t)(g) & 0xFCU) << 3) | \
                                                    (((uint16_t)(b) & 0xF8U) >> 3)))

#define IAP_UI_BOOT_BG_COLOR             IAP_UI_RGB565(5U, 12U, 22U)
#define IAP_UI_BOOT_DIVIDER_COLOR        IAP_UI_RGB565(36U, 48U, 70U)
#define IAP_UI_BOOT_LINE_COLOR           IAP_UI_RGB565(49U, 68U, 95U)
#define IAP_UI_BOOT_MUTED_COLOR          IAP_UI_RGB565(129U, 142U, 160U)
#define IAP_UI_BOOT_CYAN_COLOR           IAP_UI_RGB565(103U, 203U, 255U)
#define IAP_UI_BOOT_CYAN_SOFT_COLOR      IAP_UI_RGB565(68U, 124U, 164U)
#define IAP_UI_BOOT_TRACK_COLOR          IAP_UI_RGB565(28U, 34U, 46U)
#define IAP_UI_BOOT_TRACK_BORDER_COLOR   IAP_UI_RGB565(70U, 83U, 104U)
#define IAP_UI_BOOT_ORANGE_HI_COLOR      IAP_UI_RGB565(255U, 199U, 76U)
#define IAP_UI_BOOT_ORANGE_MID_COLOR     IAP_UI_RGB565(255U, 148U, 44U)
#define IAP_UI_BOOT_ORANGE_LOW_COLOR     IAP_UI_RGB565(255U, 90U, 38U)
#define IAP_UI_BOOT_SHADOW_COLOR         IAP_UI_RGB565(23U, 36U, 54U)

#define IAP_UI_BOOT_LEFT_CONTENT_LEFT    8U
#define IAP_UI_BOOT_LEFT_CONTENT_RIGHT   146U
#define IAP_UI_BOOT_RIGHT_CONTENT_LEFT   168U
#define IAP_UI_BOOT_RIGHT_CONTENT_RIGHT  304U
#define IAP_UI_BOOT_DIVIDER_X            154U
#define IAP_UI_BOOT_ICON_X               48U
#define IAP_UI_BOOT_ICON_Y               20U
#define IAP_UI_BOOT_TITLE_Y              112U
#define IAP_UI_BOOT_SUBTITLE_Y           146U
#define IAP_UI_BOOT_SUBTITLE_LINE_Y      164U
#define IAP_UI_BOOT_HEADER_Y             24U
#define IAP_UI_BOOT_STEP1_Y              74U
#define IAP_UI_BOOT_STEP2_Y              110U
#define IAP_UI_BOOT_STEP3_Y              146U
#define IAP_UI_BOOT_STEP_CLEAR_RIGHT     307U
#define IAP_UI_BOOT_STEP_ICON_X          176U
#define IAP_UI_BOOT_STEP_BAR_X           198U
#define IAP_UI_BOOT_STEP_TEXT_X          210U
#define IAP_UI_BOOT_TRACK_LEFT           170U
#define IAP_UI_BOOT_TRACK_TOP            178U
#define IAP_UI_BOOT_TRACK_RIGHT          301U
#define IAP_UI_BOOT_TRACK_BOTTOM         194U
#define IAP_UI_BOOT_PROGRESS_TEXT_Y      202U
#define IAP_UI_BOOT_HINT_Y               222U
#define IAP_UI_BOOT_HINT_GLYPH_SIZE      12U
#define IAP_UI_BOOT_FRAME_TOP            20U
#define IAP_UI_BOOT_FRAME_BOTTOM         218U
#define IAP_UI_BOOT_STEP_TICK_MS         80U

typedef enum
{
    IAP_UI_SCREEN_NONE = 0U,
    IAP_UI_SCREEN_SPLASH,
    IAP_UI_SCREEN_PREPARE,
    IAP_UI_SCREEN_PROGRESS,
    IAP_UI_SCREEN_SUCCESS,
    IAP_UI_SCREEN_FAILURE
} iap_ui_screen_t;

typedef struct
{
    uint16_t elapsed_ms;
    uint8_t percent;
} iap_ui_boot_keyframe_t;

typedef enum
{
    IAP_UI_BOOT_STEP_PENDING = 0U,
    IAP_UI_BOOT_STEP_ACTIVE = 1U,
    IAP_UI_BOOT_STEP_DONE = 2U
} iap_ui_boot_step_state_t;

static uint8_t s_ymodem_last_percent = 0xFFU;
static uint8_t s_ymodem_progress_started = 0U;
static uint8_t s_iap_ui_backlight_on = 0U;
static uint8_t s_iap_ui_display_initialized = 0U;
static iap_ui_screen_t s_iap_ui_screen = IAP_UI_SCREEN_NONE;
static uint8_t s_boot_last_percent = 0xFFU;
static iap_ui_boot_step_state_t s_boot_last_step0 = (iap_ui_boot_step_state_t)0xFFU;
static iap_ui_boot_step_state_t s_boot_last_step1 = (iap_ui_boot_step_state_t)0xFFU;
static iap_ui_boot_step_state_t s_boot_last_step2 = (iap_ui_boot_step_state_t)0xFFU;
static uint16_t s_boot_last_fill_right = 0xFFFFU;
static uint16_t s_boot_percent_value_x = 0U;
static uint16_t s_boot_percent_value_y = 0U;
static uint16_t s_boot_percent_value_right = 0U;

static const uint16_t s_iap_ui_title_upgrade[] = { 0x7248, 0x672C, 0x5347, 0x7EA7 };
static const uint16_t s_iap_ui_status_running[] = { 0x6B63, 0x5728, 0x8FDB, 0x884C };
static const uint16_t s_iap_ui_status_success[] = { 0x5347, 0x7EA7, 0x6210, 0x529F };
static const uint16_t s_iap_ui_status_failure[] = { 0x5347, 0x7EA7, 0x5931, 0x8D25 };

static const uint16_t s_iap_ui_boot_title[] = { 0x7EA2, 0x5916, 0x70ED, 0x6210, 0x50CF, 0x7EC8, 0x7AEF };
static const uint16_t s_iap_ui_boot_subtitle[] = { 0x667A, 0x80FD, 0x6D4B, 0x6E29, 0x8BBE, 0x5907 };
static const uint16_t s_iap_ui_boot_header[] = { 0x7CFB, 0x7EDF, 0x542F, 0x52A8, 0x4E2D };
static const uint16_t s_iap_ui_boot_step_fw[] = { 0x56FA, 0x4EF6, 0x68C0, 0x67E5 };
static const uint16_t s_iap_ui_boot_step_verify[] = { 0x7CFB, 0x7EDF, 0x6821, 0x9A8C };
static const uint16_t s_iap_ui_boot_step_ready[] = { 0x51C6, 0x5907, 0x542F, 0x52A8 };
static const uint16_t s_iap_ui_boot_progress_text[] = { 0x542F, 0x52A8, 0x8FDB, 0x5EA6 };
static const uint16_t s_iap_ui_boot_hint[] = { 0x5373, 0x5C06, 0x8FDB, 0x5165, 0x4E3B, 0x754C, 0x9762, 0xFF0C, 0x8BF7, 0x7A0D, 0x540E };

static const iap_ui_boot_keyframe_t s_iap_ui_boot_keyframes[] =
{
    { 0U, 0U },
    { 300U, 15U },
    { 800U, 40U },
    { 1300U, 70U },
    { 1800U, 95U },
    { 2000U, 100U }
};

static uint8_t iap_ui_ascii_char_width(uint8_t sizey)
{
    return (sizey >= 16U) ? 8U : 6U;
}

static uint16_t iap_ui_ascii_strlen(const char *text)
{
    uint16_t length = 0U;

    if (text == 0)
    {
        return 0U;
    }

    while (text[length] != '\0')
    {
        ++length;
    }

    return length;
}

static uint16_t iap_ui_ascii_text_width(const char *text, uint8_t sizey)
{
    return (uint16_t)(iap_ui_ascii_strlen(text) * iap_ui_ascii_char_width(sizey));
}

static uint16_t iap_ui_center_x_in_range(uint16_t left, uint16_t right, uint16_t width)
{
    uint16_t range_width = 0U;

    if (right <= left)
    {
        return left;
    }

    range_width = (uint16_t)(right - left + 1U);
    if (width >= range_width)
    {
        return left;
    }

    return (uint16_t)(left + ((range_width - width) / 2U));
}

static void iap_ui_draw_ascii(uint16_t x,
                              uint16_t y,
                              const char *text,
                              uint16_t fc,
                              uint16_t bc,
                              uint8_t sizey)
{
    if (text == 0 || text[0] == '\0')
    {
        return;
    }

    LCD_ShowString(x, y, (const u8 *)text, fc, bc, sizey, 0U);
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

    LCD_ShowString(x, y, (const u8 *)text, fc, bc, sizey, 0U);
}

static const uint8_t *iap_ui_find_zh_bitmap(uint16_t codepoint)
{
    uint16_t index = 0U;

    for (index = 0U;
         index < (uint16_t)(sizeof(iap_ui_boot_zh_glyphs) / sizeof(iap_ui_boot_zh_glyphs[0]));
         ++index)
    {
        if (iap_ui_boot_zh_glyphs[index].codepoint == codepoint)
        {
            return iap_ui_boot_zh_glyphs[index].bitmap;
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

static void iap_ui_draw_mono_bitmap_masked(uint16_t x,
                                           uint16_t y,
                                           const uint8_t *bitmap,
                                           uint8_t width,
                                           uint8_t height,
                                           uint16_t fc)
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

            if ((bitmap[byte_index] & mask) != 0U)
            {
                LCD_DrawPoint((uint16_t)(x + col), (uint16_t)(y + row), fc);
            }
        }
    }
}

static void iap_ui_draw_mono_bitmap_scaled_masked(uint16_t x,
                                                  uint16_t y,
                                                  const uint8_t *bitmap,
                                                  uint8_t src_width,
                                                  uint8_t src_height,
                                                  uint8_t dst_width,
                                                  uint8_t dst_height,
                                                  uint16_t fc)
{
    uint8_t bytes_per_row = 0U;
    uint8_t dst_row = 0U;
    uint8_t dst_col = 0U;

    if (bitmap == 0 || src_width == 0U || src_height == 0U || dst_width == 0U || dst_height == 0U)
    {
        return;
    }

    bytes_per_row = (uint8_t)((src_width + 7U) / 8U);
    for (dst_row = 0U; dst_row < dst_height; ++dst_row)
    {
        uint8_t src_row = (uint8_t)(((uint16_t)dst_row * src_height) / dst_height);

        if (src_row >= src_height)
        {
            src_row = (uint8_t)(src_height - 1U);
        }

        for (dst_col = 0U; dst_col < dst_width; ++dst_col)
        {
            uint8_t src_col = (uint8_t)(((uint16_t)dst_col * src_width) / dst_width);
            uint16_t byte_index = 0U;
            uint8_t mask = 0U;

            if (src_col >= src_width)
            {
                src_col = (uint8_t)(src_width - 1U);
            }

            byte_index = (uint16_t)(src_row * bytes_per_row) + (uint16_t)(src_col / 8U);
            mask = (uint8_t)(0x80U >> (src_col % 8U));
            if ((bitmap[byte_index] & mask) != 0U)
            {
                LCD_DrawPoint((uint16_t)(x + dst_col), (uint16_t)(y + dst_row), fc);
            }
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

static void iap_ui_draw_zh_text_scaled_center(uint16_t y,
                                              const uint16_t *codepoints,
                                              uint8_t count,
                                              uint8_t size,
                                              uint16_t fc)
{
    uint16_t width = 0U;
    uint16_t x = 0U;
    uint8_t index = 0U;

    if (codepoints == 0 || count == 0U || size == 0U)
    {
        return;
    }

    width = (uint16_t)(count * size);
    if (width < LCD_W)
    {
        x = (uint16_t)((LCD_W - width) / 2U);
    }

    for (index = 0U; index < count; ++index)
    {
        const uint8_t *bitmap = iap_ui_find_zh_bitmap(codepoints[index]);

        if (bitmap != 0)
        {
            iap_ui_draw_mono_bitmap_scaled_masked((uint16_t)(x + ((uint16_t)index * size)),
                                                  y,
                                                  bitmap,
                                                  16U,
                                                  16U,
                                                  size,
                                                  size,
                                                  fc);
        }
    }
}

static void iap_ui_fill_rect_inclusive(uint16_t left,
                                       uint16_t top,
                                       uint16_t right,
                                       uint16_t bottom,
                                       uint16_t color)
{
    if (right < left || bottom < top)
    {
        return;
    }

    LCD_Fill(left, top, (uint16_t)(right + 1U), (uint16_t)(bottom + 1U), color);
}

static void iap_ui_draw_hline(uint16_t x, uint16_t y, uint16_t width, uint16_t color)
{
    if (width == 0U)
    {
        return;
    }

    iap_ui_fill_rect_inclusive(x, y, (uint16_t)(x + width - 1U), y, color);
}

static void iap_ui_draw_vline(uint16_t x, uint16_t y, uint16_t height, uint16_t color)
{
    if (height == 0U)
    {
        return;
    }

    iap_ui_fill_rect_inclusive(x, y, x, (uint16_t)(y + height - 1U), color);
}

static uint8_t iap_ui_ascii_contains(const char *text, const char *pattern)
{
    uint16_t pattern_index = 0U;

    if (text == 0 || pattern == 0 || pattern[0] == '\0')
    {
        return 0U;
    }

    while (*text != '\0')
    {
        pattern_index = 0U;
        while (pattern[pattern_index] != '\0' &&
               text[pattern_index] != '\0' &&
               text[pattern_index] == pattern[pattern_index])
        {
            ++pattern_index;
        }

        if (pattern[pattern_index] == '\0')
        {
            return 1U;
        }

        ++text;
    }

    return 0U;
}

static uint8_t iap_ui_text_has_error(const char *line1, const char *line2)
{
    if ((line1 != 0 && (iap_ui_ascii_contains(line1, "timeout") != 0U ||
                        iap_ui_ascii_contains(line1, "fail") != 0U ||
                        iap_ui_ascii_contains(line1, "Fail") != 0U ||
                        iap_ui_ascii_contains(line1, "bad") != 0U ||
                        iap_ui_ascii_contains(line1, "Error") != 0U ||
                        iap_ui_ascii_contains(line1, "rejected") != 0U)) ||
        (line2 != 0 && (iap_ui_ascii_contains(line2, "timeout") != 0U ||
                        iap_ui_ascii_contains(line2, "fail") != 0U ||
                        iap_ui_ascii_contains(line2, "Fail") != 0U ||
                        iap_ui_ascii_contains(line2, "bad") != 0U ||
                        iap_ui_ascii_contains(line2, "Error") != 0U ||
                        iap_ui_ascii_contains(line2, "rejected") != 0U)))
    {
        return 1U;
    }

    return 0U;
}

static void iap_ui_format_percent_text(uint8_t percent, char out[5])
{
    if (percent >= 100U)
    {
        out[0] = '1';
        out[1] = '0';
        out[2] = '0';
        out[3] = '%';
        out[4] = '\0';
        return;
    }

    if (percent >= 10U)
    {
        out[0] = (char)('0' + (percent / 10U));
        out[1] = (char)('0' + (percent % 10U));
        out[2] = '%';
        out[3] = '\0';
        return;
    }

    out[0] = (char)('0' + percent);
    out[1] = '%';
    out[2] = '\0';
}

static uint8_t iap_ui_boot_progress_percent(uint16_t elapsed_ms)
{
    uint16_t index = 0U;

    if (elapsed_ms >= s_iap_ui_boot_keyframes[(sizeof(s_iap_ui_boot_keyframes) / sizeof(s_iap_ui_boot_keyframes[0])) - 1U].elapsed_ms)
    {
        return 100U;
    }

    for (index = 1U; index < (uint16_t)(sizeof(s_iap_ui_boot_keyframes) / sizeof(s_iap_ui_boot_keyframes[0])); ++index)
    {
        const iap_ui_boot_keyframe_t *previous = &s_iap_ui_boot_keyframes[index - 1U];
        const iap_ui_boot_keyframe_t *next = &s_iap_ui_boot_keyframes[index];

        if (elapsed_ms <= next->elapsed_ms)
        {
            uint32_t range = (uint32_t)(next->elapsed_ms - previous->elapsed_ms);
            uint32_t delta = (uint32_t)(elapsed_ms - previous->elapsed_ms);
            uint32_t percent_delta = (uint32_t)(next->percent - previous->percent);

            if (range == 0U)
            {
                return next->percent;
            }

            return (uint8_t)(previous->percent + ((delta * percent_delta) / range));
        }
    }

    return 100U;
}

static iap_ui_boot_step_state_t iap_ui_boot_step_state(uint16_t elapsed_ms, uint8_t step_index)
{
    if (step_index == 0U)
    {
        return (elapsed_ms < 800U) ? IAP_UI_BOOT_STEP_ACTIVE : IAP_UI_BOOT_STEP_DONE;
    }

    if (step_index == 1U)
    {
        if (elapsed_ms < 800U)
        {
            return IAP_UI_BOOT_STEP_PENDING;
        }

        return (elapsed_ms < 1300U) ? IAP_UI_BOOT_STEP_ACTIVE : IAP_UI_BOOT_STEP_DONE;
    }

    if (elapsed_ms < 1300U)
    {
        return IAP_UI_BOOT_STEP_PENDING;
    }

    return (elapsed_ms < 1900U) ? IAP_UI_BOOT_STEP_ACTIVE : IAP_UI_BOOT_STEP_DONE;
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
    char percent_text[5];
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

    iap_ui_format_percent_text(percent, percent_text);
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

static void iap_ui_draw_boot_brand(void)
{
    uint16_t title_x = 0U;
    uint16_t subtitle_x = 0U;

    iap_ui_draw_mono_bitmap_masked(IAP_UI_BOOT_ICON_X,
                                   IAP_UI_BOOT_ICON_Y,
                                   iap_ui_boot_logo_blue_56,
                                   56U,
                                   56U,
                                   IAP_UI_BOOT_CYAN_COLOR);
    iap_ui_draw_mono_bitmap_masked(IAP_UI_BOOT_ICON_X,
                                   IAP_UI_BOOT_ICON_Y,
                                   iap_ui_boot_logo_orange_56,
                                   56U,
                                   56U,
                                   IAP_UI_BOOT_ORANGE_MID_COLOR);

    title_x = iap_ui_center_x_in_range(IAP_UI_BOOT_LEFT_CONTENT_LEFT,
                                       IAP_UI_BOOT_LEFT_CONTENT_RIGHT,
                                       (uint16_t)(sizeof(s_iap_ui_boot_title) / sizeof(s_iap_ui_boot_title[0])) * 16U);
    subtitle_x = iap_ui_center_x_in_range(IAP_UI_BOOT_LEFT_CONTENT_LEFT,
                                          IAP_UI_BOOT_LEFT_CONTENT_RIGHT,
                                          (uint16_t)(sizeof(s_iap_ui_boot_subtitle) / sizeof(s_iap_ui_boot_subtitle[0])) * 16U);

    iap_ui_draw_zh_text((uint16_t)(title_x + 1U),
                        (uint16_t)(IAP_UI_BOOT_TITLE_Y + 1U),
                        s_iap_ui_boot_title,
                        (uint8_t)(sizeof(s_iap_ui_boot_title) / sizeof(s_iap_ui_boot_title[0])),
                        IAP_UI_BOOT_SHADOW_COLOR,
                        IAP_UI_BOOT_BG_COLOR);
    iap_ui_draw_zh_text(title_x,
                        IAP_UI_BOOT_TITLE_Y,
                        s_iap_ui_boot_title,
                        (uint8_t)(sizeof(s_iap_ui_boot_title) / sizeof(s_iap_ui_boot_title[0])),
                        WHITE,
                        IAP_UI_BOOT_BG_COLOR);
    iap_ui_draw_zh_text(subtitle_x,
                        IAP_UI_BOOT_SUBTITLE_Y,
                        s_iap_ui_boot_subtitle,
                        (uint8_t)(sizeof(s_iap_ui_boot_subtitle) / sizeof(s_iap_ui_boot_subtitle[0])),
                        IAP_UI_BOOT_MUTED_COLOR,
                        IAP_UI_BOOT_BG_COLOR);

    iap_ui_draw_hline(18U, IAP_UI_BOOT_SUBTITLE_LINE_Y, 34U, IAP_UI_BOOT_LINE_COLOR);
    iap_ui_draw_hline(104U, IAP_UI_BOOT_SUBTITLE_LINE_Y, 34U, IAP_UI_BOOT_LINE_COLOR);
}

static void iap_ui_draw_boot_header(void)
{
    uint16_t header_width = (uint16_t)(sizeof(s_iap_ui_boot_header) / sizeof(s_iap_ui_boot_header[0])) * 16U;
    uint16_t header_x = iap_ui_center_x_in_range(IAP_UI_BOOT_RIGHT_CONTENT_LEFT,
                                                 IAP_UI_BOOT_RIGHT_CONTENT_RIGHT,
                                                 header_width);
    uint16_t left_line_right = 0U;
    uint16_t right_line_left = 0U;

    iap_ui_draw_zh_text(header_x,
                        IAP_UI_BOOT_HEADER_Y,
                        s_iap_ui_boot_header,
                        (uint8_t)(sizeof(s_iap_ui_boot_header) / sizeof(s_iap_ui_boot_header[0])),
                        IAP_UI_BOOT_CYAN_COLOR,
                        IAP_UI_BOOT_BG_COLOR);

    left_line_right = (header_x > 10U) ? (uint16_t)(header_x - 10U) : IAP_UI_BOOT_RIGHT_CONTENT_LEFT;
    if (left_line_right > IAP_UI_BOOT_RIGHT_CONTENT_LEFT)
    {
        iap_ui_draw_hline(IAP_UI_BOOT_RIGHT_CONTENT_LEFT + 4U,
                          (uint16_t)(IAP_UI_BOOT_HEADER_Y + 8U),
                          (uint16_t)(left_line_right - IAP_UI_BOOT_RIGHT_CONTENT_LEFT - 3U),
                          IAP_UI_BOOT_LINE_COLOR);
    }

    right_line_left = (uint16_t)(header_x + header_width + 10U);
    if (right_line_left < IAP_UI_BOOT_RIGHT_CONTENT_RIGHT)
    {
        iap_ui_draw_hline(right_line_left,
                          (uint16_t)(IAP_UI_BOOT_HEADER_Y + 8U),
                          (uint16_t)(IAP_UI_BOOT_RIGHT_CONTENT_RIGHT - right_line_left - 3U),
                          IAP_UI_BOOT_LINE_COLOR);
    }
}

static void iap_ui_draw_boot_progress_track(void)
{
    iap_ui_fill_rect_inclusive(IAP_UI_BOOT_TRACK_LEFT,
                               IAP_UI_BOOT_TRACK_TOP,
                               IAP_UI_BOOT_TRACK_RIGHT,
                               IAP_UI_BOOT_TRACK_BOTTOM,
                               IAP_UI_BOOT_TRACK_BORDER_COLOR);
    iap_ui_fill_rect_inclusive((uint16_t)(IAP_UI_BOOT_TRACK_LEFT + 1U),
                               (uint16_t)(IAP_UI_BOOT_TRACK_TOP + 1U),
                               (uint16_t)(IAP_UI_BOOT_TRACK_RIGHT - 1U),
                               (uint16_t)(IAP_UI_BOOT_TRACK_BOTTOM - 1U),
                               IAP_UI_BOOT_TRACK_COLOR);
}

static void iap_ui_draw_boot_progress_label_once(void)
{
    static const char s_boot_percent_max_text[] = "100%";
    uint16_t label_width = 0U;
    uint16_t percent_width = 0U;
    uint16_t total_width = 0U;
    uint16_t x = 0U;

    label_width = (uint16_t)(sizeof(s_iap_ui_boot_progress_text) /
                             sizeof(s_iap_ui_boot_progress_text[0])) * 16U;
    percent_width = iap_ui_ascii_text_width(s_boot_percent_max_text, 12U);
    total_width = (uint16_t)(label_width + 8U + percent_width);
    x = iap_ui_center_x_in_range(IAP_UI_BOOT_RIGHT_CONTENT_LEFT,
                                 IAP_UI_BOOT_RIGHT_CONTENT_RIGHT,
                                 total_width);

    iap_ui_draw_zh_text(x,
                        IAP_UI_BOOT_PROGRESS_TEXT_Y,
                        s_iap_ui_boot_progress_text,
                        (uint8_t)(sizeof(s_iap_ui_boot_progress_text) / sizeof(s_iap_ui_boot_progress_text[0])),
                        WHITE,
                        IAP_UI_BOOT_BG_COLOR);

    s_boot_percent_value_x = (uint16_t)(x + label_width + 8U);
    s_boot_percent_value_y = (uint16_t)(IAP_UI_BOOT_PROGRESS_TEXT_Y + 2U);
    s_boot_percent_value_right = (uint16_t)(s_boot_percent_value_x + percent_width + 1U);
}

static void iap_ui_draw_boot_static_frame(void)
{
    LCD_Fill(0U, 0U, LCD_W, LCD_H, IAP_UI_BOOT_BG_COLOR);
    iap_ui_draw_vline(IAP_UI_BOOT_DIVIDER_X,
                      IAP_UI_BOOT_FRAME_TOP,
                      (uint16_t)(IAP_UI_BOOT_FRAME_BOTTOM - IAP_UI_BOOT_FRAME_TOP + 1U),
                      IAP_UI_BOOT_DIVIDER_COLOR);

    iap_ui_draw_boot_brand();
    iap_ui_draw_boot_header();
    iap_ui_draw_hline(IAP_UI_BOOT_RIGHT_CONTENT_LEFT + 4U, 98U, 128U, IAP_UI_BOOT_DIVIDER_COLOR);
    iap_ui_draw_hline(IAP_UI_BOOT_RIGHT_CONTENT_LEFT + 4U, 134U, 128U, IAP_UI_BOOT_DIVIDER_COLOR);
    iap_ui_draw_boot_progress_track();
    iap_ui_draw_boot_progress_label_once();
    iap_ui_draw_zh_text_scaled_center(IAP_UI_BOOT_HINT_Y,
                                      s_iap_ui_boot_hint,
                                      (uint8_t)(sizeof(s_iap_ui_boot_hint) / sizeof(s_iap_ui_boot_hint[0])),
                                      IAP_UI_BOOT_HINT_GLYPH_SIZE,
                                      IAP_UI_BOOT_MUTED_COLOR);

    s_boot_last_percent = 0xFFU;
    s_boot_last_step0 = (iap_ui_boot_step_state_t)0xFFU;
    s_boot_last_step1 = (iap_ui_boot_step_state_t)0xFFU;
    s_boot_last_step2 = (iap_ui_boot_step_state_t)0xFFU;
    s_boot_last_fill_right = 0xFFFFU;
}

static void iap_ui_draw_boot_step(uint16_t y,
                                  const uint16_t *label,
                                  uint8_t count,
                                  iap_ui_boot_step_state_t state)
{
    const uint8_t *icon = iap_ui_boot_status_pending_16;
    uint16_t icon_color = IAP_UI_BOOT_MUTED_COLOR;
    uint16_t text_color = IAP_UI_BOOT_MUTED_COLOR;

    iap_ui_fill_rect_inclusive(IAP_UI_BOOT_RIGHT_CONTENT_LEFT,
                               y,
                               IAP_UI_BOOT_STEP_CLEAR_RIGHT,
                               (uint16_t)(y + 17U),
                               IAP_UI_BOOT_BG_COLOR);

    if (state == IAP_UI_BOOT_STEP_DONE)
    {
        icon = iap_ui_boot_status_done_16;
        icon_color = IAP_UI_BOOT_CYAN_COLOR;
        text_color = WHITE;
    }
    else if (state == IAP_UI_BOOT_STEP_ACTIVE)
    {
        icon = iap_ui_boot_status_active_16;
        icon_color = IAP_UI_BOOT_ORANGE_MID_COLOR;
        text_color = WHITE;
    }

    iap_ui_draw_mono_bitmap_masked(IAP_UI_BOOT_STEP_ICON_X, y, icon, 16U, 16U, icon_color);
    iap_ui_draw_vline(IAP_UI_BOOT_STEP_BAR_X, (uint16_t)(y + 2U), 12U, IAP_UI_BOOT_LINE_COLOR);
    iap_ui_draw_zh_text(IAP_UI_BOOT_STEP_TEXT_X,
                        y,
                        label,
                        count,
                        text_color,
                        IAP_UI_BOOT_BG_COLOR);
}

static void iap_ui_draw_boot_progress_fill(uint8_t percent)
{
    uint16_t inner_left = (uint16_t)(IAP_UI_BOOT_TRACK_LEFT + 2U);
    uint16_t inner_top = (uint16_t)(IAP_UI_BOOT_TRACK_TOP + 2U);
    uint16_t inner_right = (uint16_t)(IAP_UI_BOOT_TRACK_RIGHT - 2U);
    uint16_t inner_bottom = (uint16_t)(IAP_UI_BOOT_TRACK_BOTTOM - 2U);
    uint16_t inner_width = 0U;
    uint16_t fill_width = 0U;
    uint16_t fill_right = 0U;
    uint16_t draw_left = 0U;

    if (percent > 100U)
    {
        percent = 100U;
    }

    inner_width = (uint16_t)(inner_right - inner_left + 1U);
    if (percent == 0U || inner_width == 0U)
    {
        return;
    }

    fill_width = (uint16_t)(((uint32_t)inner_width * percent) / 100UL);
    if (fill_width == 0U)
    {
        fill_width = 1U;
    }

    fill_right = (uint16_t)(inner_left + fill_width - 1U);
    if (fill_right > inner_right)
    {
        fill_right = inner_right;
    }

    if (s_boot_last_fill_right == 0xFFFFU ||
        s_boot_last_fill_right < inner_left ||
        s_boot_last_fill_right > inner_right)
    {
        draw_left = inner_left;
    }
    else if (fill_right > s_boot_last_fill_right)
    {
        draw_left = (uint16_t)(s_boot_last_fill_right + 1U);
    }
    else
    {
        return;
    }

    iap_ui_fill_rect_inclusive(draw_left,
                               inner_top,
                               fill_right,
                               inner_bottom,
                               IAP_UI_BOOT_ORANGE_MID_COLOR);

    if ((inner_top + 1U) <= inner_bottom)
    {
        iap_ui_fill_rect_inclusive(draw_left,
                                   inner_top,
                                   fill_right,
                                   (uint16_t)(inner_top + 1U),
                                   IAP_UI_BOOT_ORANGE_HI_COLOR);
    }

    s_boot_last_fill_right = fill_right;
}

static void iap_ui_draw_boot_percent_value(uint8_t percent)
{
    char percent_text[5];

    iap_ui_fill_rect_inclusive(s_boot_percent_value_x,
                               s_boot_percent_value_y,
                               s_boot_percent_value_right,
                               (uint16_t)(s_boot_percent_value_y + 11U),
                               IAP_UI_BOOT_BG_COLOR);

    iap_ui_format_percent_text(percent, percent_text);
    iap_ui_draw_ascii(s_boot_percent_value_x,
                      s_boot_percent_value_y,
                      percent_text,
                      IAP_UI_BOOT_ORANGE_MID_COLOR,
                      IAP_UI_BOOT_BG_COLOR,
                      12U);
}

static void iap_ui_update_normal_boot(uint16_t elapsed_ms)
{
    uint8_t percent = iap_ui_boot_progress_percent(elapsed_ms);
    iap_ui_boot_step_state_t step0 = iap_ui_boot_step_state(elapsed_ms, 0U);
    iap_ui_boot_step_state_t step1 = iap_ui_boot_step_state(elapsed_ms, 1U);
    iap_ui_boot_step_state_t step2 = iap_ui_boot_step_state(elapsed_ms, 2U);

    if (step0 != s_boot_last_step0)
    {
        iap_ui_draw_boot_step(IAP_UI_BOOT_STEP1_Y,
                              s_iap_ui_boot_step_fw,
                              (uint8_t)(sizeof(s_iap_ui_boot_step_fw) / sizeof(s_iap_ui_boot_step_fw[0])),
                              step0);
        s_boot_last_step0 = step0;
    }

    if (step1 != s_boot_last_step1)
    {
        iap_ui_draw_boot_step(IAP_UI_BOOT_STEP2_Y,
                              s_iap_ui_boot_step_verify,
                              (uint8_t)(sizeof(s_iap_ui_boot_step_verify) / sizeof(s_iap_ui_boot_step_verify[0])),
                              step1);
        s_boot_last_step1 = step1;
    }

    if (step2 != s_boot_last_step2)
    {
        iap_ui_draw_boot_step(IAP_UI_BOOT_STEP3_Y,
                              s_iap_ui_boot_step_ready,
                              (uint8_t)(sizeof(s_iap_ui_boot_step_ready) / sizeof(s_iap_ui_boot_step_ready[0])),
                              step2);
        s_boot_last_step2 = step2;
    }

    if (percent != s_boot_last_percent)
    {
        iap_ui_draw_boot_progress_fill(percent);
        iap_ui_draw_boot_percent_value(percent);
        s_boot_last_percent = percent;
    }

    iap_ui_present_if_needed();
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
    if (s_iap_ui_screen != IAP_UI_SCREEN_SPLASH)
    {
        iap_ui_draw_boot_static_frame();
        s_iap_ui_screen = IAP_UI_SCREEN_SPLASH;
    }

    iap_ui_update_normal_boot(0U);
}

void iap_ui_run_normal_boot_2s(uint8_t feed_watchdog)
{
    uint16_t elapsed_ms = 0U;

    iap_ui_show_boot_splash();

    while (elapsed_ms < IAP_BOOT_SPLASH_HOLD_MS)
    {
        if (feed_watchdog != 0U)
        {
            iap_feed_watchdog();
        }

        delay_ms((u16)IAP_UI_BOOT_STEP_TICK_MS);
        elapsed_ms = (uint16_t)(elapsed_ms + IAP_UI_BOOT_STEP_TICK_MS);
        if (elapsed_ms > IAP_BOOT_SPLASH_HOLD_MS)
        {
            elapsed_ms = IAP_BOOT_SPLASH_HOLD_MS;
        }

        iap_ui_update_normal_boot(elapsed_ms);
    }
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
