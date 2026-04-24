#ifndef UI_RENDERER_H
#define UI_RENDERER_H

#include <stdint.h>

#include "battery_monitor.h"
#include "clock_profile_service.h"
#include "power_manager.h"

#define UI_HEADER_HEIGHT        30U
#define UI_CONTENT_TOP          42U
#define UI_ROW_HEIGHT           24U
#define UI_LIST_START_Y         108U
#define UI_FOOTER_LINE1_Y       196U
#define UI_FOOTER_LINE2_Y       216U
#define UI_FOOTER_FONT_SIZE     12U

const char *ui_renderer_localize(const char *text);
void ui_renderer_draw_header(const char *title, uint16_t header_color);
void ui_renderer_draw_header_status(const char *title, uint16_t header_color);
void ui_renderer_draw_header_hint(const char *title, const char *hint, uint16_t header_color);
void ui_renderer_draw_header_path(const char *parent_title,
                                  const char *child_title,
                                  uint16_t header_color);
void ui_renderer_draw_header_path_status(const char *parent_title,
                                         const char *child_title,
                                         uint16_t header_color);
void ui_renderer_draw_header_path_hint(const char *parent_title,
                                       const char *child_title,
                                       const char *hint,
                                       uint16_t header_color);
void ui_renderer_draw_footer(const char *line1, const char *line2);
void ui_renderer_clear_body(uint16_t color);
void ui_renderer_clear_row(uint16_t y, uint16_t color);
void ui_renderer_draw_value_row(uint16_t y,
                                const char *label,
                                const char *value,
                                uint16_t value_color,
                                uint16_t back_color);
void ui_renderer_draw_list_item(uint16_t y,
                                const char *label,
                                uint8_t selected,
                                uint8_t accent,
                                uint16_t back_color);
void ui_renderer_draw_toggle_item(uint16_t y,
                                  const char *label,
                                  uint8_t enabled,
                                  uint8_t selected,
                                  uint16_t back_color);
void ui_renderer_draw_option_item(uint16_t y,
                                  const char *label,
                                  const char *value,
                                  uint8_t selected,
                                  uint16_t back_color);
const char *ui_renderer_battery_level_text(battery_level_t level);
const char *ui_renderer_power_state_text(power_state_t state);
const char *ui_renderer_power_policy_text(power_policy_t policy);
const char *ui_renderer_clock_policy_text(clock_profile_policy_t policy);
const char *ui_renderer_clock_profile_text(clock_profile_t profile);

#endif
