#include "ui_renderer.h"

#include <stdio.h>
#include <string.h>

#include "app_display_runtime.h"
#include "esp_host_service.h"
#include "lcd_init.h"
#include "lcd.h"
#include "lcd_utf8.h"

#define UI_TEXT_LEFT_X      12U
#define UI_VALUE_X          180U
#define UI_ITEM_LEFT_X      12U
#define UI_ITEM_VALUE_X     232U
#define UI_UTF8_FONT_SIZE   16U
#define UI_HEADER_TITLE_X   8U
#define UI_HEADER_TITLE_Y   7U
#define UI_HEADER_STATUS_RIGHT_MARGIN 8U
#define UI_HEADER_STATUS_GAP 8U
#define UI_HEADER_BATTERY_WIDTH 16U
#define UI_HEADER_BATTERY_HEIGHT 10U
#define UI_HEADER_BATTERY_TIP_WIDTH 2U
#define UI_HEADER_BATTERY_TIP_HEIGHT 4U
#define UI_HEADER_WIFI_WIDTH 14U
#define UI_HEADER_WIFI_HEIGHT 12U

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
        if (*cursor < 0x80U)
        {
            width = (uint16_t)(width + (font_size / 2U));
            ++cursor;
            continue;
        }

        width = (uint16_t)(width + font_size);
        if ((*cursor & 0xE0U) == 0xC0U && cursor[1] != '\0')
        {
            cursor += 2;
        }
        else if ((*cursor & 0xF0U) == 0xE0U &&
                 cursor[1] != '\0' &&
                 cursor[2] != '\0')
        {
            cursor += 3;
        }
        else if ((*cursor & 0xF8U) == 0xF0U &&
                 cursor[1] != '\0' &&
                 cursor[2] != '\0' &&
                 cursor[3] != '\0')
        {
            cursor += 4;
        }
        else
        {
            ++cursor;
        }
    }

    return width;
}

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

        if ((uint16_t)(width + char_width) > max_width || (uint16_t)(write_index + char_len + 1U) >= buffer_len)
        {
            break;
        }

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

static void ui_renderer_draw_battery_icon(uint16_t x, uint16_t y, uint8_t percent, uint16_t back_color)
{
    uint16_t body_left = x;
    uint16_t body_top = y;
    uint16_t body_right = (uint16_t)(x + UI_HEADER_BATTERY_WIDTH - 1U);
    uint16_t body_bottom = (uint16_t)(y + UI_HEADER_BATTERY_HEIGHT - 1U);
    uint16_t tip_left = (uint16_t)(body_right + 1U);
    uint16_t tip_top = (uint16_t)(y + ((UI_HEADER_BATTERY_HEIGHT - UI_HEADER_BATTERY_TIP_HEIGHT) / 2U));
    uint16_t tip_bottom = (uint16_t)(tip_top + UI_HEADER_BATTERY_TIP_HEIGHT - 1U);
    uint16_t inner_left = (uint16_t)(body_left + 2U);
    uint16_t inner_top = (uint16_t)(body_top + 2U);
    uint16_t inner_right = (uint16_t)(body_right - 2U);
    uint16_t inner_bottom = (uint16_t)(body_bottom - 2U);
    uint16_t inner_width = 0U;
    uint16_t fill_width = 0U;
    uint16_t fill_color = ui_renderer_battery_fill_color(percent);

    LCD_DrawRectangle(body_left, body_top, body_right, body_bottom, WHITE);
    LCD_DrawRectangle(tip_left,
                      tip_top,
                      (uint16_t)(tip_left + UI_HEADER_BATTERY_TIP_WIDTH - 1U),
                      tip_bottom,
                      WHITE);

    LCD_Fill(inner_left, inner_top, inner_right, inner_bottom, back_color);
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

static void ui_renderer_draw_wifi_icon(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_DrawLine((uint16_t)(x + 1U), (uint16_t)(y + 4U), (uint16_t)(x + 7U), y, color);
    LCD_DrawLine((uint16_t)(x + 7U), y, (uint16_t)(x + 13U), (uint16_t)(y + 4U), color);
    LCD_DrawLine((uint16_t)(x + 2U), (uint16_t)(y + 5U), (uint16_t)(x + 7U), (uint16_t)(y + 1U), color);
    LCD_DrawLine((uint16_t)(x + 7U), (uint16_t)(y + 1U), (uint16_t)(x + 12U), (uint16_t)(y + 5U), color);

    LCD_DrawLine((uint16_t)(x + 3U), (uint16_t)(y + 7U), (uint16_t)(x + 7U), (uint16_t)(y + 4U), color);
    LCD_DrawLine((uint16_t)(x + 7U), (uint16_t)(y + 4U), (uint16_t)(x + 11U), (uint16_t)(y + 7U), color);
    LCD_DrawLine((uint16_t)(x + 4U), (uint16_t)(y + 8U), (uint16_t)(x + 7U), (uint16_t)(y + 5U), color);
    LCD_DrawLine((uint16_t)(x + 7U), (uint16_t)(y + 5U), (uint16_t)(x + 10U), (uint16_t)(y + 8U), color);

    LCD_DrawLine((uint16_t)(x + 5U), (uint16_t)(y + 9U), (uint16_t)(x + 7U), (uint16_t)(y + 7U), color);
    LCD_DrawLine((uint16_t)(x + 7U), (uint16_t)(y + 7U), (uint16_t)(x + 9U), (uint16_t)(y + 9U), color);
    LCD_Fill((uint16_t)(x + 6U), (uint16_t)(y + 10U), (uint16_t)(x + 8U), (uint16_t)(y + 11U), color);
}

static uint16_t ui_renderer_draw_header_status_right(uint16_t header_color)
{
    esp_host_status_t host_status;
    char percent_text[8];
    uint16_t right_x = (uint16_t)(LCD_W - UI_HEADER_STATUS_RIGHT_MARGIN);
    uint16_t percent_width = 0U;
    uint16_t percent_x = 0U;
    uint16_t battery_left = 0U;

    esp_host_get_status_copy(&host_status);
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

    battery_left = (uint16_t)(percent_x - UI_HEADER_STATUS_GAP - UI_HEADER_BATTERY_WIDTH - UI_HEADER_BATTERY_TIP_WIDTH);
    ui_renderer_draw_battery_icon(battery_left, 9U, battery_monitor_get_percent(), header_color);

    if (host_status.wifi_connected != 0U)
    {
        uint16_t wifi_left = (uint16_t)(battery_left - UI_HEADER_STATUS_GAP - UI_HEADER_WIFI_WIDTH);
        ui_renderer_draw_wifi_icon(wifi_left, 8U, WHITE);
        return wifi_left;
    }

    return battery_left;
}

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
    if (display_parent != 0 && display_parent[0] != '\0')
    {
        snprintf(path_buffer, path_buffer_len, "%s", display_parent);
    }
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

static void ui_renderer_draw_header_core(const char *title,
                                         uint16_t header_color,
                                         uint8_t show_status_icons)
{
    char fitted_title[96];
    const char *display_title = ui_renderer_localize(title);
    uint16_t title_right_limit = (uint16_t)(LCD_W - UI_HEADER_STATUS_RIGHT_MARGIN);

    app_display_runtime_lock();
    LCD_Fill(0, 0, LCD_W - 1U, UI_HEADER_HEIGHT - 1U, header_color);
    if (show_status_icons != 0U)
    {
        title_right_limit = ui_renderer_draw_header_status_right(header_color);
    }

    if (display_title != 0 && display_title[0] != '\0' && title_right_limit > (UI_HEADER_TITLE_X + 4U))
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

const char *ui_renderer_localize(const char *text)
{
    if (text == 0)
    {
        return 0;
    }

    if (strcmp(text, "Main Menu") == 0) return "\xE4\xB8\xBB\xE8\x8F\x9C\xE5\x8D\x95";
    if (strcmp(text, "Thermal") == 0) return "\xE7\x83\xAD\xE6\x88\x90\xE5\x83\x8F";
    if (strcmp(text, "Update") == 0) return "\xE7\xB3\xBB\xE7\xBB\x9F\xE6\x9B\xB4\xE6\x96\xB0";
    if (strcmp(text, "WiFi") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE7\xBD\x91\xE7\xBB\x9C";
    if (strcmp(text, "WiFi(KEY6)") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE7\xBD\x91\xE7\xBB\x9C\x20\xE9\x94\xAE\x36";
    if (strcmp(text, "WiFi...") == 0) return "\xE6\x97\xA0\xE7\xBA\xBF\xE7\xBD\x91\xE7\xBB\x9C\x2E\x2E\x2E";
    if (strcmp(text, "Power") == 0) return "\xE7\x94\xB5\xE6\xBA\x90\xE7\xAE\xA1\xE7\x90\x86";
    if (strcmp(text, "System") == 0) return "\xE7\xB3\xBB\xE7\xBB\x9F\xE8\xAE\xBE\xE7\xBD\xAE";

    if (strcmp(text, "Check Now") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\xE6\x9B\xB4\xE6\x96\xB0";
    if (strcmp(text, "Start Update") == 0) return "\xE5\xBC\x80\xE5\xA7\x8B\xE5\x8D\x87\xE7\xBA\xA7";
    if (strcmp(text, "Restore Last") == 0) return "\xE6\x81\xA2\xE5\xA4\x8D\xE4\xB8\x8A\xE7\x89\x88";
    if (strcmp(text, "Restore last") == 0) return "\xE6\x81\xA2\xE5\xA4\x8D\xE4\xB8\x8A\xE7\x89\x88";
    if (strcmp(text, "Restore Previous Version") == 0) return "\xE6\x81\xA2\xE5\xA4\x8D\xE4\xB8\x8A\xE4\xB8\x80\xE4\xB8\xAA\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Version Info") == 0) return "\xE7\x89\x88\xE6\x9C\xAC\xE4\xBF\xA1\xE6\x81\xAF";
    if (strcmp(text, "Debug Mode") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\xE6\xA8\xA1\xE5\xBC\x8F";
    if (strcmp(text, "Debug Tools") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\xE5\xB7\xA5\xE5\x85\xB7";
    if (strcmp(text, "Debug Page") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\xE9\xA1\xB5\xE9\x9D\xA2";
    if (strcmp(text, "Perf Baseline") == 0) return "\xE6\x80\xA7\xE8\x83\xBD\xE5\x9F\xBA\xE7\xBA\xBF";
    if (strcmp(text, "Debug Screen") == 0) return "\xE8\xB0\x83\xE8\xAF\x95\xE5\xB1\x8F\xE5\xB9\x95";
    if (strcmp(text, "Remote Keys") == 0) return "\xE9\x81\xA5\xE6\x8E\xA7\xE6\x8C\x89\xE9\x94\xAE";

    if (strcmp(text, "Power Mode") == 0) return "\xE7\x94\xB5\xE6\xBA\x90\xE6\xA8\xA1\xE5\xBC\x8F";
    if (strcmp(text, "Clock Policy") == 0) return "\xE6\x97\xB6\xE9\x92\x9F\xE7\xAD\x96\xE7\x95\xA5";
    if (strcmp(text, "Screen Off") == 0) return "\xE7\x86\x84\xE5\xB1\x8F\xE6\x97\xB6\xE9\x97\xB4";
    if (strcmp(text, "Stop Wake") == 0) return "\x53\x54\x4F\x50\xE5\x94\xA4\xE9\x86\x92";
    if (strcmp(text, "Standby") == 0) return "\xE8\x87\xAA\xE5\x8A\xA8\xE5\xBE\x85\xE6\x9C\xBA";
    if (strcmp(text, "ESP Save") == 0) return "\x45\x53\x50\xE4\xBC\x91\xE7\x9C\xA0";
    if (strcmp(text, "Standby Test") == 0) return "\xE5\xBE\x85\xE6\x9C\xBA\xE6\xB5\x8B\xE8\xAF\x95";

    if (strcmp(text, "Current") == 0) return "\xE5\xBD\x93\xE5\x89\x8D";
    if (strcmp(text, "Target") == 0) return "\xE7\x9B\xAE\xE6\xA0\x87";
    if (strcmp(text, "Mode") == 0) return "\xE6\xA8\xA1\xE5\xBC\x8F";
    if (strcmp(text, "Battery") == 0) return "\xE7\x94\xB5\xE6\xB1\xA0";
    if (strcmp(text, "Battery Level") == 0) return "\xE7\x94\xB5\xE9\x87\x8F";
    if (strcmp(text, "Connection") == 0) return "\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "WiFi Status") == 0) return "\x57\x49\x46\x49\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE5\x86\xB5";
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

    if (strcmp(text, "KEY1/KEY3 Move  KEY2 Enter") == 0) return "\xE9\x94\xAE\x31\x2F\x33\xE7\xA7\xBB\xE5\x8A\xA8\x20\xE9\x94\xAE\x32\xE8\xBF\x9B\xE5\x85\xA5";
    if (strcmp(text, "KEY1/KEY3 Move  KEY2 Select") == 0) return "\xE9\x94\xAE\x31\x2F\x33\xE7\xA7\xBB\xE5\x8A\xA8\x20\xE9\x94\xAE\x32\xE9\x80\x89\xE6\x8B\xA9";
    if (strcmp(text, "KEY2 Toggle  Hold Home") == 0) return "\xE9\x94\xAE\x32\xE5\x88\x87\xE6\x8D\xA2\x20\xE9\x95\xBF\xE6\x8C\x89\xE9\x94\xAE\x32\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "KEY2 Cycle  Hold Home") == 0) return "\xE9\x94\xAE\x32\xE5\x88\x87\xE6\x8D\xA2\x20\xE9\x95\xBF\xE6\x8C\x89\xE9\x94\xAE\x32\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "KEY1/KEY3 Move  KEY2 Change") == 0) return "\xE9\x94\xAE\x31\x2F\x33\xE7\xA7\xBB\xE5\x8A\xA8\x20\xE9\x94\xAE\x32\xE4\xBF\xAE\xE6\x94\xB9";
    if (strcmp(text, "KEY1 Back  KEY2 Confirm") == 0) return "\xE9\x94\xAE\x31\xE8\xBF\x94\xE5\x9B\x9E\x20\xE9\x94\xAE\x32\xE7\xA1\xAE\xE8\xAE\xA4";
    if (strcmp(text, "KEY1/KEY2 Back  Hold Home") == 0) return "\xE9\x94\xAE\x31\x2F\x32\xE8\xBF\x94\xE5\x9B\x9E\x20\xE9\x95\xBF\xE6\x8C\x89\xE9\x94\xAE\x32\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "KEY1 Back  KEY2 Enable") == 0) return "\xE9\x94\xAE\x31\xE8\xBF\x94\xE5\x9B\x9E\x20\xE9\x94\xAE\x32\xE5\xBC\x80\xE5\x90\xAF";
    if (strcmp(text, "KEY1/KEY3 Page  KEY2 Reset") == 0) return "\xE9\x94\xAE\x31\x2F\x33\xE7\xBF\xBB\xE9\xA1\xB5\x20\xE9\x94\xAE\x32\xE6\xB8\x85\xE9\x9B\xB6";

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

    if (strcmp(text, "PRESS KEY6") == 0) return "\xE6\x8C\x89\xE9\x94\xAE\x36\xE5\x94\xA4\xE9\x86\x92";
    if (strcmp(text, "KEY6") == 0) return "\xE9\x94\xAE\x36";
    if (strcmp(text, "WORKING") == 0) return "\xE5\xA4\x84\xE7\x90\x86\xE4\xB8\xAD";
    if (strcmp(text, "SETUP") == 0) return "\x57\x49\x46\x49\xE6\x9C\xAA\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "CONNECTED") == 0) return "\xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "CONNECTING") == 0) return "\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD";
    if (strcmp(text, "NOT CONNECTED") == 0) return "\x57\x49\x46\x49\xE6\x9C\xAA\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "OFFLINE") == 0) return "\x57\x49\x46\x49\xE6\x9C\xAA\xE8\xBF\x9E\xE6\x8E\xA5";
    if (strcmp(text, "PENDING") == 0) return "\xE5\xBE\x85\xE7\xA1\xAE\xE8\xAE\xA4";
    if (strcmp(text, "DISABLED") == 0) return "\xE5\xB7\xB2\xE7\xA6\x81\xE7\x94\xA8";
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

    if (strcmp(text, "ESP32 busy") == 0) return "\x45\x53\x50\x33\x32\xE5\xBF\x99";
    if (strcmp(text, "No WiFi") == 0) return "\xE6\x97\xA0\x57\x69\x46\x69";
    if (strcmp(text, "Meta failed") == 0) return "\xE5\x85\x83\xE6\x95\xB0\xE6\x8D\xAE\xE5\xA4\xB1\xE8\xB4\xA5";
    if (strcmp(text, "No package") == 0) return "\xE6\x97\xA0\xE5\x8D\x87\xE7\xBA\xA7\xE5\x8C\x85";
    if (strcmp(text, "Product err") == 0) return "\xE4\xBA\xA7\xE5\x93\x81\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "HW rev err") == 0) return "\xE7\xA1\xAC\xE4\xBB\xB6\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "Protocol err") == 0) return "\xE5\x8D\x8F\xE8\xAE\xAE\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "Partition err") == 0) return "\xE5\x88\x86\xE5\x8C\xBA\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "Version err") == 0) return "\xE7\x89\x88\xE6\x9C\xAC\xE9\x94\x99\xE8\xAF\xAF";
    if (strcmp(text, "No update") == 0) return "\xE6\xB2\xA1\xE6\x9C\x89\xE6\x9B\xB4\xE6\x96\xB0";
    if (strcmp(text, "UART timeout") == 0) return "\xE4\xB8\xB2\xE5\x8F\xA3\xE8\xB6\x85\xE6\x97\xB6";

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

    if (strcmp(text, "Infrared Thermal") == 0) return "\xE5\x8A\x9F\xE8\x83\xBD\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "Function Home") == 0) return "\xE5\x8A\x9F\xE8\x83\xBD\xE4\xB8\xBB\xE9\xA1\xB5";
    if (strcmp(text, "Live Measure") == 0) return "\xE5\xAE\x9E\xE6\x97\xB6\xE6\xB5\x8B\xE6\xB8\xA9";
    if (strcmp(text, "Check Version") == 0) return "\xE6\xA3\x80\xE6\x9F\xA5\xE7\x89\x88\xE6\x9C\xAC";
    if (strcmp(text, "Connection Status") == 0) return "\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE6\x80\x81";
    if (strcmp(text, "Power Profile") == 0) return "\xE7\x94\xB5\xE6\xBA\x90\xE6\xA8\xA1\xE5\xBC\x8F";
    if (strcmp(text, "Select Feature") == 0) return "\xE9\x80\x89\xE6\x8B\xA9\xE5\x8A\x9F\xE8\x83\xBD\xE8\xBF\x9B\xE5\x85\xA5\xE5\xAF\xB9\xE5\xBA\x94\xE9\xA1\xB5\xE9\x9D\xA2";

    return text;
}

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

void ui_renderer_draw_header(const char *title, uint16_t header_color)
{
    ui_renderer_draw_header_core(title, header_color, 0U);
}

void ui_renderer_draw_header_status(const char *title, uint16_t header_color)
{
    ui_renderer_draw_header_core(title, header_color, 1U);
}

void ui_renderer_draw_header_hint(const char *title, const char *hint, uint16_t header_color)
{
    (void)hint;
    ui_renderer_draw_header_core(title, header_color, 0U);
}

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

void ui_renderer_draw_header_path_hint(const char *parent_title,
                                       const char *child_title,
                                       const char *hint,
                                       uint16_t header_color)
{
    (void)hint;
    ui_renderer_draw_header_path(parent_title, child_title, header_color);
}

void ui_renderer_draw_footer(const char *line1, const char *line2)
{
    app_display_runtime_lock();
    LCD_Fill(0, UI_FOOTER_LINE1_Y, LCD_W - 1U, LCD_H - 1U, WHITE);

    ui_renderer_draw_text(8, UI_FOOTER_LINE1_Y, line1, DARKBLUE, WHITE);
    ui_renderer_draw_text(8, UI_FOOTER_LINE2_Y, line2, DARKBLUE, WHITE);
    app_display_runtime_unlock();
}

void ui_renderer_clear_body(uint16_t color)
{
    app_display_runtime_lock();
    LCD_Fill(0, UI_HEADER_HEIGHT, LCD_W - 1U, LCD_H - 1U, color);
    app_display_runtime_unlock();
}

void ui_renderer_clear_row(uint16_t y, uint16_t color)
{
    app_display_runtime_lock();
    LCD_Fill(8, y, LCD_W - 8U, (uint16_t)(y + UI_ROW_HEIGHT - 2U), color);
    app_display_runtime_unlock();
}

void ui_renderer_draw_value_row(uint16_t y,
                                const char *label,
                                const char *value,
                                uint16_t value_color,
                                uint16_t back_color)
{
    app_display_runtime_lock();
    LCD_Fill(8, y, LCD_W - 8U, (uint16_t)(y + UI_ROW_HEIGHT - 2U), back_color);

    ui_renderer_draw_text(UI_TEXT_LEFT_X, y, label, BLACK, back_color);
    ui_renderer_draw_text(UI_VALUE_X, y, value, value_color, back_color);
    app_display_runtime_unlock();
}

void ui_renderer_draw_list_item(uint16_t y,
                                const char *label,
                                uint8_t selected,
                                uint8_t accent,
                                uint16_t back_color)
{
    uint16_t row_color = back_color;
    uint16_t text_color = BLACK;

    if (selected != 0U)
    {
        row_color = accent != 0U ? LBBLUE : LGRAYBLUE;
        text_color = WHITE;
    }

    app_display_runtime_lock();
    LCD_Fill(8, y, LCD_W - 8U, (uint16_t)(y + UI_ROW_HEIGHT - 2U), row_color);
    ui_renderer_draw_text(UI_ITEM_LEFT_X, y, label, text_color, row_color);
    app_display_runtime_unlock();
}

void ui_renderer_draw_toggle_item(uint16_t y,
                                  const char *label,
                                  uint8_t enabled,
                                  uint8_t selected,
                                  uint16_t back_color)
{
    const char *value_text = (enabled != 0U) ? "\xE5\xBC\x80" : "\xE5\x85\xB3";
    uint16_t row_color = back_color;
    uint16_t text_color = BLACK;
    uint16_t value_color = (enabled != 0U) ? GREEN : RED;

    if (selected != 0U)
    {
        row_color = LGRAYBLUE;
        text_color = WHITE;
        value_color = WHITE;
    }

    app_display_runtime_lock();
    LCD_Fill(8, y, LCD_W - 8U, (uint16_t)(y + UI_ROW_HEIGHT - 2U), row_color);
    ui_renderer_draw_text(UI_ITEM_LEFT_X, y, label, text_color, row_color);
    ui_renderer_draw_text(UI_ITEM_VALUE_X, y, value_text, value_color, row_color);
    app_display_runtime_unlock();
}

void ui_renderer_draw_option_item(uint16_t y,
                                  const char *label,
                                  const char *value,
                                  uint8_t selected,
                                  uint16_t back_color)
{
    uint16_t row_color = back_color;
    uint16_t text_color = BLACK;
    uint16_t value_color = DARKBLUE;

    if (selected != 0U)
    {
        row_color = LGRAYBLUE;
        text_color = WHITE;
        value_color = WHITE;
    }

    app_display_runtime_lock();
    LCD_Fill(8, y, LCD_W - 8U, (uint16_t)(y + UI_ROW_HEIGHT - 2U), row_color);
    ui_renderer_draw_text(UI_ITEM_LEFT_X, y, label, text_color, row_color);
    ui_renderer_draw_text((uint16_t)(UI_ITEM_VALUE_X - 28U), y, value, value_color, row_color);
    app_display_runtime_unlock();
}

const char *ui_renderer_battery_level_text(battery_level_t level)
{
    switch (level)
    {
    case BATTERY_LEVEL_FULL:
        return "\xE6\xBB\xA1\xE7\x94\xB5";
    case BATTERY_LEVEL_HIGH:
        return "\xE9\xAB\x98\xE7\x94\xB5";
    case BATTERY_LEVEL_MEDIUM:
        return "\xE4\xB8\xAD\xE7\x94\xB5";
    case BATTERY_LEVEL_LOW:
        return "\xE4\xBD\x8E\xE7\x94\xB5";
    case BATTERY_LEVEL_ALERT:
    default:
        return "\xE5\x91\x8A\xE8\xAD\xA6";
    }
}

const char *ui_renderer_power_state_text(power_state_t state)
{
    switch (state)
    {
    case POWER_STATE_ACTIVE_THERMAL:
        return "\xE7\x83\xAD\xE5\x83\x8F";
    case POWER_STATE_SCREEN_OFF_IDLE:
        return "\xE7\x86\x84\xE5\xB1\x8F";
    case POWER_STATE_ACTIVE_UI:
    default:
        return "\xE7\x95\x8C\xE9\x9D\xA2";
    }
}

const char *ui_renderer_power_policy_text(power_policy_t policy)
{
    switch (policy)
    {
    case POWER_POLICY_PERFORMANCE:
        return "\xE6\x80\xA7\xE8\x83\xBD";

    case POWER_POLICY_ECO:
        return "\xE7\x9C\x81\xE7\x94\xB5";

    case POWER_POLICY_BALANCED:
    default:
        return "\xE5\x9D\x87\xE8\xA1\xA1";
    }
}

const char *ui_renderer_clock_policy_text(clock_profile_policy_t policy)
{
    switch (policy)
    {
    case CLOCK_PROFILE_POLICY_HIGH_ONLY:
        return "\xE5\x9B\xBA\xE5\xAE\x9A\x31\x36\x38\x4D\x48\x7A";

    case CLOCK_PROFILE_POLICY_MEDIUM_ONLY:
        return "\xE5\x9B\xBA\xE5\xAE\x9A\x38\x34\x4D\x48\x7A";

    case CLOCK_PROFILE_POLICY_AUTO:
    default:
        return "\xE8\x87\xAA\xE5\x8A\xA8\xE5\x88\x87\xE6\x8D\xA2";
    }
}

const char *ui_renderer_clock_profile_text(clock_profile_t profile)
{
    switch (profile)
    {
    case CLOCK_PROFILE_MEDIUM:
        return "\xE4\xB8\xAD\xE9\xA2\x91";

    case CLOCK_PROFILE_HIGH:
    default:
        return "\xE9\xAB\x98\xE9\xA2\x91";
    }
}
