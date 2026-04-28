#include "iap_ui.h"

#include "lcd_init.h"
#include "iap_ui_boot_assets.h"


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

#define IAP_UI_OTA_BG_COLOR              IAP_UI_RGB565(12U, 14U, 18U)
#define IAP_UI_OTA_PANEL_COLOR           IAP_UI_RGB565(30U, 33U, 40U)
#define IAP_UI_OTA_PANEL_ALT_COLOR       IAP_UI_RGB565(37U, 40U, 48U)
#define IAP_UI_OTA_BORDER_COLOR          IAP_UI_RGB565(86U, 90U, 100U)
#define IAP_UI_OTA_LINE_COLOR            IAP_UI_RGB565(66U, 70U, 78U)
#define IAP_UI_OTA_TEXT_COLOR            WHITE
#define IAP_UI_OTA_MUTED_COLOR           IAP_UI_RGB565(170U, 174U, 184U)
#define IAP_UI_OTA_ACCENT_COLOR          IAP_UI_RGB565(255U, 176U, 54U)
#define IAP_UI_OTA_ACCENT_BG_COLOR       IAP_UI_RGB565(67U, 44U, 23U)
#define IAP_UI_OTA_SUCCESS_COLOR         IAP_UI_RGB565(142U, 216U, 92U)
#define IAP_UI_OTA_SUCCESS_BG_COLOR      IAP_UI_RGB565(31U, 54U, 27U)
#define IAP_UI_OTA_ERROR_COLOR           IAP_UI_RGB565(232U, 92U, 92U)
#define IAP_UI_OTA_ERROR_BG_COLOR        IAP_UI_RGB565(72U, 26U, 26U)
#define IAP_UI_OTA_TRACK_BG_COLOR        IAP_UI_RGB565(20U, 22U, 27U)

#define IAP_UI_OTA_HEADER_BOTTOM         38U
#define IAP_UI_OTA_ICON_X                12U
#define IAP_UI_OTA_ICON_Y                5U
#define IAP_UI_OTA_ICON_SIZE             28U
#define IAP_UI_OTA_TITLE_X               70U
#define IAP_UI_OTA_TITLE_Y               5U
#define IAP_UI_OTA_SUBTITLE_X            72U
#define IAP_UI_OTA_SUBTITLE_Y            22U
#define IAP_UI_OTA_TAG_LEFT              246U
#define IAP_UI_OTA_TAG_TOP               7U
#define IAP_UI_OTA_TAG_RIGHT             309U
#define IAP_UI_OTA_TAG_BOTTOM            28U

#define IAP_UI_OTA_INFO_LEFT             8U
#define IAP_UI_OTA_INFO_TOP              44U
#define IAP_UI_OTA_INFO_RIGHT            311U
#define IAP_UI_OTA_INFO_BOTTOM           90U
#define IAP_UI_OTA_INFO_DIVIDER_X        160U
#define IAP_UI_OTA_INFO_DIVIDER_Y        67U
#define IAP_UI_OTA_INFO_ROW1_Y           52U
#define IAP_UI_OTA_INFO_ROW2_Y           72U
#define IAP_UI_OTA_INFO_LEFT_LABEL_X     18U
#define IAP_UI_OTA_INFO_LEFT_VALUE_X     90U
#define IAP_UI_OTA_INFO_RIGHT_LABEL_X    172U
#define IAP_UI_OTA_INFO_RIGHT_VALUE_X    244U

#define IAP_UI_OTA_STEP_LEFT             8U
#define IAP_UI_OTA_STEP_TOP              96U
#define IAP_UI_OTA_STEP_RIGHT            311U
#define IAP_UI_OTA_STEP_BOTTOM           184U
#define IAP_UI_OTA_STEP_ROW0_Y           102U
#define IAP_UI_OTA_STEP_ROW_HEIGHT       16U
#define IAP_UI_OTA_STEP_TIMELINE_X       20U
#define IAP_UI_OTA_STEP_ICON_X           12U
#define IAP_UI_OTA_STEP_INDEX_X          34U
#define IAP_UI_OTA_STEP_LABEL_X          50U
#define IAP_UI_OTA_STEP_STATUS_X         246U
#define IAP_UI_OTA_STEP_CONTENT_LEFT     44U
#define IAP_UI_OTA_STEP_CONTENT_RIGHT    304U

#define IAP_UI_OTA_PROGRESS_LEFT         8U
#define IAP_UI_OTA_PROGRESS_TOP          188U
#define IAP_UI_OTA_PROGRESS_RIGHT        311U
#define IAP_UI_OTA_PROGRESS_BOTTOM       220U
#define IAP_UI_OTA_BAR_LEFT              18U
#define IAP_UI_OTA_BAR_TOP               195U
#define IAP_UI_OTA_BAR_RIGHT             252U
#define IAP_UI_OTA_BAR_BOTTOM            206U
#define IAP_UI_OTA_PERCENT_X             264U
#define IAP_UI_OTA_PERCENT_Y             193U
#define IAP_UI_OTA_DETAIL_Y              210U

#define IAP_UI_OTA_FOOTER_LEFT           8U
#define IAP_UI_OTA_FOOTER_TOP            223U
#define IAP_UI_OTA_FOOTER_RIGHT          311U
#define IAP_UI_OTA_FOOTER_BOTTOM         239U
#define IAP_UI_OTA_FOOTER_TEXT_Y         223U
#define IAP_UI_OTA_FOOTER_LEFT_X         14U
#define IAP_UI_OTA_FOOTER_CENTER_X       126U
#define IAP_UI_OTA_FOOTER_RIGHT_X        238U

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

typedef enum
{
    IAP_UI_OTA_STEP_PENDING = 0U,
    IAP_UI_OTA_STEP_ACTIVE = 1U,
    IAP_UI_OTA_STEP_DONE = 2U
} iap_ui_ota_step_state_t;

typedef enum
{
    IAP_UI_OTA_PHASE_HANDSHAKE = 0U,
    IAP_UI_OTA_PHASE_VERIFY = 1U,
    IAP_UI_OTA_PHASE_RECEIVE = 2U,
    IAP_UI_OTA_PHASE_WRITE = 3U,
    IAP_UI_OTA_PHASE_REBOOT = 4U
} iap_ui_ota_phase_t;

typedef enum
{
    IAP_UI_OTA_LINK_STABLE = 0U,
    IAP_UI_OTA_LINK_WAIT = 1U,
    IAP_UI_OTA_LINK_FAILURE = 2U
} iap_ui_ota_link_state_t;

typedef enum
{
    IAP_UI_OTA_OUTCOME_PROGRESS = 0U,
    IAP_UI_OTA_OUTCOME_SUCCESS = 1U,
    IAP_UI_OTA_OUTCOME_FAILURE = 2U
} iap_ui_ota_outcome_t;

typedef enum
{
    IAP_UI_OTA_DETAIL_MODE_WAITING = 0U,
    IAP_UI_OTA_DETAIL_MODE_PROGRESS = 1U,
    IAP_UI_OTA_DETAIL_MODE_FAILURE = 2U
} iap_ui_ota_detail_mode_t;

typedef struct
{
    uint8_t initialized;
    iap_ui_ota_phase_t phase;
    iap_ui_ota_phase_t last_phase;
    iap_ui_ota_outcome_t outcome;
    iap_ui_ota_outcome_t last_outcome;
    iap_ui_ota_link_state_t link_state;
    iap_ui_ota_link_state_t last_link_state;
    uint8_t percent;
    uint8_t last_percent;
    uint8_t source_device;
    uint8_t last_source_device;
    uint8_t target_partition;
    uint8_t last_target_partition;
    uint16_t error_code;
    uint16_t last_error_code;
    uint32_t current_value;
    uint32_t last_current_value;
    uint32_t total_value;
    uint32_t last_total_value;
    iap_ui_ota_detail_mode_t last_detail_mode;
    uint16_t progress_fill_right;
    uint16_t detail_value_x;
    uint16_t detail_value_right;
    iap_ui_ota_step_state_t step_states[5];
    iap_ui_ota_step_state_t last_step_states[5];
    char current_version[BOOT_INFO_VERSION_LEN];
    char target_version[BOOT_INFO_VERSION_LEN];
    char last_current_version[BOOT_INFO_VERSION_LEN];
    char last_target_version[BOOT_INFO_VERSION_LEN];
} iap_ui_ota_context_t;

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
static iap_ui_ota_context_t s_iap_ui_ota;

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

static const uint16_t s_iap_ui_ota_title[] = { 0x56FA, 0x4EF6, 0x5347, 0x7EA7, 0x6A21, 0x5F0F };
static const uint16_t s_iap_ui_ota_subtitle[] = { 0x5B89, 0x5168, 0x66F4, 0x65B0 };
static const uint16_t s_iap_ui_ota_source_label[] = { 0x6765, 0x6E90, 0x8BBE, 0x5907 };
static const uint16_t s_iap_ui_ota_target_label[] = { 0x76EE, 0x6807, 0x5206, 0x533A };
static const uint16_t s_iap_ui_ota_current_label[] = { 0x5F53, 0x524D, 0x7248, 0x672C };
static const uint16_t s_iap_ui_ota_package_label[] = { 0x5347, 0x7EA7, 0x5305 };
static const uint16_t s_iap_ui_ota_step_handshake[] = { 0x8BF7, 0x6C42, 0x786E, 0x8BA4 };
static const uint16_t s_iap_ui_ota_step_verify[] = { 0x56FA, 0x4EF6, 0x68C0, 0x67E5 };
static const uint16_t s_iap_ui_ota_step_receive[] = { 0x63A5, 0x6536, 0x56FA, 0x4EF6 };
static const uint16_t s_iap_ui_ota_step_write[] = { 0x5199, 0x5165, 0x5206, 0x533A };
static const uint16_t s_iap_ui_ota_step_reboot[] = { 0x5B8C, 0x6210, 0x91CD, 0x542F };
static const uint16_t s_iap_ui_ota_done[] = { 0x5DF2, 0x5B8C, 0x6210 };
static const uint16_t s_iap_ui_ota_waiting[] = { 0x7B49, 0x5F85, 0x4E2D };
static const uint16_t s_iap_ui_ota_received[] = { 0x5DF2, 0x63A5, 0x6536 };
static const uint16_t s_iap_ui_ota_no_power[] = { 0x8BF7, 0x52FF, 0x65AD, 0x7535 };
static const uint16_t s_iap_ui_ota_auto_reboot[] = { 0x81EA, 0x52A8, 0x91CD, 0x542F };
static const uint16_t s_iap_ui_ota_link_stable[] = { 0x94FE, 0x8DEF, 0x7A33, 0x5B9A };
static const uint16_t s_iap_ui_ota_link_fail[] = { 0x94FE, 0x8DEF, 0x5931, 0x8D25 };
static const uint16_t s_iap_ui_ota_return_system[] = { 0x8FD4, 0x56DE, 0x7CFB, 0x7EDF };

static const iap_ui_boot_keyframe_t s_iap_ui_boot_keyframes[] =
{
    { 0U, 0U },
    { 450U, 15U },
    { 1200U, 40U },
    { 1950U, 70U },
    { 2700U, 95U },
    { 3000U, 100U }
};

static void iap_ui_present_if_needed(void);

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

static void iap_ui_ascii_copy(char *target, uint16_t target_len, const char *source)
{
    uint16_t index = 0U;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    while (index < target_len)
    {
        target[index] = '\0';
        ++index;
    }

    if (source == 0)
    {
        return;
    }

    index = 0U;
    while (index + 1U < target_len && source[index] != '\0')
    {
        target[index] = source[index];
        ++index;
    }
}

static void iap_ui_ascii_copy_bytes(char *target,
                                    uint16_t target_len,
                                    const uint8_t *source,
                                    uint16_t source_len)
{
    uint16_t index = 0U;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    while (index < target_len)
    {
        target[index] = '\0';
        ++index;
    }

    if (source == 0)
    {
        return;
    }

    for (index = 0U; index + 1U < target_len && index < source_len && source[index] != '\0'; ++index)
    {
        target[index] = (char)source[index];
    }
}

static uint8_t iap_ui_ascii_equal(const char *left, const char *right)
{
    if (left == 0 || right == 0)
    {
        return (left == right) ? 1U : 0U;
    }

    while (*left != '\0' || *right != '\0')
    {
        if (*left != *right)
        {
            return 0U;
        }

        ++left;
        ++right;
    }

    return 1U;
}

static uint16_t iap_ui_ascii_append_text(char *buffer,
                                         uint16_t cursor,
                                         uint16_t buffer_len,
                                         const char *text)
{
    if (buffer == 0 || buffer_len == 0U || cursor >= buffer_len)
    {
        return cursor;
    }

    if (text == 0)
    {
        buffer[cursor] = '\0';
        return cursor;
    }

    while (*text != '\0' && cursor + 1U < buffer_len)
    {
        buffer[cursor++] = *text++;
    }

    buffer[cursor] = '\0';
    return cursor;
}

static uint16_t iap_ui_ascii_append_u32(char *buffer,
                                        uint16_t cursor,
                                        uint16_t buffer_len,
                                        uint32_t value)
{
    char digits[10];
    uint8_t count = 0U;

    if (buffer == 0 || buffer_len == 0U || cursor >= buffer_len)
    {
        return cursor;
    }

    if (value == 0U)
    {
        if (cursor + 1U < buffer_len)
        {
            buffer[cursor++] = '0';
            buffer[cursor] = '\0';
        }
        return cursor;
    }

    while (value > 0U && count < sizeof(digits))
    {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    while (count > 0U && cursor + 1U < buffer_len)
    {
        buffer[cursor++] = digits[--count];
    }

    buffer[cursor] = '\0';
    return cursor;
}

static void iap_ui_format_hex16(uint16_t value, char out[7])
{
    static const char hex_digits[] = "0123456789ABCDEF";

    out[0] = '0';
    out[1] = 'x';
    out[2] = hex_digits[(value >> 12) & 0x0FU];
    out[3] = hex_digits[(value >> 8) & 0x0FU];
    out[4] = hex_digits[(value >> 4) & 0x0FU];
    out[5] = hex_digits[value & 0x0FU];
    out[6] = '\0';
}

static uint32_t iap_ui_bytes_to_kb(uint32_t value)
{
    if (value == 0U)
    {
        return 0U;
    }

    return (value + 1023U) / 1024U;
}

static void iap_ui_format_kb_detail(char out[28], uint32_t current_value, uint32_t total_value)
{
    uint16_t cursor = 0U;

    out[0] = '\0';
    cursor = iap_ui_ascii_append_u32(out, 0U, 28U, iap_ui_bytes_to_kb(current_value));
    cursor = iap_ui_ascii_append_text(out, cursor, 28U, " KB / ");
    cursor = iap_ui_ascii_append_u32(out, cursor, 28U, iap_ui_bytes_to_kb(total_value));
    (void)iap_ui_ascii_append_text(out, cursor, 28U, " KB");
}

static void iap_ui_format_error_detail(char out[24], uint16_t error_code)
{
    char hex_text[7];
    uint16_t cursor = 0U;

    iap_ui_format_hex16(error_code, hex_text);
    cursor = iap_ui_ascii_append_text(out, 0U, 24U, "ERR ");
    (void)iap_ui_ascii_append_text(out, cursor, 24U, hex_text);
}

static const char *iap_ui_target_partition_text(uint8_t partition)
{
    if (partition == OTA_CTRL_PARTITION_APP1)
    {
        return "APP1";
    }

    if (partition == OTA_CTRL_PARTITION_APP2)
    {
        return "APP2";
    }

    return "--";
}

static uint16_t iap_ui_ota_status_x(const uint16_t *status_text, uint8_t count)
{
    uint16_t width = (uint16_t)(count * 16U);

    if (IAP_UI_OTA_STEP_CONTENT_RIGHT <= width)
    {
        return IAP_UI_OTA_STEP_STATUS_X;
    }

    return (uint16_t)(IAP_UI_OTA_STEP_CONTENT_RIGHT - width);
}

static const uint16_t *iap_ui_ota_footer_left_text(iap_ui_ota_outcome_t outcome,
                                                   uint8_t *count)
{
    if (outcome == IAP_UI_OTA_OUTCOME_SUCCESS)
    {
        *count = (uint8_t)(sizeof(s_iap_ui_status_success) / sizeof(s_iap_ui_status_success[0]));
        return s_iap_ui_status_success;
    }

    if (outcome == IAP_UI_OTA_OUTCOME_FAILURE)
    {
        *count = (uint8_t)(sizeof(s_iap_ui_status_failure) / sizeof(s_iap_ui_status_failure[0]));
        return s_iap_ui_status_failure;
    }

    *count = (uint8_t)(sizeof(s_iap_ui_ota_no_power) / sizeof(s_iap_ui_ota_no_power[0]));
    return s_iap_ui_ota_no_power;
}

static const uint16_t *iap_ui_ota_footer_center_text(iap_ui_ota_outcome_t outcome,
                                                     uint8_t *count)
{
    if (outcome == IAP_UI_OTA_OUTCOME_FAILURE)
    {
        *count = (uint8_t)(sizeof(s_iap_ui_ota_return_system) / sizeof(s_iap_ui_ota_return_system[0]));
        return s_iap_ui_ota_return_system;
    }

    *count = (uint8_t)(sizeof(s_iap_ui_ota_auto_reboot) / sizeof(s_iap_ui_ota_auto_reboot[0]));
    return s_iap_ui_ota_auto_reboot;
}

static const uint16_t *iap_ui_ota_footer_right_text(iap_ui_ota_link_state_t link_state,
                                                    uint8_t *count)
{
    if (link_state == IAP_UI_OTA_LINK_FAILURE)
    {
        *count = (uint8_t)(sizeof(s_iap_ui_ota_link_fail) / sizeof(s_iap_ui_ota_link_fail[0]));
        return s_iap_ui_ota_link_fail;
    }

    *count = (uint8_t)(sizeof(s_iap_ui_ota_link_stable) / sizeof(s_iap_ui_ota_link_stable[0]));
    return s_iap_ui_ota_link_stable;
}

static uint16_t iap_ui_ota_status_color(iap_ui_ota_step_state_t state,
                                        iap_ui_ota_outcome_t outcome)
{
    if (state == IAP_UI_OTA_STEP_DONE)
    {
        return IAP_UI_OTA_SUCCESS_COLOR;
    }

    if (state == IAP_UI_OTA_STEP_ACTIVE)
    {
        return (outcome == IAP_UI_OTA_OUTCOME_FAILURE) ? IAP_UI_OTA_ERROR_COLOR :
                                                         IAP_UI_OTA_ACCENT_COLOR;
    }

    return IAP_UI_OTA_MUTED_COLOR;
}

static uint16_t iap_ui_ota_active_bg_color(iap_ui_ota_outcome_t outcome)
{
    return (outcome == IAP_UI_OTA_OUTCOME_FAILURE) ? IAP_UI_OTA_ERROR_BG_COLOR :
                                                     IAP_UI_OTA_ACCENT_BG_COLOR;
}

static uint16_t iap_ui_ota_progress_color(iap_ui_ota_outcome_t outcome)
{
    if (outcome == IAP_UI_OTA_OUTCOME_SUCCESS)
    {
        return IAP_UI_OTA_SUCCESS_COLOR;
    }

    if (outcome == IAP_UI_OTA_OUTCOME_FAILURE)
    {
        return IAP_UI_OTA_ERROR_COLOR;
    }

    return IAP_UI_OTA_ACCENT_COLOR;
}

static void iap_ui_ota_apply_phase_states(void)
{
    uint8_t index = 0U;
    uint8_t active_index = 0U;

    switch (s_iap_ui_ota.phase)
    {
    case IAP_UI_OTA_PHASE_VERIFY:
        active_index = 1U;
        break;
    case IAP_UI_OTA_PHASE_RECEIVE:
        active_index = 2U;
        break;
    case IAP_UI_OTA_PHASE_WRITE:
        active_index = 3U;
        break;
    case IAP_UI_OTA_PHASE_REBOOT:
        active_index = 4U;
        break;
    case IAP_UI_OTA_PHASE_HANDSHAKE:
    default:
        active_index = 0U;
        break;
    }

    for (index = 0U; index < 5U; ++index)
    {
        if (s_iap_ui_ota.outcome == IAP_UI_OTA_OUTCOME_SUCCESS)
        {
            s_iap_ui_ota.step_states[index] = IAP_UI_OTA_STEP_DONE;
        }
        else if (index < active_index)
        {
            s_iap_ui_ota.step_states[index] = IAP_UI_OTA_STEP_DONE;
        }
        else if (index == active_index)
        {
            s_iap_ui_ota.step_states[index] = IAP_UI_OTA_STEP_ACTIVE;
        }
        else
        {
            s_iap_ui_ota.step_states[index] = IAP_UI_OTA_STEP_PENDING;
        }
    }
}

static void iap_ui_ota_reset_context(void)
{
    memset(&s_iap_ui_ota, 0, sizeof(s_iap_ui_ota));
    s_iap_ui_ota.initialized = 0U;
    s_iap_ui_ota.phase = IAP_UI_OTA_PHASE_HANDSHAKE;
    s_iap_ui_ota.last_phase = (iap_ui_ota_phase_t)0xFFU;
    s_iap_ui_ota.outcome = IAP_UI_OTA_OUTCOME_PROGRESS;
    s_iap_ui_ota.last_outcome = (iap_ui_ota_outcome_t)0xFFU;
    s_iap_ui_ota.link_state = IAP_UI_OTA_LINK_STABLE;
    s_iap_ui_ota.last_link_state = (iap_ui_ota_link_state_t)0xFFU;
    s_iap_ui_ota.percent = 0U;
    s_iap_ui_ota.last_percent = 0xFFU;
    s_iap_ui_ota.source_device = 1U;
    s_iap_ui_ota.last_source_device = 0xFFU;
    s_iap_ui_ota.target_partition = 0xFFU;
    s_iap_ui_ota.last_target_partition = 0xFFU;
    s_iap_ui_ota.error_code = 0U;
    s_iap_ui_ota.last_error_code = 0xFFFFU;
    s_iap_ui_ota.last_detail_mode = (iap_ui_ota_detail_mode_t)0xFFU;
    s_iap_ui_ota.progress_fill_right = 0xFFFFU;
    s_iap_ui_ota.detail_value_x = 0U;
    s_iap_ui_ota.detail_value_right = 0U;
    memset(s_iap_ui_ota.last_step_states, 0xFF, sizeof(s_iap_ui_ota.last_step_states));
    iap_ui_ascii_copy(s_iap_ui_ota.current_version, sizeof(s_iap_ui_ota.current_version), "--");
    iap_ui_ascii_copy(s_iap_ui_ota.target_version, sizeof(s_iap_ui_ota.target_version), "--");
    iap_ui_ascii_copy(s_iap_ui_ota.last_current_version, sizeof(s_iap_ui_ota.last_current_version), "");
    iap_ui_ascii_copy(s_iap_ui_ota.last_target_version, sizeof(s_iap_ui_ota.last_target_version), "");
    iap_ui_ota_apply_phase_states();
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
        return (elapsed_ms < 1200U) ? IAP_UI_BOOT_STEP_ACTIVE : IAP_UI_BOOT_STEP_DONE;
    }

    if (step_index == 1U)
    {
        if (elapsed_ms < 1200U)
        {
            return IAP_UI_BOOT_STEP_PENDING;
        }

        return (elapsed_ms < 1950U) ? IAP_UI_BOOT_STEP_ACTIVE : IAP_UI_BOOT_STEP_DONE;
    }

    if (elapsed_ms < 1950U)
    {
        return IAP_UI_BOOT_STEP_PENDING;
    }

    return (elapsed_ms < 2850U) ? IAP_UI_BOOT_STEP_ACTIVE : IAP_UI_BOOT_STEP_DONE;
}

static uint8_t iap_ui_ota_screen_active(void)
{
    return (s_iap_ui_screen == IAP_UI_SCREEN_PREPARE ||
            s_iap_ui_screen == IAP_UI_SCREEN_PROGRESS ||
            s_iap_ui_screen == IAP_UI_SCREEN_SUCCESS ||
            s_iap_ui_screen == IAP_UI_SCREEN_FAILURE) ? 1U : 0U;
}

static void iap_ui_draw_ota_header_static(void)
{
    LCD_Fill(0U, 0U, LCD_W, (u16)(IAP_UI_OTA_HEADER_BOTTOM + 1U), IAP_UI_OTA_BG_COLOR);
    iap_ui_draw_mono_bitmap_scaled_masked(IAP_UI_OTA_ICON_X,
                                          IAP_UI_OTA_ICON_Y,
                                          iap_ui_boot_logo_blue_56,
                                          56U,
                                          56U,
                                          IAP_UI_OTA_ICON_SIZE,
                                          IAP_UI_OTA_ICON_SIZE,
                                          IAP_UI_BOOT_MUTED_COLOR);
    iap_ui_draw_mono_bitmap_scaled_masked(IAP_UI_OTA_ICON_X,
                                          IAP_UI_OTA_ICON_Y,
                                          iap_ui_boot_logo_orange_56,
                                          56U,
                                          56U,
                                          IAP_UI_OTA_ICON_SIZE,
                                          IAP_UI_OTA_ICON_SIZE,
                                          IAP_UI_OTA_ACCENT_COLOR);
    iap_ui_draw_zh_text(IAP_UI_OTA_TITLE_X,
                        IAP_UI_OTA_TITLE_Y,
                        s_iap_ui_ota_title,
                        (uint8_t)(sizeof(s_iap_ui_ota_title) / sizeof(s_iap_ui_ota_title[0])),
                        IAP_UI_OTA_TEXT_COLOR,
                        IAP_UI_OTA_BG_COLOR);
    iap_ui_draw_zh_text(IAP_UI_OTA_SUBTITLE_X,
                        IAP_UI_OTA_SUBTITLE_Y,
                        s_iap_ui_ota_subtitle,
                        (uint8_t)(sizeof(s_iap_ui_ota_subtitle) / sizeof(s_iap_ui_ota_subtitle[0])),
                        IAP_UI_OTA_MUTED_COLOR,
                        IAP_UI_OTA_BG_COLOR);
    iap_ui_draw_ascii((uint16_t)(IAP_UI_OTA_SUBTITLE_X + 72U),
                      IAP_UI_OTA_SUBTITLE_Y + 2U,
                      "OTA",
                      IAP_UI_OTA_ACCENT_COLOR,
                      IAP_UI_OTA_BG_COLOR,
                      12U);

    LCD_DrawRectangle(IAP_UI_OTA_TAG_LEFT,
                      IAP_UI_OTA_TAG_TOP,
                      IAP_UI_OTA_TAG_RIGHT,
                      IAP_UI_OTA_TAG_BOTTOM,
                      IAP_UI_OTA_ACCENT_COLOR);
    iap_ui_draw_ascii((uint16_t)(IAP_UI_OTA_TAG_LEFT + 7U),
                      (uint16_t)(IAP_UI_OTA_TAG_TOP + 5U),
                      "BootLoader",
                      IAP_UI_OTA_ACCENT_COLOR,
                      IAP_UI_OTA_BG_COLOR,
                      12U);

    iap_ui_draw_hline(0U,
                      (uint16_t)(IAP_UI_OTA_HEADER_BOTTOM + 1U),
                      LCD_W,
                      IAP_UI_OTA_ACCENT_COLOR);
}

static void iap_ui_draw_ota_info_static(void)
{
    iap_ui_fill_rect_inclusive(IAP_UI_OTA_INFO_LEFT,
                               IAP_UI_OTA_INFO_TOP,
                               IAP_UI_OTA_INFO_RIGHT,
                               IAP_UI_OTA_INFO_BOTTOM,
                               IAP_UI_OTA_PANEL_COLOR);
    LCD_DrawRectangle(IAP_UI_OTA_INFO_LEFT,
                      IAP_UI_OTA_INFO_TOP,
                      IAP_UI_OTA_INFO_RIGHT,
                      IAP_UI_OTA_INFO_BOTTOM,
                      IAP_UI_OTA_BORDER_COLOR);
    iap_ui_draw_vline(IAP_UI_OTA_INFO_DIVIDER_X,
                      (uint16_t)(IAP_UI_OTA_INFO_TOP + 6U),
                      34U,
                      IAP_UI_OTA_LINE_COLOR);
    iap_ui_draw_hline((uint16_t)(IAP_UI_OTA_INFO_LEFT + 10U),
                      IAP_UI_OTA_INFO_DIVIDER_Y,
                      284U,
                      IAP_UI_OTA_LINE_COLOR);

    iap_ui_draw_zh_text(IAP_UI_OTA_INFO_LEFT_LABEL_X,
                        IAP_UI_OTA_INFO_ROW1_Y,
                        s_iap_ui_ota_source_label,
                        (uint8_t)(sizeof(s_iap_ui_ota_source_label) / sizeof(s_iap_ui_ota_source_label[0])),
                        IAP_UI_OTA_MUTED_COLOR,
                        IAP_UI_OTA_PANEL_COLOR);
    iap_ui_draw_zh_text(IAP_UI_OTA_INFO_RIGHT_LABEL_X,
                        IAP_UI_OTA_INFO_ROW1_Y,
                        s_iap_ui_ota_target_label,
                        (uint8_t)(sizeof(s_iap_ui_ota_target_label) / sizeof(s_iap_ui_ota_target_label[0])),
                        IAP_UI_OTA_MUTED_COLOR,
                        IAP_UI_OTA_PANEL_COLOR);
    iap_ui_draw_zh_text(IAP_UI_OTA_INFO_LEFT_LABEL_X,
                        IAP_UI_OTA_INFO_ROW2_Y,
                        s_iap_ui_ota_current_label,
                        (uint8_t)(sizeof(s_iap_ui_ota_current_label) / sizeof(s_iap_ui_ota_current_label[0])),
                        IAP_UI_OTA_MUTED_COLOR,
                        IAP_UI_OTA_PANEL_COLOR);
    iap_ui_draw_zh_text(IAP_UI_OTA_INFO_RIGHT_LABEL_X,
                        IAP_UI_OTA_INFO_ROW2_Y,
                        s_iap_ui_ota_package_label,
                        (uint8_t)(sizeof(s_iap_ui_ota_package_label) / sizeof(s_iap_ui_ota_package_label[0])),
                        IAP_UI_OTA_MUTED_COLOR,
                        IAP_UI_OTA_PANEL_COLOR);
}

static void iap_ui_draw_ota_step_panel_static(void)
{
    uint8_t index = 0U;

    iap_ui_fill_rect_inclusive(IAP_UI_OTA_STEP_LEFT,
                               IAP_UI_OTA_STEP_TOP,
                               IAP_UI_OTA_STEP_RIGHT,
                               IAP_UI_OTA_STEP_BOTTOM,
                               IAP_UI_OTA_PANEL_COLOR);
    LCD_DrawRectangle(IAP_UI_OTA_STEP_LEFT,
                      IAP_UI_OTA_STEP_TOP,
                      IAP_UI_OTA_STEP_RIGHT,
                      IAP_UI_OTA_STEP_BOTTOM,
                      IAP_UI_OTA_BORDER_COLOR);
    iap_ui_draw_vline(IAP_UI_OTA_STEP_TIMELINE_X,
                      (uint16_t)(IAP_UI_OTA_STEP_ROW0_Y + 8U),
                      66U,
                      IAP_UI_OTA_LINE_COLOR);

    for (index = 1U; index < 5U; ++index)
    {
        iap_ui_draw_hline((uint16_t)(IAP_UI_OTA_STEP_CONTENT_LEFT + 4U),
                          (uint16_t)(IAP_UI_OTA_STEP_ROW0_Y + (uint16_t)(index * IAP_UI_OTA_STEP_ROW_HEIGHT) - 2U),
                          252U,
                          IAP_UI_OTA_LINE_COLOR);
    }
}

static void iap_ui_draw_ota_progress_static(void)
{
    iap_ui_fill_rect_inclusive(IAP_UI_OTA_PROGRESS_LEFT,
                               IAP_UI_OTA_PROGRESS_TOP,
                               IAP_UI_OTA_PROGRESS_RIGHT,
                               IAP_UI_OTA_PROGRESS_BOTTOM,
                               IAP_UI_OTA_PANEL_COLOR);
    LCD_DrawRectangle(IAP_UI_OTA_PROGRESS_LEFT,
                      IAP_UI_OTA_PROGRESS_TOP,
                      IAP_UI_OTA_PROGRESS_RIGHT,
                      IAP_UI_OTA_PROGRESS_BOTTOM,
                      IAP_UI_OTA_BORDER_COLOR);
    iap_ui_fill_rect_inclusive(IAP_UI_OTA_BAR_LEFT,
                               IAP_UI_OTA_BAR_TOP,
                               IAP_UI_OTA_BAR_RIGHT,
                               IAP_UI_OTA_BAR_BOTTOM,
                               IAP_UI_OTA_BORDER_COLOR);
    iap_ui_fill_rect_inclusive((uint16_t)(IAP_UI_OTA_BAR_LEFT + 1U),
                               (uint16_t)(IAP_UI_OTA_BAR_TOP + 1U),
                               (uint16_t)(IAP_UI_OTA_BAR_RIGHT - 1U),
                               (uint16_t)(IAP_UI_OTA_BAR_BOTTOM - 1U),
                               IAP_UI_OTA_TRACK_BG_COLOR);
}

static void iap_ui_draw_ota_footer_static(void)
{
    iap_ui_fill_rect_inclusive(IAP_UI_OTA_FOOTER_LEFT,
                               IAP_UI_OTA_FOOTER_TOP,
                               IAP_UI_OTA_FOOTER_RIGHT,
                               IAP_UI_OTA_FOOTER_BOTTOM,
                               IAP_UI_OTA_PANEL_COLOR);
    LCD_DrawRectangle(IAP_UI_OTA_FOOTER_LEFT,
                      IAP_UI_OTA_FOOTER_TOP,
                      IAP_UI_OTA_FOOTER_RIGHT,
                      IAP_UI_OTA_FOOTER_BOTTOM,
                      IAP_UI_OTA_BORDER_COLOR);
}

static void iap_ui_draw_upgrade_frame(void)
{
    LCD_Fill(0U, 0U, LCD_W, LCD_H, IAP_UI_OTA_BG_COLOR);
    iap_ui_draw_ota_header_static();
    iap_ui_draw_ota_info_static();
    iap_ui_draw_ota_step_panel_static();
    iap_ui_draw_ota_progress_static();
    iap_ui_draw_ota_footer_static();

    s_iap_ui_ota.last_phase = (iap_ui_ota_phase_t)0xFFU;
    s_iap_ui_ota.last_outcome = (iap_ui_ota_outcome_t)0xFFU;
    s_iap_ui_ota.last_link_state = (iap_ui_ota_link_state_t)0xFFU;
    s_iap_ui_ota.last_percent = 0xFFU;
    s_iap_ui_ota.last_source_device = 0xFFU;
    s_iap_ui_ota.last_target_partition = 0xFFU;
    s_iap_ui_ota.last_current_value = 0xFFFFFFFFUL;
    s_iap_ui_ota.last_total_value = 0xFFFFFFFFUL;
    s_iap_ui_ota.last_error_code = 0xFFFFU;
    s_iap_ui_ota.last_detail_mode = (iap_ui_ota_detail_mode_t)0xFFU;
    s_iap_ui_ota.progress_fill_right = 0xFFFFU;
    s_iap_ui_ota.detail_value_x = 0U;
    s_iap_ui_ota.detail_value_right = 0U;
    memset(s_iap_ui_ota.last_step_states, 0xFF, sizeof(s_iap_ui_ota.last_step_states));
    iap_ui_ascii_copy(s_iap_ui_ota.last_current_version, sizeof(s_iap_ui_ota.last_current_version), "");
    iap_ui_ascii_copy(s_iap_ui_ota.last_target_version, sizeof(s_iap_ui_ota.last_target_version), "");
}

static void iap_ui_ota_draw_info_values(void)
{
    const char *source_text = (s_iap_ui_ota.source_device != 0U) ? "ESP32" : "--";
    const char *partition_text = iap_ui_target_partition_text(s_iap_ui_ota.target_partition);

    if (s_iap_ui_ota.source_device != s_iap_ui_ota.last_source_device ||
        s_iap_ui_ota.target_partition != s_iap_ui_ota.last_target_partition ||
        iap_ui_ascii_equal(s_iap_ui_ota.current_version, s_iap_ui_ota.last_current_version) == 0U ||
        iap_ui_ascii_equal(s_iap_ui_ota.target_version, s_iap_ui_ota.last_target_version) == 0U)
    {
        iap_ui_fill_rect_inclusive(IAP_UI_OTA_INFO_LEFT_VALUE_X,
                                   IAP_UI_OTA_INFO_ROW1_Y,
                                   146U,
                                   (uint16_t)(IAP_UI_OTA_INFO_ROW1_Y + 15U),
                                   IAP_UI_OTA_PANEL_COLOR);
        iap_ui_fill_rect_inclusive(IAP_UI_OTA_INFO_RIGHT_VALUE_X,
                                   IAP_UI_OTA_INFO_ROW1_Y,
                                   300U,
                                   (uint16_t)(IAP_UI_OTA_INFO_ROW1_Y + 15U),
                                   IAP_UI_OTA_PANEL_COLOR);
        iap_ui_fill_rect_inclusive(IAP_UI_OTA_INFO_LEFT_VALUE_X,
                                   IAP_UI_OTA_INFO_ROW2_Y,
                                   146U,
                                   (uint16_t)(IAP_UI_OTA_INFO_ROW2_Y + 15U),
                                   IAP_UI_OTA_PANEL_COLOR);
        iap_ui_fill_rect_inclusive(IAP_UI_OTA_INFO_RIGHT_VALUE_X,
                                   IAP_UI_OTA_INFO_ROW2_Y,
                                   300U,
                                   (uint16_t)(IAP_UI_OTA_INFO_ROW2_Y + 15U),
                                   IAP_UI_OTA_PANEL_COLOR);

        iap_ui_draw_ascii(IAP_UI_OTA_INFO_LEFT_VALUE_X,
                          IAP_UI_OTA_INFO_ROW1_Y,
                          source_text,
                          IAP_UI_OTA_ACCENT_COLOR,
                          IAP_UI_OTA_PANEL_COLOR,
                          16U);
        iap_ui_draw_ascii(IAP_UI_OTA_INFO_RIGHT_VALUE_X,
                          IAP_UI_OTA_INFO_ROW1_Y,
                          partition_text,
                          IAP_UI_OTA_ACCENT_COLOR,
                          IAP_UI_OTA_PANEL_COLOR,
                          16U);
        iap_ui_draw_ascii(IAP_UI_OTA_INFO_LEFT_VALUE_X,
                          IAP_UI_OTA_INFO_ROW2_Y,
                          s_iap_ui_ota.current_version,
                          IAP_UI_OTA_ACCENT_COLOR,
                          IAP_UI_OTA_PANEL_COLOR,
                          16U);
        iap_ui_draw_ascii(IAP_UI_OTA_INFO_RIGHT_VALUE_X,
                          IAP_UI_OTA_INFO_ROW2_Y,
                          s_iap_ui_ota.target_version,
                          IAP_UI_OTA_ACCENT_COLOR,
                          IAP_UI_OTA_PANEL_COLOR,
                          16U);

        s_iap_ui_ota.last_source_device = s_iap_ui_ota.source_device;
        s_iap_ui_ota.last_target_partition = s_iap_ui_ota.target_partition;
        iap_ui_ascii_copy(s_iap_ui_ota.last_current_version,
                          sizeof(s_iap_ui_ota.last_current_version),
                          s_iap_ui_ota.current_version);
        iap_ui_ascii_copy(s_iap_ui_ota.last_target_version,
                          sizeof(s_iap_ui_ota.last_target_version),
                          s_iap_ui_ota.target_version);
    }
}

static void iap_ui_ota_draw_step_row(uint8_t index)
{
    const uint16_t *label = s_iap_ui_ota_step_handshake;
    uint8_t label_count = 4U;
    const uint16_t *status = s_iap_ui_ota_waiting;
    uint8_t status_count = 3U;
    uint16_t row_y = (uint16_t)(IAP_UI_OTA_STEP_ROW0_Y + ((uint16_t)index * IAP_UI_OTA_STEP_ROW_HEIGHT));
    uint16_t fill_color = IAP_UI_OTA_PANEL_COLOR;
    uint16_t text_color = IAP_UI_OTA_MUTED_COLOR;
    uint16_t status_color = IAP_UI_OTA_MUTED_COLOR;
    const uint8_t *icon = iap_ui_boot_status_pending_16;
    char index_text[2];

    switch (index)
    {
    case 1U:
        label = s_iap_ui_ota_step_verify;
        label_count = (uint8_t)(sizeof(s_iap_ui_ota_step_verify) / sizeof(s_iap_ui_ota_step_verify[0]));
        break;
    case 2U:
        label = s_iap_ui_ota_step_receive;
        label_count = (uint8_t)(sizeof(s_iap_ui_ota_step_receive) / sizeof(s_iap_ui_ota_step_receive[0]));
        break;
    case 3U:
        label = s_iap_ui_ota_step_write;
        label_count = (uint8_t)(sizeof(s_iap_ui_ota_step_write) / sizeof(s_iap_ui_ota_step_write[0]));
        break;
    case 4U:
        label = s_iap_ui_ota_step_reboot;
        label_count = (uint8_t)(sizeof(s_iap_ui_ota_step_reboot) / sizeof(s_iap_ui_ota_step_reboot[0]));
        break;
    default:
        break;
    }

    if (s_iap_ui_ota.step_states[index] == IAP_UI_OTA_STEP_DONE)
    {
        icon = iap_ui_boot_status_done_16;
        status = s_iap_ui_ota_done;
        status_count = 3U;
        text_color = IAP_UI_OTA_TEXT_COLOR;
        status_color = IAP_UI_OTA_SUCCESS_COLOR;
    }
    else if (s_iap_ui_ota.step_states[index] == IAP_UI_OTA_STEP_ACTIVE)
    {
        icon = iap_ui_boot_status_active_16;
        status = (s_iap_ui_ota.outcome == IAP_UI_OTA_OUTCOME_FAILURE) ?
                     s_iap_ui_status_failure :
                     s_iap_ui_status_running;
        status_count = (uint8_t)((s_iap_ui_ota.outcome == IAP_UI_OTA_OUTCOME_FAILURE) ?
                                     (sizeof(s_iap_ui_status_failure) / sizeof(s_iap_ui_status_failure[0])) :
                                     (sizeof(s_iap_ui_status_running) / sizeof(s_iap_ui_status_running[0])));
        fill_color = iap_ui_ota_active_bg_color(s_iap_ui_ota.outcome);
        text_color = IAP_UI_OTA_TEXT_COLOR;
        status_color = iap_ui_ota_status_color(IAP_UI_OTA_STEP_ACTIVE, s_iap_ui_ota.outcome);
    }

    iap_ui_fill_rect_inclusive((uint16_t)(IAP_UI_OTA_STEP_LEFT + 1U),
                               row_y,
                               (uint16_t)(IAP_UI_OTA_STEP_RIGHT - 1U),
                               (uint16_t)(row_y + IAP_UI_OTA_STEP_ROW_HEIGHT - 1U),
                               IAP_UI_OTA_PANEL_COLOR);
    if (fill_color != IAP_UI_OTA_PANEL_COLOR)
    {
        iap_ui_fill_rect_inclusive(IAP_UI_OTA_STEP_CONTENT_LEFT,
                                   row_y,
                                   IAP_UI_OTA_STEP_CONTENT_RIGHT,
                                   (uint16_t)(row_y + IAP_UI_OTA_STEP_ROW_HEIGHT - 1U),
                                   fill_color);
        LCD_DrawRectangle(IAP_UI_OTA_STEP_CONTENT_LEFT,
                          row_y,
                          IAP_UI_OTA_STEP_CONTENT_RIGHT,
                          (uint16_t)(row_y + IAP_UI_OTA_STEP_ROW_HEIGHT - 1U),
                          status_color);
    }

    if (index < 4U)
    {
        iap_ui_draw_hline((uint16_t)(IAP_UI_OTA_STEP_CONTENT_LEFT + 4U),
                          (uint16_t)(row_y + IAP_UI_OTA_STEP_ROW_HEIGHT),
                          252U,
                          IAP_UI_OTA_LINE_COLOR);
    }

    iap_ui_draw_mono_bitmap_masked(IAP_UI_OTA_STEP_ICON_X,
                                   row_y,
                                   icon,
                                   16U,
                                   16U,
                                   iap_ui_ota_status_color(s_iap_ui_ota.step_states[index], s_iap_ui_ota.outcome));
    index_text[0] = (char)('1' + index);
    index_text[1] = '\0';
    iap_ui_draw_ascii(IAP_UI_OTA_STEP_INDEX_X,
                      (uint16_t)(row_y + 1U),
                      index_text,
                      iap_ui_ota_status_color(s_iap_ui_ota.step_states[index], s_iap_ui_ota.outcome),
                      IAP_UI_OTA_PANEL_COLOR,
                      16U);
    iap_ui_draw_zh_text(IAP_UI_OTA_STEP_LABEL_X,
                        row_y,
                        label,
                        label_count,
                        text_color,
                        fill_color);
    iap_ui_draw_zh_text(iap_ui_ota_status_x(status, status_count),
                        row_y,
                        status,
                        status_count,
                        status_color,
                        fill_color);
}

static void iap_ui_ota_draw_steps(void)
{
    uint8_t index = 0U;

    iap_ui_ota_apply_phase_states();
    for (index = 0U; index < 5U; ++index)
    {
        if (s_iap_ui_ota.step_states[index] != s_iap_ui_ota.last_step_states[index] ||
            s_iap_ui_ota.outcome != s_iap_ui_ota.last_outcome)
        {
            iap_ui_ota_draw_step_row(index);
            s_iap_ui_ota.last_step_states[index] = s_iap_ui_ota.step_states[index];
        }
    }

    s_iap_ui_ota.last_phase = s_iap_ui_ota.phase;
}

static void iap_ui_ota_draw_progress_fill(void)
{
    uint8_t percent = s_iap_ui_ota.percent;
    uint16_t inner_left = (uint16_t)(IAP_UI_OTA_BAR_LEFT + 2U);
    uint16_t inner_top = (uint16_t)(IAP_UI_OTA_BAR_TOP + 2U);
    uint16_t inner_right = (uint16_t)(IAP_UI_OTA_BAR_RIGHT - 2U);
    uint16_t inner_bottom = (uint16_t)(IAP_UI_OTA_BAR_BOTTOM - 2U);
    uint16_t inner_width = (uint16_t)(inner_right - inner_left + 1U);
    uint16_t fill_width = 0U;
    uint16_t fill_right = 0U;
    uint16_t draw_left = 0U;
    uint8_t full_redraw = 0U;
    uint16_t accent = iap_ui_ota_progress_color(s_iap_ui_ota.outcome);

    if (percent > 100U)
    {
        percent = 100U;
    }

    if (s_iap_ui_ota.last_percent == 0xFFU ||
        percent < s_iap_ui_ota.last_percent ||
        s_iap_ui_ota.outcome != s_iap_ui_ota.last_outcome)
    {
        full_redraw = 1U;
    }

    if (full_redraw != 0U)
    {
        iap_ui_fill_rect_inclusive(inner_left,
                                   inner_top,
                                   inner_right,
                                   inner_bottom,
                                   IAP_UI_OTA_TRACK_BG_COLOR);
        s_iap_ui_ota.progress_fill_right = 0xFFFFU;
    }

    if (percent == 0U || inner_width == 0U)
    {
        s_iap_ui_ota.last_percent = percent;
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

    if (s_iap_ui_ota.progress_fill_right == 0xFFFFU ||
        s_iap_ui_ota.progress_fill_right < inner_left ||
        s_iap_ui_ota.progress_fill_right > inner_right)
    {
        draw_left = inner_left;
    }
    else if (fill_right > s_iap_ui_ota.progress_fill_right)
    {
        draw_left = (uint16_t)(s_iap_ui_ota.progress_fill_right + 1U);
    }
    else
    {
        s_iap_ui_ota.last_percent = percent;
        return;
    }

    if (draw_left <= fill_right)
    {
        iap_ui_fill_rect_inclusive(draw_left,
                                   inner_top,
                                   fill_right,
                                   inner_bottom,
                                   accent);
        if ((inner_top + 1U) <= inner_bottom)
        {
            iap_ui_fill_rect_inclusive(draw_left,
                                       inner_top,
                                       fill_right,
                                       (uint16_t)(inner_top + 1U),
                                       (accent == IAP_UI_OTA_ACCENT_COLOR) ?
                                           IAP_UI_BOOT_ORANGE_HI_COLOR :
                                           accent);
        }
    }

    s_iap_ui_ota.progress_fill_right = fill_right;
    s_iap_ui_ota.last_percent = percent;
}

static void iap_ui_ota_draw_percent(void)
{
    char percent_text[5];

    iap_ui_fill_rect_inclusive(IAP_UI_OTA_PERCENT_X,
                               IAP_UI_OTA_PERCENT_Y,
                               304U,
                               (uint16_t)(IAP_UI_OTA_PERCENT_Y + 15U),
                               IAP_UI_OTA_PANEL_COLOR);
    iap_ui_format_percent_text(s_iap_ui_ota.percent, percent_text);
    iap_ui_draw_ascii(IAP_UI_OTA_PERCENT_X,
                      IAP_UI_OTA_PERCENT_Y,
                      percent_text,
                      iap_ui_ota_progress_color(s_iap_ui_ota.outcome),
                      IAP_UI_OTA_PANEL_COLOR,
                      16U);
}

static void iap_ui_ota_draw_detail(void)
{
    char detail_text[28];
    iap_ui_ota_detail_mode_t detail_mode = IAP_UI_OTA_DETAIL_MODE_PROGRESS;
    uint16_t prefix_width = (uint16_t)(sizeof(s_iap_ui_ota_received) / sizeof(s_iap_ui_ota_received[0])) * 16U;
    uint16_t start_x = 0U;
    uint8_t redraw_static = 0U;

    if (s_iap_ui_ota.outcome == IAP_UI_OTA_OUTCOME_FAILURE)
    {
        detail_mode = IAP_UI_OTA_DETAIL_MODE_FAILURE;
    }
    else if (s_iap_ui_ota.total_value == 0U)
    {
        detail_mode = IAP_UI_OTA_DETAIL_MODE_WAITING;
    }

    if (detail_mode != s_iap_ui_ota.last_detail_mode)
    {
        redraw_static = 1U;
    }

    if (detail_mode == IAP_UI_OTA_DETAIL_MODE_FAILURE)
    {
        iap_ui_fill_rect_inclusive((uint16_t)(IAP_UI_OTA_PROGRESS_LEFT + 1U),
                                   IAP_UI_OTA_DETAIL_Y,
                                   (uint16_t)(IAP_UI_OTA_PROGRESS_RIGHT - 1U),
                                   (uint16_t)(IAP_UI_OTA_DETAIL_Y + 12U),
                                   IAP_UI_OTA_PANEL_COLOR);
        iap_ui_format_error_detail(detail_text, s_iap_ui_ota.error_code);
        iap_ui_draw_ascii_center(IAP_UI_OTA_DETAIL_Y,
                                 detail_text,
                                 IAP_UI_OTA_ERROR_COLOR,
                                 IAP_UI_OTA_PANEL_COLOR,
                                 12U);
        s_iap_ui_ota.last_detail_mode = detail_mode;
        s_iap_ui_ota.detail_value_x = 0U;
        s_iap_ui_ota.detail_value_right = 0U;
        return;
    }

    if (detail_mode == IAP_UI_OTA_DETAIL_MODE_WAITING)
    {
        if (redraw_static != 0U)
        {
            iap_ui_fill_rect_inclusive((uint16_t)(IAP_UI_OTA_PROGRESS_LEFT + 1U),
                                       IAP_UI_OTA_DETAIL_Y,
                                       (uint16_t)(IAP_UI_OTA_PROGRESS_RIGHT - 1U),
                                       (uint16_t)(IAP_UI_OTA_DETAIL_Y + 12U),
                                       IAP_UI_OTA_PANEL_COLOR);
            iap_ui_draw_zh_text_center(IAP_UI_OTA_DETAIL_Y,
                                       s_iap_ui_ota_waiting,
                                       (uint8_t)(sizeof(s_iap_ui_ota_waiting) / sizeof(s_iap_ui_ota_waiting[0])),
                                       IAP_UI_OTA_MUTED_COLOR,
                                       IAP_UI_OTA_PANEL_COLOR);
        }
        s_iap_ui_ota.last_detail_mode = detail_mode;
        s_iap_ui_ota.detail_value_x = 0U;
        s_iap_ui_ota.detail_value_right = 0U;
        return;
    }

    iap_ui_format_kb_detail(detail_text, s_iap_ui_ota.current_value, s_iap_ui_ota.total_value);
    if (redraw_static != 0U ||
        s_iap_ui_ota.total_value != s_iap_ui_ota.last_total_value ||
        s_iap_ui_ota.detail_value_x == 0U ||
        s_iap_ui_ota.detail_value_right <= s_iap_ui_ota.detail_value_x)
    {
        uint16_t detail_width = iap_ui_ascii_text_width(detail_text, 12U);

        iap_ui_fill_rect_inclusive((uint16_t)(IAP_UI_OTA_PROGRESS_LEFT + 1U),
                                   IAP_UI_OTA_DETAIL_Y,
                                   (uint16_t)(IAP_UI_OTA_PROGRESS_RIGHT - 1U),
                                   (uint16_t)(IAP_UI_OTA_DETAIL_Y + 12U),
                                   IAP_UI_OTA_PANEL_COLOR);
        start_x = iap_ui_center_x_in_range((uint16_t)(IAP_UI_OTA_PROGRESS_LEFT + 4U),
                                           (uint16_t)(IAP_UI_OTA_PROGRESS_RIGHT - 4U),
                                           (uint16_t)(prefix_width + 8U + detail_width));
        iap_ui_draw_zh_text(start_x,
                            IAP_UI_OTA_DETAIL_Y - 2U,
                            s_iap_ui_ota_received,
                            (uint8_t)(sizeof(s_iap_ui_ota_received) / sizeof(s_iap_ui_ota_received[0])),
                            IAP_UI_OTA_TEXT_COLOR,
                            IAP_UI_OTA_PANEL_COLOR);
        s_iap_ui_ota.detail_value_x = (uint16_t)(start_x + prefix_width + 8U);
        s_iap_ui_ota.detail_value_right = (uint16_t)(IAP_UI_OTA_PROGRESS_RIGHT - 6U);
    }

    iap_ui_fill_rect_inclusive(s_iap_ui_ota.detail_value_x,
                               IAP_UI_OTA_DETAIL_Y,
                               s_iap_ui_ota.detail_value_right,
                               (uint16_t)(IAP_UI_OTA_DETAIL_Y + 12U),
                               IAP_UI_OTA_PANEL_COLOR);
    iap_ui_draw_ascii(s_iap_ui_ota.detail_value_x,
                      IAP_UI_OTA_DETAIL_Y,
                      detail_text,
                      IAP_UI_OTA_ACCENT_COLOR,
                      IAP_UI_OTA_PANEL_COLOR,
                      12U);
    s_iap_ui_ota.last_detail_mode = detail_mode;
}

static void iap_ui_ota_draw_footer(void)
{
    const uint16_t *left_text = 0;
    const uint16_t *center_text = 0;
    const uint16_t *right_text = 0;
    uint8_t left_count = 0U;
    uint8_t center_count = 0U;
    uint8_t right_count = 0U;

    iap_ui_fill_rect_inclusive((uint16_t)(IAP_UI_OTA_FOOTER_LEFT + 1U),
                               IAP_UI_OTA_FOOTER_TOP + 1U,
                               (uint16_t)(IAP_UI_OTA_FOOTER_RIGHT - 1U),
                               (uint16_t)(IAP_UI_OTA_FOOTER_BOTTOM - 1U),
                               IAP_UI_OTA_PANEL_COLOR);

    left_text = iap_ui_ota_footer_left_text(s_iap_ui_ota.outcome, &left_count);
    center_text = iap_ui_ota_footer_center_text(s_iap_ui_ota.outcome, &center_count);
    right_text = iap_ui_ota_footer_right_text(s_iap_ui_ota.link_state, &right_count);

    iap_ui_draw_zh_text(IAP_UI_OTA_FOOTER_LEFT_X,
                        IAP_UI_OTA_FOOTER_TEXT_Y,
                        left_text,
                        left_count,
                        (s_iap_ui_ota.outcome == IAP_UI_OTA_OUTCOME_FAILURE) ?
                            IAP_UI_OTA_ERROR_COLOR :
                            ((s_iap_ui_ota.outcome == IAP_UI_OTA_OUTCOME_SUCCESS) ?
                                 IAP_UI_OTA_SUCCESS_COLOR :
                                 IAP_UI_OTA_ACCENT_COLOR),
                        IAP_UI_OTA_PANEL_COLOR);
    iap_ui_draw_zh_text(IAP_UI_OTA_FOOTER_CENTER_X,
                        IAP_UI_OTA_FOOTER_TEXT_Y,
                        center_text,
                        center_count,
                        IAP_UI_OTA_MUTED_COLOR,
                        IAP_UI_OTA_PANEL_COLOR);
    iap_ui_draw_zh_text(IAP_UI_OTA_FOOTER_RIGHT_X,
                        IAP_UI_OTA_FOOTER_TEXT_Y,
                        right_text,
                        right_count,
                        (s_iap_ui_ota.link_state == IAP_UI_OTA_LINK_FAILURE) ?
                            IAP_UI_OTA_ERROR_COLOR :
                            IAP_UI_OTA_SUCCESS_COLOR,
                        IAP_UI_OTA_PANEL_COLOR);
}

static void iap_ui_render_upgrade_screen(void)
{
    if (iap_ui_ota_screen_active() == 0U || s_iap_ui_ota.initialized == 0U)
    {
        iap_ui_draw_upgrade_frame();
        s_iap_ui_ota.initialized = 1U;
    }

    iap_ui_ota_draw_info_values();
    iap_ui_ota_draw_steps();
    if (s_iap_ui_ota.percent != s_iap_ui_ota.last_percent ||
        s_iap_ui_ota.outcome != s_iap_ui_ota.last_outcome)
    {
        iap_ui_ota_draw_progress_fill();
        iap_ui_ota_draw_percent();
    }
    if (s_iap_ui_ota.current_value != s_iap_ui_ota.last_current_value ||
        s_iap_ui_ota.total_value != s_iap_ui_ota.last_total_value ||
        s_iap_ui_ota.error_code != s_iap_ui_ota.last_error_code ||
        s_iap_ui_ota.outcome != s_iap_ui_ota.last_outcome)
    {
        iap_ui_ota_draw_detail();
    }

    if (s_iap_ui_ota.outcome != s_iap_ui_ota.last_outcome ||
        s_iap_ui_ota.link_state != s_iap_ui_ota.last_link_state)
    {
        iap_ui_ota_draw_footer();
        s_iap_ui_ota.last_link_state = s_iap_ui_ota.link_state;
    }

    s_iap_ui_ota.last_outcome = s_iap_ui_ota.outcome;
    s_iap_ui_ota.last_current_value = s_iap_ui_ota.current_value;
    s_iap_ui_ota.last_total_value = s_iap_ui_ota.total_value;
    s_iap_ui_ota.last_error_code = s_iap_ui_ota.error_code;
    iap_ui_present_if_needed();
}

static void iap_ui_present_if_needed(void)
{
    if (s_iap_ui_backlight_on == 0U)
    {
        LCD_BLK_Set();
        s_iap_ui_backlight_on = 1U;
    }
}

static iap_ui_ota_phase_t iap_ui_phase_from_ctrl_stage(uint8_t stage)
{
    switch (stage)
    {
    case OTA_CTRL_STAGE_DOWNLOAD:
    case OTA_CTRL_STAGE_TRANSFER:
        return IAP_UI_OTA_PHASE_RECEIVE;

    case OTA_CTRL_STAGE_VERIFY_SIG:
    case OTA_CTRL_STAGE_VERIFY_CRC:
    case OTA_CTRL_STAGE_AES_PREPARE:
    case OTA_CTRL_STAGE_READY:
        return IAP_UI_OTA_PHASE_VERIFY;

    case OTA_CTRL_STAGE_DONE:
        return IAP_UI_OTA_PHASE_REBOOT;

    case OTA_CTRL_STAGE_QUERY:
    default:
        return IAP_UI_OTA_PHASE_HANDSHAKE;
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
    iap_ui_ota_reset_context();
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
    if (iap_ui_ota_screen_active() == 0U)
    {
        iap_ui_ota_reset_context();
    }

    s_iap_ui_ota.phase = IAP_UI_OTA_PHASE_HANDSHAKE;
    s_iap_ui_ota.outcome = IAP_UI_OTA_OUTCOME_PROGRESS;
    s_iap_ui_ota.link_state = IAP_UI_OTA_LINK_STABLE;
    s_iap_ui_ota.error_code = 0U;
    s_iap_ui_ota.percent = 0U;
    s_iap_ui_ota.current_value = 0U;
    s_iap_ui_screen = IAP_UI_SCREEN_PREPARE;
    iap_ui_render_upgrade_screen();
}

void iap_ui_show_upgrade_success(void)
{
    if (iap_ui_ota_screen_active() == 0U)
    {
        iap_ui_ota_reset_context();
    }

    s_iap_ui_ota.phase = IAP_UI_OTA_PHASE_REBOOT;
    s_iap_ui_ota.outcome = IAP_UI_OTA_OUTCOME_SUCCESS;
    s_iap_ui_ota.link_state = IAP_UI_OTA_LINK_STABLE;
    s_iap_ui_ota.error_code = 0U;
    s_iap_ui_ota.percent = 100U;
    if (s_iap_ui_ota.total_value != 0U)
    {
        s_iap_ui_ota.current_value = s_iap_ui_ota.total_value;
    }
    s_iap_ui_screen = IAP_UI_SCREEN_SUCCESS;
    iap_ui_render_upgrade_screen();
}

void iap_ui_show_upgrade_failure(void)
{
    uint8_t percent = (s_ymodem_last_percent == 0xFFU) ? 0U : s_ymodem_last_percent;

    if (iap_ui_ota_screen_active() == 0U)
    {
        iap_ui_ota_reset_context();
    }

    s_iap_ui_ota.outcome = IAP_UI_OTA_OUTCOME_FAILURE;
    s_iap_ui_ota.link_state = IAP_UI_OTA_LINK_FAILURE;
    s_iap_ui_ota.percent = percent;
    s_iap_ui_screen = IAP_UI_SCREEN_FAILURE;
    iap_ui_render_upgrade_screen();
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
        s_iap_ui_ota.error_code = 0U;
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
        s_iap_ui_ota.error_code = 0U;
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

    if (stage == OTA_CTRL_STAGE_DONE && percent >= 100U)
    {
        iap_ui_show_upgrade_success();
        return;
    }

    if (iap_ui_ota_screen_active() == 0U)
    {
        iap_ui_show_upgrade_prepare();
    }

    if (stage == OTA_CTRL_STAGE_TRANSFER ||
        stage == OTA_CTRL_STAGE_DOWNLOAD)
    {
        s_iap_ui_ota.phase = IAP_UI_OTA_PHASE_RECEIVE;
        if (percent != OTA_CTRL_PERCENT_UNKNOWN)
        {
            s_iap_ui_ota.percent = percent;
        }
        if (total_value != 0U)
        {
            s_iap_ui_ota.current_value = current_value;
            s_iap_ui_ota.total_value = total_value;
        }
        s_iap_ui_screen = IAP_UI_SCREEN_PROGRESS;
        iap_ui_render_upgrade_screen();
        return;
    }

    s_iap_ui_ota.phase = iap_ui_phase_from_ctrl_stage(stage);

    s_iap_ui_screen = IAP_UI_SCREEN_PREPARE;
    iap_ui_render_upgrade_screen();
}

void ota_ctrl_show_ready_info(const ota_ctrl_frame_t *frame)
{
    if (iap_ui_ota_screen_active() == 0U)
    {
        iap_ui_show_upgrade_prepare();
    }

    if (frame != 0 && frame->payload_len >= OTA_CTRL_READY_PAYLOAD_LEN)
    {
        s_iap_ui_ota.target_partition = frame->payload[0];
        s_iap_ui_ota.total_value = (uint32_t)frame->payload[24] |
                                   ((uint32_t)frame->payload[25] << 8) |
                                   ((uint32_t)frame->payload[26] << 16) |
                                   ((uint32_t)frame->payload[27] << 24);
        s_iap_ui_ota.current_value = 0U;
        s_iap_ui_ota.percent = 0U;
        iap_ui_ascii_copy_bytes(s_iap_ui_ota.target_version,
                                sizeof(s_iap_ui_ota.target_version),
                                &frame->payload[4],
                                OTA_CTRL_VERSION_LEN);
    }

    s_iap_ui_ota.phase = IAP_UI_OTA_PHASE_RECEIVE;
    s_iap_ui_screen = IAP_UI_SCREEN_PROGRESS;
    iap_ui_render_upgrade_screen();
}

void ota_ctrl_show_error_code(uint8_t stage, uint16_t error_code)
{
    s_iap_ui_ota.phase = iap_ui_phase_from_ctrl_stage(stage);

    s_iap_ui_ota.error_code = error_code;
    iap_ui_show_upgrade_failure();
}

void ota_ctrl_show_ack_reject_reason(uint16_t reason_code)
{
    s_iap_ui_ota.phase = IAP_UI_OTA_PHASE_HANDSHAKE;
    s_iap_ui_ota.error_code = reason_code;
    iap_ui_show_upgrade_failure();
}

void iap_show_version_lines(const BootInfoTypeDef *boot_info)
{
    if (boot_info == 0)
    {
        return;
    }

    s_iap_ui_ota.target_partition = (uint8_t)boot_info->target_partition;
    iap_ui_ascii_copy(s_iap_ui_ota.current_version,
                      sizeof(s_iap_ui_ota.current_version),
                      boot_info->current_version);

    if (iap_ui_ota_screen_active() != 0U)
    {
        iap_ui_render_upgrade_screen();
    }
}

void iap_show_resume_decision(uint8_t accepted,
                              uint16_t reason_code,
                              uint32_t saved_offset,
                              uint32_t total_size)
{
    (void)reason_code;

    if (accepted != 0U)
    {
        s_iap_ui_ota.phase = IAP_UI_OTA_PHASE_RECEIVE;
        s_iap_ui_ota.current_value = saved_offset;
        s_iap_ui_ota.total_value = total_size;
        if (total_size != 0U)
        {
            s_iap_ui_ota.percent = (uint8_t)((saved_offset * 100U) / total_size);
        }
    }

    if (iap_ui_ota_screen_active() != 0U)
    {
        iap_ui_render_upgrade_screen();
    }
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
    s_iap_ui_ota.percent = percent;
    s_iap_ui_ota.current_value = safe_current;
    s_iap_ui_ota.total_value = total;
    s_iap_ui_ota.phase = (total > 0U && safe_current >= total) ?
                             IAP_UI_OTA_PHASE_WRITE :
                             IAP_UI_OTA_PHASE_RECEIVE;
    s_iap_ui_screen = IAP_UI_SCREEN_PROGRESS;
    iap_ui_render_upgrade_screen();
}
