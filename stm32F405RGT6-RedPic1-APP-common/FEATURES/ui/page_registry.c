#include "page_registry.h"

#include <stdio.h>
#include <string.h>

#include "app_display_runtime.h"
#include "app_perf_baseline.h"
#include "battery_monitor.h"
#include "delay.h"
#include "esp_host_service.h"
#include "key.h"
#include "lcd.h"
#include "lcd_dma.h"
#include "lcd_init.h"
#include "lcd_utf8.h"
#include "ota_ctrl_protocol.h"
#include "ota_service.h"
#include "power_manager.h"
#include "redpic1_thermal.h"
#include "redpic1_app.h"
#include "snapshot_storage.h"
#include "storage_service.h"
#include "ui_renderer.h"

#define PAGE_UI_RGB565(r, g, b) ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | \
                                            (((uint16_t)(g) & 0xFCU) << 3) | \
                                            (((uint16_t)(b) & 0xF8U) >> 3)))
#define PAGE_UI_BG_COLOR             PAGE_UI_RGB565(5U, 18U, 30U)
#define PAGE_UI_PANEL_COLOR          PAGE_UI_RGB565(10U, 32U, 48U)
#define PAGE_UI_PANEL_EDGE_COLOR     PAGE_UI_RGB565(48U, 78U, 96U)
#define PAGE_UI_ACCENT_COLOR         PAGE_UI_RGB565(255U, 120U, 0U)
#define PAGE_UI_ACCENT_EDGE_COLOR    PAGE_UI_RGB565(255U, 166U, 64U)
#define PAGE_UI_CYAN_COLOR           PAGE_UI_RGB565(90U, 220U, 230U)
#define PAGE_UI_SUBTEXT_COLOR        PAGE_UI_RGB565(170U, 176U, 184U)

/*
 * 椤甸潰娉ㄥ唽琛ㄦ枃浠跺悓鏃舵壙杞介〉闈㈠洖璋冩敞鍐屻€侀〉闈㈢鏈夌姸鎬併€佸紓姝ユ湇鍔″洖娴佸拰灞€閮ㄥ埛鏂伴€昏緫銆? * ui_manager 鍙礋璐ｅ仛杞婚噺璋冨害锛岀湡姝ｇ殑椤甸潰鐘舵€佷笌椤甸潰鍐呴儴鍗忎綔閮介泦涓敹鍙ｅ湪杩欓噷銆? */

/* OTA 椤甸潰鍐呴儴鏄剧ず妯″紡銆?*/
typedef enum
{
    OTA_CENTER_MODE_MENU = 0,
    OTA_CENTER_MODE_CONFIRM_WIFI,
    OTA_CENTER_MODE_CONFIRM_UPGRADE,
    OTA_CENTER_MODE_CONFIRM_ROLLBACK,
    OTA_CENTER_MODE_INFO
} ota_center_mode_t;

/* OTA 椤甸潰鍦ㄦ煡璇㈠畬鎴愬悗鐨勫悗缁姩浣溿€?*/
typedef enum
{
    OTA_PENDING_NONE = 0,
    OTA_PENDING_CHECK,
    OTA_PENDING_UPGRADE
} ota_pending_action_t;

/* 椤甸潰渚у紓姝ュ懡浠ょ殑鏈湴 pending 鐘舵€佷笌瓒呮椂淇℃伅銆?*/
typedef struct
{
    /* WiFi 寮€鍏冲懡浠ゆ槸鍚︿粛鍦ㄧ瓑寰呭搷搴斻€?*/
    uint8_t wifi_set_pending;
    /* WiFi 寮€鍏冲懡浠ょ殑鐩爣鐘舵€併€?*/
    uint8_t wifi_target_enabled;
    /* WiFi 寮€鍏冲懡浠ょ殑瓒呮椂鎴鏃堕棿銆?*/
    uint32_t wifi_set_deadline_ms;
    uint8_t ble_set_pending;
    uint8_t ble_target_enabled;
    uint32_t ble_set_deadline_ms;
    uint8_t mqtt_set_pending;
    uint8_t mqtt_target_enabled;
    uint32_t mqtt_set_deadline_ms;
    /* 璋冭瘯灞忓箷寮€鍏冲懡浠ゆ槸鍚︿粛鍦ㄧ瓑寰呭搷搴斻€?*/
    uint8_t debug_screen_pending;
    /* 璋冭瘯灞忓箷寮€鍏冲懡浠ょ殑鐩爣鐘舵€併€?*/
    uint8_t debug_screen_target_enabled;
    /* 璋冭瘯灞忓箷寮€鍏冲懡浠ょ殑瓒呮椂鎴鏃堕棿銆?*/
    uint32_t debug_screen_deadline_ms;
    /* 閬ユ帶鎸夐敭寮€鍏冲懡浠ゆ槸鍚︿粛鍦ㄧ瓑寰呭搷搴斻€?*/
    uint8_t remote_keys_pending;
    /* 閬ユ帶鎸夐敭寮€鍏冲懡浠ょ殑鐩爣鐘舵€併€?*/
    uint8_t remote_keys_target_enabled;
    /* 閬ユ帶鎸夐敭寮€鍏冲懡浠ょ殑瓒呮椂鎴鏃堕棿銆?*/
    uint32_t remote_keys_deadline_ms;
    /* 涓绘満鐘舵€佸悓姝ュ懡浠ゆ槸鍚︿粛鍦ㄧ瓑寰呭搷搴斻€?*/
    uint8_t host_state_pending;
    /* 涓绘満鐘舵€佸悓姝ュ懡浠ょ殑鐩爣鐘舵€併€?*/
    power_state_t host_state_target;
    /* 涓绘満鐘舵€佸悓姝ュ懡浠ょ殑瓒呮椂鎴鏃堕棿銆?*/
    uint32_t host_state_deadline_ms;
    /* 鐢垫簮绛栫暐鍚屾鍛戒护鏄惁浠嶅湪绛夊緟鍝嶅簲銆?*/
    uint8_t power_policy_pending;
    /* 鐢垫簮绛栫暐鍚屾鍛戒护鐨勭洰鏍囩瓥鐣ャ€?*/
    power_policy_t power_policy_target;
    /* 鐢垫簮绛栫暐鍚屾鍛戒护鐨勮秴鏃舵埅姝㈡椂闂淬€?*/
    uint32_t power_policy_deadline_ms;
    /* 寮哄埗娣辩潯鍛戒护鏄惁浠嶅湪绛夊緟鍝嶅簲銆?*/
    uint8_t forced_deep_sleep_pending;
    /* 寮哄埗娣辩潯鍛戒护鐨勮秴鏃舵埅姝㈡椂闂淬€?*/
    uint32_t forced_deep_sleep_deadline_ms;
    /* OTA 鏌ヨ鍛戒护鏄惁浠嶅湪绛夊緟鍝嶅簲銆?*/
    uint8_t ota_query_pending;
    /* OTA 鏌ヨ鍛戒护鐨勮秴鏃舵埅姝㈡椂闂淬€?*/
    uint32_t ota_query_deadline_ms;
    /* OTA 鏌ヨ鎴愬姛鍚庢槸鍚﹂渶瑕佸脊鍑烘垚鍔熶俊鎭〉銆?*/
    uint8_t ota_query_show_success_info;
    /* OTA 鏌ヨ缁撴潫鍚庨渶瑕佹墽琛岀殑鍚庣画鍔ㄤ綔銆?*/
    ota_pending_action_t ota_post_query_action;
    /* 鏄惁澶勪簬 OTA 娴佺▼瑙﹀彂鐨?WiFi 鑷姩寮€鍚樁娈点€?*/
    uint8_t ota_wifi_enable_pending;
} page_async_state_t;

/* 鎸夌储寮曢噸缁樺崟涓彍鍗曢」鐨勫洖璋冪被鍨嬨€?*/
typedef void (*page_draw_item_fn_t)(uint8_t index);

typedef struct
{
    const char *title;
    const char *subtitle;
    uint8_t icon;
    ui_page_id_t target;
    uint16_t accent;
} home_menu_item_t;

static void home_on_enter(ui_page_id_t previous_page);
static void home_on_leave(ui_page_id_t next_page);
static void home_on_key(uint8_t key_value);
static void home_on_tick(void);
static void home_render(uint8_t full_refresh);

static void thermal_on_enter(ui_page_id_t previous_page);
static void thermal_on_leave(ui_page_id_t next_page);
static void thermal_on_key(uint8_t key_value);
static void thermal_on_tick(void);
static void thermal_render(uint8_t full_refresh);

static void ota_center_on_enter(ui_page_id_t previous_page);
static void ota_center_on_leave(ui_page_id_t next_page);
static void ota_center_on_key(uint8_t key_value);
static void ota_center_on_tick(void);
static void ota_center_render(uint8_t full_refresh);

static void connectivity_on_enter(ui_page_id_t previous_page);
static void connectivity_on_leave(ui_page_id_t next_page);
static void connectivity_on_key(uint8_t key_value);
static void connectivity_on_tick(void);
static void connectivity_render(uint8_t full_refresh);

static void power_page_on_enter(ui_page_id_t previous_page);
static void power_page_on_leave(ui_page_id_t next_page);
static void power_page_on_key(uint8_t key_value);
static void power_page_on_tick(void);
static void power_page_render(uint8_t full_refresh);

static void system_on_enter(ui_page_id_t previous_page);
static void system_on_leave(ui_page_id_t next_page);
static void system_on_key(uint8_t key_value);
static void system_on_tick(void);
static void system_render(uint8_t full_refresh);

static void storage_page_on_enter(ui_page_id_t previous_page);
static void storage_page_on_leave(ui_page_id_t next_page);
static void storage_page_on_key(uint8_t key_value);
static void storage_page_on_tick(void);
static void storage_page_render(uint8_t full_refresh);

static void snapshot_review_on_enter(ui_page_id_t previous_page);
static void snapshot_review_on_leave(ui_page_id_t next_page);
static void snapshot_review_on_key(uint8_t key_value);
static void snapshot_review_on_tick(void);
static void snapshot_review_render(uint8_t full_refresh);

static void engineering_on_enter(ui_page_id_t previous_page);
static void engineering_on_leave(ui_page_id_t next_page);
static void engineering_on_key(uint8_t key_value);
static void engineering_on_tick(void);
static void engineering_render(uint8_t full_refresh);

static void perf_baseline_on_enter(ui_page_id_t previous_page);
static void perf_baseline_on_leave(ui_page_id_t next_page);
static void perf_baseline_on_key(uint8_t key_value);
static void perf_baseline_on_tick(void);
static void perf_baseline_render(uint8_t full_refresh);

static uint8_t page_set_wifi_enabled(uint8_t enabled);
static uint8_t page_set_ble_enabled(uint8_t enabled);
static uint8_t page_set_mqtt_enabled(uint8_t enabled);
static uint8_t page_set_debug_screen_enabled(uint8_t enabled);
static uint8_t page_set_remote_keys_enabled(uint8_t enabled);
static uint8_t page_refresh_host_status_async(void);
static uint8_t page_set_host_state_async(power_state_t state);
static uint8_t page_enter_forced_deep_sleep_async(uint32_t timeout_ms);
static void ota_center_enter_menu_mode(void);
static void ota_center_enter_confirm_mode(ota_center_mode_t mode);
static void ota_center_reset_menu_state(void);
static void ota_center_clear_query_follow_up(void);
static void ota_center_clear_info_latched_state(void);
static void ota_center_return_to_menu(void);
static void ota_center_exit_info_mode(uint8_t navigate_home);
static void ota_center_show_local_version_info(void);
static uint8_t ota_center_request_latest_async(uint8_t show_success_info, ota_pending_action_t post_action);
static void ota_center_show_task_busy_info(void);
static void ota_center_show_restart_info(const char *detail);
static void ota_center_handle_query_success(const app_service_rsp_t *rsp,
                                            ota_pending_action_t post_action,
                                            uint8_t show_success_info);
static void ota_center_handle_query_failure(const app_service_rsp_t *rsp);
static const char *ota_center_child_title(void);
static void page_handle_service_response(const app_service_rsp_t *rsp);
static void page_async_handle_timeouts(void);
static uint32_t page_async_make_deadline(uint32_t timeout_ms);
static uint8_t page_async_deadline_expired(uint32_t now_ms, uint32_t deadline_ms);
static void ota_center_draw_info_rows(void);
static void home_draw_item(uint8_t index);
static void page_format_ble_status(char *buffer, uint16_t buffer_len);
static void page_format_cloud_status(char *buffer, uint16_t buffer_len);
static void wifi_draw_status_row(uint8_t force_refresh);
static void wifi_draw_item(uint8_t force_refresh);
static void connectivity_draw_item(uint8_t index);
static void power_draw_info_rows(void);
static void power_draw_battery_status(void);
static void power_draw_item(uint8_t index);
static void storage_draw_item(uint8_t index);
static void engineering_draw_item(uint8_t index);
static uint8_t perf_baseline_debug_visible(void);
static void page_refresh_host_status_views(ui_page_id_t active_page);
static void page_refresh_timeout_views(ui_page_id_t active_page);
static uint8_t page_cycle_prev_index(uint8_t current_index, uint8_t item_count);
static uint8_t page_cycle_next_index(uint8_t current_index, uint8_t item_count);
static void page_move_selection(uint8_t *selected_index,
                                uint8_t item_count,
                                uint8_t move_previous,
                                page_draw_item_fn_t draw_item);
static uint32_t page_next_u32_option(const uint32_t *option_list,
                                     uint32_t option_count,
                                     uint32_t current_value);

/* 椤甸潰鍥炶皟琛紝椤哄簭涓?ui_page_id_t 鏋氫妇淇濇寔涓€鑷淬€?*/
static const ui_page_ops_t s_page_ops[UI_PAGE_COUNT] =
{
    { home_on_enter, home_on_leave, home_on_key, home_on_tick, home_render },
    { thermal_on_enter, thermal_on_leave, thermal_on_key, thermal_on_tick, thermal_render },
    { ota_center_on_enter, ota_center_on_leave, ota_center_on_key, ota_center_on_tick, ota_center_render },
    { connectivity_on_enter, connectivity_on_leave, connectivity_on_key, connectivity_on_tick, connectivity_render },
    { power_page_on_enter, power_page_on_leave, power_page_on_key, power_page_on_tick, power_page_render },
    { system_on_enter, system_on_leave, system_on_key, system_on_tick, system_render },
    { storage_page_on_enter, storage_page_on_leave, storage_page_on_key, storage_page_on_tick, storage_page_render },
    { snapshot_review_on_enter, snapshot_review_on_leave, snapshot_review_on_key, snapshot_review_on_tick, snapshot_review_render },
    { engineering_on_enter, engineering_on_leave, engineering_on_key, engineering_on_tick, engineering_render },
    { perf_baseline_on_enter, perf_baseline_on_leave, perf_baseline_on_key, perf_baseline_on_tick, perf_baseline_render }
};

/* 鍚勯〉闈㈢殑閫夋嫨鐘舵€佷笌杞婚噺鍒锋柊缂撳瓨閮藉彧鍦ㄦ湰鏂囦欢鍐呴儴鍙銆?*/
static uint8_t s_home_selected = 0U;
static uint8_t s_wifi_selected = 0U;
static uint8_t s_power_selected = 0U;
static uint8_t s_system_selected = 0U;
static uint8_t s_storage_selected = 0U;
static uint8_t s_engineering_selected = 0U;
static uint8_t s_perf_baseline_subpage = 0U;
static uint32_t s_perf_baseline_next_refresh_ms = 0U;
static uint32_t s_wifi_next_refresh_ms = 0U;
static char s_connectivity_status_cache[3][24];
static uint16_t s_connectivity_status_color_cache[3];
static uint8_t s_connectivity_status_cache_valid[3];
static char s_connectivity_item_label_cache[3][24];
static uint8_t s_connectivity_item_enabled_cache[3];
static uint8_t s_connectivity_item_selected_cache[3];
static uint8_t s_connectivity_item_pending_cache[3];
static uint8_t s_connectivity_item_cache_valid[3];
static ota_center_mode_t s_ota_mode = OTA_CENTER_MODE_MENU;
static ota_pending_action_t s_ota_pending_action = OTA_PENDING_NONE;
static uint8_t s_ota_selected = 0U;
static char s_ota_latest_version[BOOT_INFO_VERSION_LEN];
static char s_ota_notice_line1[64];
static char s_ota_notice_line2[64];
static char s_ota_info_current_version[BOOT_INFO_VERSION_LEN];
static char s_ota_info_latest_version[BOOT_INFO_VERSION_LEN];
static char s_ota_info_partition[12];
static char s_storage_notice_line1[32];
static char s_storage_notice_line2[32];
static redpic1_thermal_snapshot_t s_snapshot_review_snapshot;
static uint8_t s_snapshot_review_gray_frame[REDPIC1_THERMAL_SNAPSHOT_PIXEL_COUNT];
static uint32_t s_snapshot_review_index = 0U;
static storage_status_t s_snapshot_review_status = STORAGE_STATUS_NO_SNAPSHOT;
static uint8_t s_snapshot_review_loaded = 0U;
  static uint8_t s_ota_show_version_rows = 0U;
static uint8_t s_ota_show_partition_rows = 0U;
static page_async_state_t s_async_state;

/* 棣栭〉鑿滃崟鏂囨湰銆?*/
static const home_menu_item_t s_home_items[] =
{
    { "Thermal",  "Live Thermal View",     0U, UI_PAGE_THERMAL,      PAGE_UI_ACCENT_COLOR },
    { "Update",   "Check Version / Upgrade", 1U, UI_PAGE_OTA_CENTER,   PAGE_UI_ACCENT_COLOR },
    { "Wireless", "Connection Status",     2U, UI_PAGE_CONNECTIVITY, PAGE_UI_ACCENT_COLOR },
    { "Power",    "Power Profile",         3U, UI_PAGE_POWER,        PAGE_UI_ACCENT_COLOR },
    { "System",   "Select Feature",        4U, UI_PAGE_SYSTEM,       PAGE_UI_ACCENT_COLOR }
};

/* OTA 椤甸潰鑿滃崟鏂囨湰銆?*/
static const char * const s_ota_items[] =
{
    "Check Now",
    "Start Update",
    "Restore Previous Version",
    "Version Info"
};

/* 绯荤粺椤甸潰鑿滃崟鏂囨湰銆?*/
static const char * const s_system_items[] =
{
#if (REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ENABLE != 0U)
    "Pause Send Temp",
#endif
    "KEY2 Snapshot",
    "SD Card",
    "Debug Mode",
    "Debug Page"
};

static const char * const s_storage_items[] =
{
    "Mount / Info",
    "Write Test",
    "Read Test",
    "View Latest"
};

/* 宸ョ▼椤甸潰鑿滃崟鏂囨湰銆?*/
static const char * const s_engineering_items[] =
{
    "Perf Baseline",
    "Debug Screen",
    "Remote Keys"
};

/* 鐢垫簮椤甸潰鑿滃崟鏂囨湰銆?*/
static const char * const s_power_items[] =
{
    "Screen Off",
    "Standby",
    "ESP Save"
};

static const uint32_t s_power_screen_off_options_ms[] =
{
    15000UL,
    30000UL,
    45000UL,
    60000UL,
    120000UL,
    180000UL,
    300000UL,
    600000UL
};

#define HOME_ITEM_COUNT            5U
#define OTA_ITEM_COUNT             4U
#define POWER_ITEM_COUNT           3U
#if (REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ENABLE != 0U)
    #define SYSTEM_ITEM_THERMAL_PAUSE_SEND 0U
    #define SYSTEM_ITEM_KEY2_SNAPSHOT      1U
    #define SYSTEM_ITEM_SD_CARD            2U
    #define SYSTEM_ITEM_DEBUG_MODE         3U
    #define SYSTEM_ITEM_DEBUG_PAGE         4U
    #define SYSTEM_ITEM_BASE_COUNT         4U
    #define SYSTEM_ITEM_MAX_COUNT          5U
#else
    #define SYSTEM_ITEM_THERMAL_PAUSE_SEND 0xFFU
    #define SYSTEM_ITEM_KEY2_SNAPSHOT      0U
    #define SYSTEM_ITEM_SD_CARD            1U
    #define SYSTEM_ITEM_DEBUG_MODE         2U
    #define SYSTEM_ITEM_DEBUG_PAGE         3U
    #define SYSTEM_ITEM_BASE_COUNT         3U
    #define SYSTEM_ITEM_MAX_COUNT          4U
#endif
#define STORAGE_ITEM_COUNT         4U
#define ENGINEERING_ITEM_COUNT     3U

#define HOME_LIST_START_Y          76U
#define HOME_BANNER_TOP            34U
#define HOME_BANNER_HEIGHT         34U
#define HOME_CARD_LEFT             12U
#define HOME_CARD_RIGHT            (LCD_W - 12U)
#define HOME_CARD_HEIGHT           26U
#define HOME_CARD_GAP              5U
#define HOME_CARD_ICON_LEFT        24U
#define HOME_CARD_TEXT_LEFT        64U
#define HOME_CARD_SUBTITLE_LEFT    150U
#define HOME_CARD_CHEVRON_LEFT     (LCD_W - 28U)
#define PAGE_INFO_ROW1_Y           72U
#define PAGE_INFO_ROW2_Y           96U
#define PAGE_INFO_ROW3_Y           120U
#define PAGE_INFO_ROW4_Y           144U
#define WIFI_LIST_START_Y          152U
#define WIFI_ITEM_COUNT            3U
#define OTA_LIST_START_Y           104U
#define POWER_LIST_START_Y         104U
#define SYSTEM_LIST_START_Y        88U
#define DEBUG_LIST_START_Y         88U

#define WIFI_STATUS_REFRESH_MS     1500UL
#define POWER_PAGE_HOST_PREP_TIMEOUT_MS 400UL
#define PAGE_ASYNC_TIMEOUT_SHORT_MS 2000UL
#define PAGE_ASYNC_TIMEOUT_WIFI_MS  3000UL
#define PAGE_ASYNC_TIMEOUT_OTA_MS   55000UL
#define PERF_BASELINE_REFRESH_MS    250UL
#define PERF_SUBPAGE_COUNT          14U
#define PERF_LABEL_X                12U
#define PERF_VALUE_X                180U
#define PERF_TIMING_VALUE_X         106U
#define PERF_VALUE_PAD_CHARS        18U
#define PERF_TIMING_VALUE_PAD_CHARS 22U
#define PERF_VALUE_FONT_SIZE        16U
#define PERF_TIMING_VALUE_FONT_SIZE 12U
#define PERF_FOOTER_PAD_CHARS       40U

/* 鏍煎紡鍖栫數姹犵姸鎬佹枃鏈紝渚涢〉闈俊鎭鏄剧ず銆?*/
static void page_format_battery(char *buffer, uint16_t buffer_len)
{
    snprintf(buffer,
             buffer_len,
             "%u%%",
             battery_monitor_get_percent());
}

/* 鏍规嵁褰撳墠璁剧疆鍜屼富鏈虹姸鎬佺敓鎴愪汉鏈哄彲璇荤殑 WiFi 鐘舵€佹枃鏈€?*/
static void page_format_wifi_status(char *buffer, uint16_t buffer_len)
{
    esp_host_status_t status;
    device_settings_t settings;

    esp_host_get_status_copy(&status);
    app_rtos_settings_copy(&settings);

    if (esp_host_is_forced_deep_sleep() != 0U)
    {
        snprintf(buffer, buffer_len, "%s", "PRESS KEY6");
        return;
    }

    if (s_async_state.wifi_set_pending != 0U)
    {
        snprintf(buffer, buffer_len, "%s", "CONNECTING");
        return;
    }

    if (settings.wifi_enabled != 0U)
    {
        if (status.online == 0U)
        {
            snprintf(buffer, buffer_len, "%s", "NOT CONNECTED");
            return;
        }

        if (status.has_credentials == 0U && status.last_seen_ms != 0U)
        {
            snprintf(buffer, buffer_len, "%s", "NOT CONNECTED");
            return;
        }

        snprintf(buffer,
                 buffer_len,
                 "%s",
                 (status.wifi_connected != 0U) ? "CONNECTED" : "CONNECTING");
        return;
    }

    snprintf(buffer, buffer_len, "%s", "NOT CONNECTED");
}

static void page_format_ble_status(char *buffer, uint16_t buffer_len)
{
    esp_host_status_t status;
    device_settings_t settings;

    esp_host_get_status_copy(&status);
    app_rtos_settings_copy(&settings);

    if (s_async_state.ble_set_pending != 0U)
    {
        snprintf(buffer,
                 buffer_len,
                 "%s",
                 (s_async_state.ble_target_enabled != 0U) ? "CONNECTING" : "OFF");
        return;
    }

    if (settings.ble_enabled == 0U)
    {
        snprintf(buffer, buffer_len, "%s", "OFF");
        return;
    }

    if (status.online == 0U)
    {
        snprintf(buffer, buffer_len, "%s", "NOT CONNECTED");
        return;
    }

    snprintf(buffer,
             buffer_len,
             "%s",
             (status.ble_connected != 0U) ? "CONNECTED" : "NOT CONNECTED");
}

static void page_format_cloud_status(char *buffer, uint16_t buffer_len)
{
    esp_host_status_t status;
    device_settings_t settings;

    esp_host_get_status_copy(&status);
    app_rtos_settings_copy(&settings);

    if (s_async_state.mqtt_set_pending != 0U)
    {
        snprintf(buffer,
                 buffer_len,
                 "%s",
                 (s_async_state.mqtt_target_enabled != 0U) ? "CONNECTING" : "OFF");
        return;
    }

    if (settings.mqtt_enabled == 0U)
    {
        snprintf(buffer, buffer_len, "%s", "OFF");
        return;
    }

    if (status.online == 0U || status.wifi_connected == 0U)
    {
        snprintf(buffer, buffer_len, "%s", "NOT CONNECTED");
        return;
    }

    snprintf(buffer,
             buffer_len,
             "%s",
             (status.mqtt_connected != 0U) ? "CONNECTED" : "CONNECTING");
}

/* 杩斿洖褰撳墠璇曡繍琛岀姸鎬佺殑绠€鐭枃妗堛€?*/
/* 鏍规嵁璧峰 Y 鍧愭爣鍜岀储寮曡绠楀垪琛ㄩ」鐨勭粯鍒朵綅缃€?*/
static uint16_t page_list_item_y(uint16_t start_y, uint8_t index)
{
    return (uint16_t)(start_y + ((uint16_t)index * UI_ROW_HEIGHT));
}

/* 璁＄畻寰幆鑿滃崟涓笂涓€涓劍鐐圭储寮曘€?*/
static uint8_t page_cycle_prev_index(uint8_t current_index, uint8_t item_count)
{
    if (item_count == 0U)
    {
        return current_index;
    }

    return (uint8_t)((current_index + item_count - 1U) % item_count);
}

/* 璁＄畻寰幆鑿滃崟涓笅涓€涓劍鐐圭储寮曘€?*/
static uint8_t page_cycle_next_index(uint8_t current_index, uint8_t item_count)
{
    if (item_count == 0U)
    {
        return current_index;
    }

    return (uint8_t)((current_index + 1U) % item_count);
}

/*
 * 鏇存柊鑿滃崟鐒︾偣骞堕噸缁樺墠鍚庝袱涓储寮曚綅缃€? * 鍗充娇鍓嶅悗绱㈠紩鐩稿悓锛屼篃淇濇寔鍘熸湁鐨勫弻娆￠噸缁樿矾寰勶紝閬垮厤鏀瑰彉灞€閮ㄥ埛鏂拌涓恒€? */
static void page_move_selection(uint8_t *selected_index,
                                uint8_t item_count,
                                uint8_t move_previous,
                                page_draw_item_fn_t draw_item)
{
    uint8_t previous_index = 0U;

    if (selected_index == 0 || item_count == 0U || draw_item == 0)
    {
        return;
    }

    previous_index = *selected_index;
    *selected_index = (move_previous != 0U) ?
                      page_cycle_prev_index(*selected_index, item_count) :
                      page_cycle_next_index(*selected_index, item_count);

    draw_item(previous_index);
    draw_item(*selected_index);
}

/* 浠ュ綋鍓嶇郴缁熸椂鍩虹敓鎴愬紓姝ュ懡浠ょ殑鎴鏃堕棿銆?*/
static uint32_t page_async_make_deadline(uint32_t timeout_ms)
{
    return power_manager_get_tick_ms() + timeout_ms;
}

/* 鍒ゆ柇鏌愪釜寮傛鎴鏃堕棿鏄惁宸茬粡瓒呮椂銆?*/
static uint8_t page_async_deadline_expired(uint32_t now_ms, uint32_t deadline_ms)
{
    if (deadline_ms == 0U)
    {
        return 0U;
    }

    return (((int32_t)(now_ms - deadline_ms)) >= 0) ? 1U : 0U;
}

/* 灏嗚秴鏃舵椂闂存牸寮忓寲涓洪〉闈㈡樉绀哄瓧绗︿覆銆?*/
static void page_format_timeout_ms(char *buffer, uint16_t buffer_len, uint32_t timeout_ms)
{
    if ((timeout_ms % 60000UL) == 0UL)
    {
        snprintf(buffer, buffer_len, "%lu min", (unsigned long)(timeout_ms / 60000UL));
    }
    else if ((timeout_ms % 1000UL) == 0UL)
    {
        snprintf(buffer, buffer_len, "%lu s", (unsigned long)(timeout_ms / 1000UL));
    }
    else
    {
        snprintf(buffer, buffer_len, "%lu ms", (unsigned long)timeout_ms);
    }
}

/* 鍦ㄦ棤绗﹀彿鏁村瀷閫夐」琛ㄤ腑杞浆鍒颁笅涓€涓厤缃€笺€?*/
static uint32_t page_next_u32_option(const uint32_t *option_list,
                                     uint32_t option_count,
                                     uint32_t current_value)
{
    uint32_t index = 0U;

    if (option_list == 0 || option_count == 0U)
    {
        return current_value;
    }

    for (index = 0U; index < option_count; ++index)
    {
        if (option_list[index] == current_value)
        {
            return option_list[(index + 1U) % option_count];
        }
    }

    return option_list[0];
}

/* 杞浆寰楀埌涓嬩竴涓?Stop 鍞ら啋瓒呮椂閫夐」銆?*/
/* 杞浆寰楀埌涓嬩竴涓唲灞忚秴鏃堕€夐」銆?*/
static uint32_t page_next_screen_off_timeout_ms(uint32_t current_ms)
{
    return page_next_u32_option(s_power_screen_off_options_ms,
                                (uint32_t)(sizeof(s_power_screen_off_options_ms) /
                                           sizeof(s_power_screen_off_options_ms[0])),
                                current_ms);
}

/* 瑙ｆ瀽鏈€杩戜竴娆″浣嶅師鍥狅紝鐢ㄤ簬绯荤粺椤甸潰鏄剧ず銆?*/
/* 鏍规嵁璋冭瘯妯″紡鐘舵€佽绠楃郴缁熼〉闈㈠綋鍓嶅彲瑙侀」鏁伴噺銆?*/
static uint8_t system_item_count(void)
{
    device_settings_t settings;

    app_rtos_settings_copy(&settings);
    return (settings.debug_mode_enabled != 0U) ? SYSTEM_ITEM_MAX_COUNT : SYSTEM_ITEM_BASE_COUNT;
}

/* 璁剧疆鍐欏叆鎴愬姛鍚庯紝闇€瑕佸悓姝ユ洿鏂板綋鍓嶈繍琛屾椂鐢垫簮绛栫暐銆?*/
static void page_apply_settings(const device_settings_t *settings)
{
    if (settings == 0)
    {
        return;
    }

    power_manager_set_policy(settings->power_policy);
    power_manager_set_screen_off_timeout_ms(settings->screen_off_timeout_ms);
}

/* 鍐欏叆璁剧疆骞跺湪鎴愬姛鍚庡悓姝ュ簲鐢ㄨ繍琛屾椂鍓綔鐢ㄣ€?*/
static uint8_t page_store_settings(device_settings_t *updated)
{
    if (updated == 0)
    {
        return 0U;
    }

    if (app_rtos_settings_update(updated) == 0U)
    {
        return 0U;
    }

    page_apply_settings(updated);
    return 1U;
}

/* 寮傛鎻愪氦 WiFi 寮€鍏冲懡浠わ紝骞剁淮鎶ら〉闈晶 pending 鐘舵€併€?*/
static uint8_t page_set_wifi_enabled(uint8_t enabled)
{
    app_service_cmd_t cmd;
    uint8_t normalized_enabled = (enabled != 0U) ? 1U : 0U;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SET_WIFI;
    cmd.arg0 = normalized_enabled;
    cmd.value = (normalized_enabled != 0U) ? 800UL : 250UL;

    if (s_async_state.wifi_set_pending != 0U)
    {
        s_async_state.wifi_target_enabled = normalized_enabled;
        s_async_state.wifi_set_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_WIFI_MS);
        (void)app_service_submit_async(&cmd);
        return 1U;
    }

    if (app_service_submit_async(&cmd) == 0U)
    {
        return 0U;
    }

    s_async_state.wifi_set_pending = 1U;
    s_async_state.wifi_target_enabled = normalized_enabled;
    s_async_state.wifi_set_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_WIFI_MS);
    ui_manager_request_render();
    return 1U;
}

/* 寮傛鎻愪氦璋冭瘯灞忓箷寮€鍏冲懡浠ゃ€?*/
static uint8_t page_set_ble_enabled(uint8_t enabled)
{
    app_service_cmd_t cmd;
    uint8_t normalized_enabled = (enabled != 0U) ? 1U : 0U;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SET_BLE;
    cmd.arg0 = normalized_enabled;

    if (s_async_state.ble_set_pending != 0U)
    {
        s_async_state.ble_target_enabled = normalized_enabled;
        s_async_state.ble_set_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
        (void)app_service_submit_async(&cmd);
        return 1U;
    }

    if (app_service_submit_async(&cmd) == 0U)
    {
        return 0U;
    }

    s_async_state.ble_set_pending = 1U;
    s_async_state.ble_target_enabled = normalized_enabled;
    s_async_state.ble_set_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
    ui_manager_request_render();
    return 1U;
}

static uint8_t page_set_mqtt_enabled(uint8_t enabled)
{
    app_service_cmd_t cmd;
    uint8_t normalized_enabled = (enabled != 0U) ? 1U : 0U;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SET_MQTT;
    cmd.arg0 = normalized_enabled;

    if (s_async_state.mqtt_set_pending != 0U)
    {
        s_async_state.mqtt_target_enabled = normalized_enabled;
        s_async_state.mqtt_set_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
        (void)app_service_submit_async(&cmd);
        return 1U;
    }

    if (app_service_submit_async(&cmd) == 0U)
    {
        return 0U;
    }

    s_async_state.mqtt_set_pending = 1U;
    s_async_state.mqtt_target_enabled = normalized_enabled;
    s_async_state.mqtt_set_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
    ui_manager_request_render();
    return 1U;
}

static uint8_t page_set_debug_screen_enabled(uint8_t enabled)
{
    app_service_cmd_t cmd;
    uint8_t normalized_enabled = (enabled != 0U) ? 1U : 0U;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SET_DEBUG_SCREEN;
    cmd.arg0 = normalized_enabled;

    if (s_async_state.debug_screen_pending != 0U)
    {
        s_async_state.debug_screen_target_enabled = normalized_enabled;
        s_async_state.debug_screen_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
        (void)app_service_submit_async(&cmd);
        return 1U;
    }

    if (app_service_submit_async(&cmd) == 0U)
    {
        return 0U;
    }

    s_async_state.debug_screen_pending = 1U;
    s_async_state.debug_screen_target_enabled = normalized_enabled;
    s_async_state.debug_screen_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
    ui_manager_request_render();
    return 1U;
}

/* 寮傛鎻愪氦閬ユ帶鎸夐敭寮€鍏冲懡浠ゃ€?*/
static uint8_t page_set_remote_keys_enabled(uint8_t enabled)
{
    app_service_cmd_t cmd;
    uint8_t normalized_enabled = (enabled != 0U) ? 1U : 0U;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SET_REMOTE_KEYS;
    cmd.arg0 = normalized_enabled;

    if (s_async_state.remote_keys_pending != 0U)
    {
        s_async_state.remote_keys_target_enabled = normalized_enabled;
        s_async_state.remote_keys_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
        (void)app_service_submit_async(&cmd);
        return 1U;
    }

    if (app_service_submit_async(&cmd) == 0U)
    {
        return 0U;
    }

    s_async_state.remote_keys_pending = 1U;
    s_async_state.remote_keys_target_enabled = normalized_enabled;
    s_async_state.remote_keys_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
    ui_manager_request_render();
    return 1U;
}

/* 寮傛璇锋眰 ESP 涓绘満鍒锋柊鏈€鏂扮姸鎬併€?*/
static uint8_t page_refresh_host_status_async(void)
{
    app_service_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_ESP_REFRESH_STATUS;
    return app_service_submit_async(&cmd);
}

/* 寮傛灏嗗綋鍓嶇數婧愮姸鎬佸悓姝ョ粰涓绘満渚с€?*/
static uint8_t page_set_host_state_async(power_state_t state)
{
    app_service_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SET_HOST_STATE;
    cmd.arg0 = (uint8_t)state;

    if (s_async_state.host_state_pending != 0U)
    {
        s_async_state.host_state_target = state;
        s_async_state.host_state_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
        (void)app_service_submit_async(&cmd);
        return 1U;
    }

    if (app_service_submit_async(&cmd) == 0U)
    {
        return 0U;
    }

    s_async_state.host_state_pending = 1U;
    s_async_state.host_state_target = state;
    s_async_state.host_state_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
    return 1U;
}

/* 寮傛灏嗙數婧愮瓥鐣ュ悓姝ョ粰涓绘満渚с€?*/
static uint8_t page_set_power_policy_async(power_policy_t policy)
{
    app_service_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SET_POWER_POLICY;
    cmd.arg0 = (uint8_t)policy;

    if (s_async_state.power_policy_pending != 0U)
    {
        s_async_state.power_policy_target = policy;
        s_async_state.power_policy_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
        (void)app_service_submit_async(&cmd);
        return 1U;
    }

    if (app_service_submit_async(&cmd) == 0U)
    {
        return 0U;
    }

    s_async_state.power_policy_pending = 1U;
    s_async_state.power_policy_target = policy;
    s_async_state.power_policy_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_SHORT_MS);
    return 1U;
}

/* 寮傛璇锋眰涓绘満杩涘叆寮哄埗娣辩潯鍑嗗娴佺▼銆?*/
static uint8_t page_enter_forced_deep_sleep_async(uint32_t timeout_ms)
{
    app_service_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_ENTER_FORCED_DEEP_SLEEP;
    cmd.value = timeout_ms;

    if (s_async_state.forced_deep_sleep_pending != 0U)
    {
        s_async_state.forced_deep_sleep_deadline_ms =
            page_async_make_deadline(timeout_ms + PAGE_ASYNC_TIMEOUT_SHORT_MS);
        (void)app_service_submit_async(&cmd);
        return 1U;
    }
    if (app_service_submit_async(&cmd) == 0U)
    {
        return 0U;
    }

    s_async_state.forced_deep_sleep_pending = 1U;
    s_async_state.forced_deep_sleep_deadline_ms =
        page_async_make_deadline(timeout_ms + PAGE_ASYNC_TIMEOUT_SHORT_MS);
    return 1U;
}

/* 璁剧疆 OTA 淇℃伅椤电殑涓よ鎻愮ず鏂囨湰銆?*/
static void ota_center_set_notice(const char *line1, const char *line2)
{
    memset(s_ota_notice_line1, 0, sizeof(s_ota_notice_line1));
    memset(s_ota_notice_line2, 0, sizeof(s_ota_notice_line2));
    memset(s_ota_info_current_version, 0, sizeof(s_ota_info_current_version));
    memset(s_ota_info_latest_version, 0, sizeof(s_ota_info_latest_version));
    memset(s_ota_info_partition, 0, sizeof(s_ota_info_partition));
    s_ota_show_version_rows = 0U;
    s_ota_show_partition_rows = 0U;

    if (line1 != 0)
    {
        snprintf(s_ota_notice_line1, sizeof(s_ota_notice_line1), "%s", line1);
    }
    if (line2 != 0)
    {
        snprintf(s_ota_notice_line2, sizeof(s_ota_notice_line2), "%s", line2);
    }
}

static void ota_center_set_version_rows(const char *current_version, const char *latest_version)
{
    memset(s_ota_info_current_version, 0, sizeof(s_ota_info_current_version));
    memset(s_ota_info_latest_version, 0, sizeof(s_ota_info_latest_version));
    memset(s_ota_info_partition, 0, sizeof(s_ota_info_partition));

    snprintf(s_ota_info_current_version,
             sizeof(s_ota_info_current_version),
             "%s",
             (current_version != 0 && current_version[0] != '\0') ? current_version : "--");
    snprintf(s_ota_info_latest_version,
             sizeof(s_ota_info_latest_version),
             "%s",
             (latest_version != 0 && latest_version[0] != '\0') ? latest_version : "--");
    s_ota_show_version_rows = 1U;
    s_ota_show_partition_rows = 0U;
}

static void ota_center_set_local_info_rows(const char *current_version, const char *partition_name)
{
    memset(s_ota_info_current_version, 0, sizeof(s_ota_info_current_version));
    memset(s_ota_info_latest_version, 0, sizeof(s_ota_info_latest_version));
    memset(s_ota_info_partition, 0, sizeof(s_ota_info_partition));

    snprintf(s_ota_info_current_version,
             sizeof(s_ota_info_current_version),
             "%s",
             (current_version != 0 && current_version[0] != '\0') ? current_version : "--");
    snprintf(s_ota_info_partition,
             sizeof(s_ota_info_partition),
             "%s",
             (partition_name != 0 && partition_name[0] != '\0') ? partition_name : "--");
    s_ota_show_version_rows = 0U;
    s_ota_show_partition_rows = 1U;
}

/* 灏?OTA 椤甸潰鍒囧洖鏅€氳彍鍗曟ā寮忋€?*/
static void ota_center_enter_menu_mode(void)
{
    s_ota_mode = OTA_CENTER_MODE_MENU;
}

/* 鍒囨崲 OTA 椤甸潰鍒扮‘璁ょ被瀛愮姸鎬侊紝骞朵繚鎸佸師鏈夋暣椤靛埛鏂拌矾寰勪笉鍙樸€?*/
static void ota_center_enter_confirm_mode(ota_center_mode_t mode)
{
    s_ota_mode = mode;
    ui_manager_force_full_refresh();
}

/* 澶嶄綅 OTA 椤甸潰鑿滃崟鎬佸強鍏舵寕璧峰姩浣溿€?*/
static void ota_center_reset_menu_state(void)
{
    ota_center_enter_menu_mode();
    s_ota_pending_action = OTA_PENDING_NONE;
    ota_center_set_notice(0, 0);
}

/* 娓呯悊 OTA 鏌ヨ瀹屾垚鍚庣殑鍚庣画鍔ㄤ綔鏍囪銆?*/
static void ota_center_clear_query_follow_up(void)
{
    s_async_state.ota_post_query_action = OTA_PENDING_NONE;
    s_async_state.ota_query_show_success_info = 0U;
}

/* 娓呯悊 OTA 淇℃伅椤典緷璧栫殑涓存椂鎸傝捣鐘舵€併€?*/
static void ota_center_clear_info_latched_state(void)
{
    s_async_state.ota_wifi_enable_pending = 0U;
    ota_center_clear_query_follow_up();
    s_ota_pending_action = OTA_PENDING_NONE;
    ota_center_set_notice(0, 0);
}

/* 浠?OTA 纭绫诲瓙鐘舵€佽繑鍥炴櫘閫氳彍鍗曪紝骞舵部鐢ㄥ師鏉ョ殑鏁撮〉鍒锋柊鏃舵満銆?*/
static void ota_center_return_to_menu(void)
{
    ota_center_enter_menu_mode();
    ui_manager_force_full_refresh();
}

/* 閫€鍑?OTA 淇℃伅椤碉紝鍙€夋嫨杩斿洖棣栭〉鎴栧洖鍒?OTA 鑿滃崟銆?*/
static void ota_center_exit_info_mode(uint8_t navigate_home)
{
    ota_center_clear_info_latched_state();

    if (navigate_home != 0U)
    {
        ui_manager_navigate_home();
        return;
    }

    ota_center_return_to_menu();
}

/* 鍒囨崲 OTA 椤甸潰鍒颁俊鎭睍绀烘ā寮忥紝骞剁珛鍗宠姹傛暣椤靛埛鏂般€?*/
static void ota_center_show_info_mode(const char *line1, const char *line2)
{
    s_ota_mode = OTA_CENTER_MODE_INFO;
    ota_center_set_notice(line1, line2);
    ui_manager_force_full_refresh();
}

static void ota_center_present_info_now(void)
{
    if (ui_manager_get_active_page() != UI_PAGE_OTA_CENTER)
    {
        return;
    }

    if (app_display_runtime_is_awake() == 0U)
    {
        return;
    }

    (void)app_display_runtime_request_ui_render(ota_center_render, 1U);
}

static void ota_center_show_version_status(const char *status_text,
                                           const char *current_version,
                                           const char *latest_version)
{
    s_ota_mode = OTA_CENTER_MODE_INFO;
    ota_center_set_notice(status_text, 0);
    ota_center_set_version_rows(current_version, latest_version);
    ui_manager_force_full_refresh();
}

static void ota_center_show_local_version_info(void)
{
    s_ota_mode = OTA_CENTER_MODE_INFO;
    ota_center_set_notice("Version Info", 0);
    ota_center_set_local_info_rows(ota_service_get_display_version(),
                                   ota_service_get_partition_name(ota_service_get_active_partition()));
    ui_manager_force_full_refresh();
}

/* 缁熶竴鏄剧ず OTA 椤甸潰鈥滀换鍔″繖鈥濇彁绀猴紝閬垮厤鍥哄畾鏂囨鍒嗘暎鍦ㄥ涓垎鏀腑銆?*/
static void ota_center_show_task_busy_info(void)
{
    ota_center_show_info_mode("Please wait", "Task busy");
}

/* 缁熶竴鏄剧ず OTA 閲嶅惎绫绘彁绀猴紝淇濇寔鍗囩骇鍜屽洖婊氬垎鏀殑鎻愮ず璺緞涓€鑷淬€?*/
static void ota_center_show_restart_info(const char *detail)
{
    ota_center_show_info_mode("Restarting", detail);
}

/* 缁樺埗 OTA 椤甸潰椤堕儴鐨勪俊鎭鍖哄煙銆?*/
static void ota_center_draw_info_rows(void)
{
    char wifi_buffer[24];

    page_format_wifi_status(wifi_buffer, sizeof(wifi_buffer));
    ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y,
                               "WiFi Status",
                               wifi_buffer,
                               BLACK,
                               WHITE);
}

/* 缁樺埗鍗曚釜 OTA 鑿滃崟椤广€?*/
static void ota_center_draw_menu_item(uint8_t index)
{
    if (index >= OTA_ITEM_COUNT)
    {
        return;
    }

    ui_renderer_draw_list_item(page_list_item_y(OTA_LIST_START_Y, index),
                               s_ota_items[index],
                               (s_ota_selected == index) ? 1U : 0U,
                               1U,
                               WHITE);
}

/* 閲嶇粯鍏ㄩ儴 OTA 鑿滃崟椤广€?*/
static void ota_center_draw_menu_items(void)
{
    uint8_t index = 0U;

    for (index = 0U; index < OTA_ITEM_COUNT; ++index)
    {
        ota_center_draw_menu_item(index);
    }
}

/* 寮傛鏌ヨ鏈€鏂?OTA 鐗堟湰淇℃伅銆?*/
static uint8_t ota_center_request_latest_async(uint8_t show_success_info, ota_pending_action_t post_action)
{
    app_service_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_OTA_QUERY_LATEST;

    if (s_async_state.ota_query_pending != 0U)
    {
        s_async_state.ota_query_show_success_info = (show_success_info != 0U) ? 1U : 0U;
        s_async_state.ota_post_query_action = post_action;
        s_async_state.ota_query_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_OTA_MS);
        (void)app_service_submit_async(&cmd);
        return 1U;
    }
    ota_center_show_info_mode("Checking", "Please wait");
    ota_center_present_info_now();

    if (app_service_submit_async(&cmd) == 0U)
    {
        return 0U;
    }

    memset(s_ota_latest_version, 0, sizeof(s_ota_latest_version));
    s_async_state.ota_query_pending = 1U;
    s_async_state.ota_query_deadline_ms = page_async_make_deadline(PAGE_ASYNC_TIMEOUT_OTA_MS);
    s_async_state.ota_query_show_success_info = (show_success_info != 0U) ? 1U : 0U;
    s_async_state.ota_post_query_action = post_action;
    return 1U;
}

/* 鏍规嵁褰撳墠鐗堟湰淇℃伅鎺ㄨ繘 OTA 鍗囩骇纭娴佺▼銆?*/
static uint8_t ota_center_start_upgrade_flow(void)
{
    if ((s_ota_latest_version[0] == '\0') ||
        (ota_service_compare_version(s_ota_latest_version,
                                     ota_service_get_display_version()) <= 0))
    {
        if (ota_center_request_latest_async(0U, OTA_PENDING_UPGRADE) == 0U)
        {
            ota_center_show_task_busy_info();
            return 0U;
        }

        return 0U;
    }

    ota_center_enter_confirm_mode(OTA_CENTER_MODE_CONFIRM_UPGRADE);
    return 1U;
}

/* 缁熶竴澶勭悊 OTA 鏌ヨ鎴愬姛鍚庣殑鐗堟湰姣旇緝銆佺‘璁ゆ祦绋嬪拰淇℃伅椤佃烦杞矾寰勩€?*/
static void ota_center_handle_query_success(const app_service_rsp_t *rsp,
                                            ota_pending_action_t post_action,
                                            uint8_t show_success_info)
{
    snprintf(s_ota_latest_version, sizeof(s_ota_latest_version), "%s", rsp->text);

    if (ota_service_compare_version(s_ota_latest_version,
                                    ota_service_get_display_version()) > 0)
    {
        if (post_action == OTA_PENDING_UPGRADE)
        {
            ota_center_enter_confirm_mode(OTA_CENTER_MODE_CONFIRM_UPGRADE);
            return;
        }

        if (show_success_info != 0U)
        {
            ota_center_show_version_status("Found new version",
                                           ota_service_get_display_version(),
                                           s_ota_latest_version);
        }
        else
        {
            ota_center_return_to_menu();
        }
        return;
    }

    ota_center_show_version_status("Up to date version",
                                   ota_service_get_display_version(),
                                   ota_service_get_display_version());
}

/* 缁熶竴澶勭悊 OTA 鏌ヨ澶辫触鍚庣殑閿欒鏂囨鏄犲皠锛屼繚鎸佸師鏈夐敊璇爜鍒版彁绀烘枃鏈殑瀵瑰簲鍏崇郴銆?*/
static void ota_center_handle_query_failure(const app_service_rsp_t *rsp)
{
    if (rsp->reason == OTA_CTRL_ERR_NO_UPDATE)
    {
        ota_center_show_version_status("Up to date version",
                                       ota_service_get_display_version(),
                                       ota_service_get_display_version());
    }
    else if (rsp->reason == OTA_CTRL_ERR_NO_WIFI)
    {
        ota_center_show_info_mode("WiFi not ready", "Try again");
    }
    else if (rsp->reason == OTA_CTRL_ERR_BUSY)
    {
        if (rsp->value != 0U)
        {
            ota_center_show_info_mode("Module syncing", "Retry soon");
        }
        else
        {
            ota_center_show_info_mode("Please wait", "Device busy");
        }
    }
    else
    {
        ota_center_show_info_mode("Check failed", ota_service_reason_text(rsp->reason));
    }
}

static const char *ota_center_child_title(void)
{
    if (s_ota_mode == OTA_CENTER_MODE_CONFIRM_UPGRADE)
    {
        return "Start Update";
    }

    if (s_ota_mode == OTA_CENTER_MODE_CONFIRM_ROLLBACK)
    {
        return "Restore Previous Version";
    }

    if (s_ota_pending_action == OTA_PENDING_UPGRADE)
    {
        return "Start Update";
    }

    if (s_ota_pending_action == OTA_PENDING_CHECK)
    {
        return "Check Now";
    }

    if (s_ota_selected < OTA_ITEM_COUNT)
    {
        return s_ota_items[s_ota_selected];
    }

    return "Version Info";
}

/* 鎸夊綋鍓嶆椿鍔ㄩ〉闈㈠埛鏂板彈涓绘満鐘舵€佸奖鍝嶇殑鐣岄潰鍖哄煙銆?*/
static void page_refresh_host_status_views(ui_page_id_t active_page)
{
    if (active_page == UI_PAGE_CONNECTIVITY)
    {
        wifi_draw_status_row(0U);
        wifi_draw_item(0U);
    }
    else if (active_page == UI_PAGE_OTA_CENTER && s_ota_mode == OTA_CENTER_MODE_MENU)
    {
        ota_center_draw_info_rows();
    }
    else if (active_page == UI_PAGE_POWER)
    {
        power_draw_item(2U);
    }
}

/* 澶勭悊 WiFi 寮€鍏冲懡浠ょ殑寮傛鍝嶅簲銆?*/
static void page_handle_wifi_set_response(const app_service_rsp_t *rsp)
{
    device_settings_t updated;
    ota_pending_action_t pending_action = s_ota_pending_action;
    ui_page_id_t active_page = ui_manager_get_active_page();

    s_async_state.wifi_set_pending = 0U;
    s_async_state.wifi_set_deadline_ms = 0U;
    if (rsp->ok != 0U)
    {
        app_rtos_settings_copy(&updated);
        updated.wifi_enabled = s_async_state.wifi_target_enabled;
        (void)page_store_settings(&updated);
        (void)page_refresh_host_status_async();
    }

    if (s_async_state.ota_wifi_enable_pending != 0U)
    {
        s_async_state.ota_wifi_enable_pending = 0U;
        s_ota_pending_action = OTA_PENDING_NONE;

        if (rsp->ok == 0U)
        {
            ota_center_show_info_mode("WiFi error", "Try again");
            return;
        }

        if (pending_action == OTA_PENDING_CHECK)
        {
            if (ota_center_request_latest_async(1U, OTA_PENDING_CHECK) == 0U)
            {
                ota_center_show_task_busy_info();
            }
            return;
        }

        if (pending_action == OTA_PENDING_UPGRADE)
        {
            (void)ota_center_start_upgrade_flow();
            return;
        }

        ota_center_return_to_menu();
        return;
    }

    page_refresh_host_status_views(active_page);
}

static void page_handle_ble_set_response(const app_service_rsp_t *rsp)
{
    device_settings_t updated;

    s_async_state.ble_set_pending = 0U;
    s_async_state.ble_set_deadline_ms = 0U;
    if (rsp->ok == 0U)
    {
        return;
    }

    app_rtos_settings_copy(&updated);
    updated.ble_enabled = s_async_state.ble_target_enabled;
    (void)page_store_settings(&updated);
    page_refresh_host_status_views(ui_manager_get_active_page());
}

static void page_handle_mqtt_set_response(const app_service_rsp_t *rsp)
{
    device_settings_t updated;

    s_async_state.mqtt_set_pending = 0U;
    s_async_state.mqtt_set_deadline_ms = 0U;
    if (rsp->ok == 0U)
    {
        return;
    }

    app_rtos_settings_copy(&updated);
    updated.mqtt_enabled = s_async_state.mqtt_target_enabled;
    (void)page_store_settings(&updated);
    page_refresh_host_status_views(ui_manager_get_active_page());
}

/* 澶勭悊璋冭瘯灞忓箷寮€鍏冲懡浠ょ殑寮傛鍝嶅簲銆?*/
static void page_handle_debug_screen_response(const app_service_rsp_t *rsp)
{
    device_settings_t updated;

    s_async_state.debug_screen_pending = 0U;
    s_async_state.debug_screen_deadline_ms = 0U;
    if (rsp->ok == 0U)
    {
        return;
    }

    app_rtos_settings_copy(&updated);
    updated.esp32_debug_screen_enabled = s_async_state.debug_screen_target_enabled;
    (void)page_store_settings(&updated);

    if (ui_manager_get_active_page() == UI_PAGE_ENGINEERING)
    {
        engineering_draw_item(1U);
    }
}

/* 澶勭悊閬ユ帶鎸夐敭寮€鍏冲懡浠ょ殑寮傛鍝嶅簲銆?*/
static void page_handle_remote_keys_response(const app_service_rsp_t *rsp)
{
    device_settings_t updated;

    s_async_state.remote_keys_pending = 0U;
    s_async_state.remote_keys_deadline_ms = 0U;
    if (rsp->ok == 0U)
    {
        return;
    }

    app_rtos_settings_copy(&updated);
    updated.esp32_remote_keys_enabled = s_async_state.remote_keys_target_enabled;
    (void)page_store_settings(&updated);

    if (ui_manager_get_active_page() == UI_PAGE_ENGINEERING)
    {
        engineering_draw_item(2U);
    }
}

/* 澶勭悊涓绘満鐘舵€佸悓姝ュ懡浠ょ殑寮傛鍝嶅簲銆?*/
static void page_handle_host_state_response(const app_service_rsp_t *rsp)
{
    (void)rsp;
    s_async_state.host_state_pending = 0U;
    s_async_state.host_state_deadline_ms = 0U;
}

/* 澶勭悊鐢垫簮绛栫暐鍚屾鍛戒护鐨勫紓姝ュ搷搴斻€?*/
static void page_handle_power_policy_response(const app_service_rsp_t *rsp)
{
    if (s_async_state.power_policy_pending == 0U)
    {
        return;
    }

    s_async_state.power_policy_pending = 0U;
    s_async_state.power_policy_deadline_ms = 0U;
    if (rsp->ok == 0U)
    {
        return;
    }

    (void)page_set_host_state_async(power_manager_get_state());
    if (ui_manager_get_active_page() == UI_PAGE_POWER)
    {
        power_draw_info_rows();
        power_draw_item(0U);
    }
}

/* 澶勭悊寮哄埗娣辩潯鍛戒护鐨勫紓姝ュ搷搴斻€?*/
static void page_handle_forced_deep_sleep_response(const app_service_rsp_t *rsp)
{
    (void)rsp;
    s_async_state.forced_deep_sleep_pending = 0U;
    s_async_state.forced_deep_sleep_deadline_ms = 0U;

    if (ui_manager_get_active_page() == UI_PAGE_POWER)
    {
        power_draw_item(3U);
    }
}

/* 澶勭悊 OTA 鏈€鏂扮増鏈煡璇㈠懡浠ょ殑寮傛鍝嶅簲銆?*/
static void page_handle_ota_query_response(const app_service_rsp_t *rsp)
{
    ota_pending_action_t post_action = s_async_state.ota_post_query_action;
    uint8_t show_success_info = s_async_state.ota_query_show_success_info;

    s_async_state.ota_query_pending = 0U;
    s_async_state.ota_query_deadline_ms = 0U;
    ota_center_clear_query_follow_up();

    if (rsp->ok != 0U)
    {
        ota_center_handle_query_success(rsp, post_action, show_success_info);
        return;
    }

    ota_center_handle_query_failure(rsp);
}

/* 鎸夊懡浠ょ被鍨嬫妸鏈嶅姟鍝嶅簲鍒嗗彂缁欏悇椤甸潰鍐呴儴澶勭悊鍣ㄣ€?*/
static void page_handle_service_response(const app_service_rsp_t *rsp)
{
    if (rsp == 0)
    {
        return;
    }

    switch (rsp->cmd_id)
    {
    case APP_SERVICE_CMD_SET_WIFI:
        page_handle_wifi_set_response(rsp);
        break;

    case APP_SERVICE_CMD_SET_BLE:
        page_handle_ble_set_response(rsp);
        break;

    case APP_SERVICE_CMD_SET_MQTT:
        page_handle_mqtt_set_response(rsp);
        break;

    case APP_SERVICE_CMD_SET_DEBUG_SCREEN:
        page_handle_debug_screen_response(rsp);
        break;

    case APP_SERVICE_CMD_SET_REMOTE_KEYS:
        page_handle_remote_keys_response(rsp);
        break;

    case APP_SERVICE_CMD_SET_HOST_STATE:
        page_handle_host_state_response(rsp);
        break;

    case APP_SERVICE_CMD_SET_POWER_POLICY:
        page_handle_power_policy_response(rsp);
        break;

    case APP_SERVICE_CMD_ENTER_FORCED_DEEP_SLEEP:
        page_handle_forced_deep_sleep_response(rsp);
        break;

    case APP_SERVICE_CMD_OTA_QUERY_LATEST:
        page_handle_ota_query_response(rsp);
        break;

    case APP_SERVICE_CMD_ESP_REFRESH_STATUS:
        page_refresh_host_status_views(ui_manager_get_active_page());
        break;

    case APP_SERVICE_CMD_NONE:
    default:
        break;
    }
}

/*
 * 瓒呮椂澶勭悊鍙礋璐ｉ噴鏀鹃〉闈晶鐨?pending/UI 绛夊緟鐘舵€併€? * 鍚庡彴鏈嶅姟浠诲姟浠嶅彲鑳界◢鍚庡畬鎴愶紝杩欓噷涓嶈兘鎶娾€滅晫闈㈣秴鏃垛€濊褰撴垚鈥滀换鍔¤鍙栨秷鈥濄€? */
static void page_refresh_timeout_views(ui_page_id_t active_page)
{
    if (active_page == UI_PAGE_CONNECTIVITY)
    {
        wifi_draw_status_row(0U);
        wifi_draw_item(0U);
    }
    else if (active_page == UI_PAGE_ENGINEERING)
    {
        engineering_draw_item(0U);
        engineering_draw_item(1U);
        engineering_draw_item(2U);
    }
    else if (active_page == UI_PAGE_POWER)
    {
        power_draw_item(2U);
    }
}

/* 鎵弿鎵€鏈夐〉闈晶寮傛 pending 椤癸紝骞跺湪瓒呮椂鍚庢仮澶嶉〉闈㈡樉绀恒€?*/
static void page_async_handle_timeouts(void)
{
    uint32_t now_ms = power_manager_get_tick_ms();
    ui_page_id_t active_page = ui_manager_get_active_page();
    uint8_t timed_out = 0U;

    if (s_async_state.wifi_set_pending != 0U &&
        page_async_deadline_expired(now_ms, s_async_state.wifi_set_deadline_ms) != 0U)
    {
        s_async_state.wifi_set_pending = 0U;
        s_async_state.wifi_set_deadline_ms = 0U;
        timed_out = 1U;

        if (s_async_state.ota_wifi_enable_pending != 0U)
        {
            s_async_state.ota_wifi_enable_pending = 0U;
            s_ota_pending_action = OTA_PENDING_NONE;
            if (active_page == UI_PAGE_OTA_CENTER)
            {
                ota_center_show_info_mode("WiFi timeout", "Try again");
            }
        }
    }

    if (s_async_state.debug_screen_pending != 0U &&
        page_async_deadline_expired(now_ms, s_async_state.debug_screen_deadline_ms) != 0U)
    {
        s_async_state.debug_screen_pending = 0U;
        s_async_state.debug_screen_deadline_ms = 0U;
        timed_out = 1U;
    }

    if (s_async_state.ble_set_pending != 0U &&
        page_async_deadline_expired(now_ms, s_async_state.ble_set_deadline_ms) != 0U)
    {
        s_async_state.ble_set_pending = 0U;
        s_async_state.ble_set_deadline_ms = 0U;
        timed_out = 1U;
    }

    if (s_async_state.mqtt_set_pending != 0U &&
        page_async_deadline_expired(now_ms, s_async_state.mqtt_set_deadline_ms) != 0U)
    {
        s_async_state.mqtt_set_pending = 0U;
        s_async_state.mqtt_set_deadline_ms = 0U;
        timed_out = 1U;
    }

    if (s_async_state.remote_keys_pending != 0U &&
        page_async_deadline_expired(now_ms, s_async_state.remote_keys_deadline_ms) != 0U)
    {
        s_async_state.remote_keys_pending = 0U;
        s_async_state.remote_keys_deadline_ms = 0U;
        timed_out = 1U;
    }

    if (s_async_state.host_state_pending != 0U &&
        page_async_deadline_expired(now_ms, s_async_state.host_state_deadline_ms) != 0U)
    {
        s_async_state.host_state_pending = 0U;
        s_async_state.host_state_deadline_ms = 0U;
        timed_out = 1U;
    }

    if (s_async_state.power_policy_pending != 0U &&
        page_async_deadline_expired(now_ms, s_async_state.power_policy_deadline_ms) != 0U)
    {
        s_async_state.power_policy_pending = 0U;
        s_async_state.power_policy_deadline_ms = 0U;
        timed_out = 1U;
    }

    if (s_async_state.forced_deep_sleep_pending != 0U &&
        page_async_deadline_expired(now_ms, s_async_state.forced_deep_sleep_deadline_ms) != 0U)
    {
        s_async_state.forced_deep_sleep_pending = 0U;
        s_async_state.forced_deep_sleep_deadline_ms = 0U;
        timed_out = 1U;
    }

    if (s_async_state.ota_query_pending != 0U &&
        page_async_deadline_expired(now_ms, s_async_state.ota_query_deadline_ms) != 0U)
    {
        s_async_state.ota_query_pending = 0U;
        s_async_state.ota_query_deadline_ms = 0U;
        ota_center_clear_query_follow_up();
        if (active_page == UI_PAGE_OTA_CENTER)
        {
            ota_center_show_info_mode("Check timeout", "Try again");
        }
        timed_out = 1U;
    }

    if (timed_out == 0U)
    {
        return;
    }

    page_refresh_timeout_views(active_page);
    ui_manager_request_render();
}

/* 閲嶇粯棣栭〉鍏ㄩ儴鑿滃崟椤广€?*/
static uint16_t home_card_y(uint8_t index)
{
    return (uint16_t)(HOME_LIST_START_Y + ((uint16_t)index * (HOME_CARD_HEIGHT + HOME_CARD_GAP)));
}

static void home_draw_chevron(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_DrawLine(x, y, (uint16_t)(x + 7U), (uint16_t)(y + 7U), color);
    LCD_DrawLine((uint16_t)(x + 7U), (uint16_t)(y + 7U), x, (uint16_t)(y + 14U), color);
}

static void home_draw_icon(uint8_t index, uint16_t x, uint16_t y, uint16_t color)
{
    switch (index)
    {
    case 0U:
        LCD_DrawRectangle((uint16_t)(x + 3U), (uint16_t)(y + 2U), (uint16_t)(x + 15U), (uint16_t)(y + 18U), color);
        LCD_DrawRectangle((uint16_t)(x + 7U), (uint16_t)(y + 4U), (uint16_t)(x + 11U), (uint16_t)(y + 8U), color);
        LCD_DrawLine((uint16_t)(x + 9U), (uint16_t)(y + 9U), (uint16_t)(x + 9U), (uint16_t)(y + 14U), color);
        LCD_DrawLine((uint16_t)(x + 7U), (uint16_t)(y + 13U), (uint16_t)(x + 11U), (uint16_t)(y + 13U), color);
        LCD_DrawLine((uint16_t)(x + 6U), (uint16_t)(y + 19U), (uint16_t)(x + 12U), (uint16_t)(y + 19U), color);
        break;

    case 1U:
        Draw_Circle((uint16_t)(x + 9U), (uint16_t)(y + 9U), 5U, color);
        LCD_DrawLine((uint16_t)(x + 9U), (uint16_t)(y + 4U), (uint16_t)(x + 9U), (uint16_t)(y + 12U), color);
        LCD_DrawLine((uint16_t)(x + 6U), (uint16_t)(y + 9U), (uint16_t)(x + 9U), (uint16_t)(y + 12U), color);
        LCD_DrawLine((uint16_t)(x + 12U), (uint16_t)(y + 9U), (uint16_t)(x + 9U), (uint16_t)(y + 12U), color);
        LCD_DrawLine((uint16_t)(x + 3U), (uint16_t)(y + 16U), (uint16_t)(x + 15U), (uint16_t)(y + 16U), color);
        break;

    case 2U:
        LCD_DrawLine((uint16_t)(x + 2U), (uint16_t)(y + 10U), (uint16_t)(x + 9U), (uint16_t)(y + 3U), color);
        LCD_DrawLine((uint16_t)(x + 16U), (uint16_t)(y + 10U), (uint16_t)(x + 9U), (uint16_t)(y + 3U), color);
        LCD_DrawLine((uint16_t)(x + 5U), (uint16_t)(y + 12U), (uint16_t)(x + 9U), (uint16_t)(y + 8U), color);
        LCD_DrawLine((uint16_t)(x + 13U), (uint16_t)(y + 12U), (uint16_t)(x + 9U), (uint16_t)(y + 8U), color);
        LCD_Fill((uint16_t)(x + 8U), (uint16_t)(y + 15U), (uint16_t)(x + 10U), (uint16_t)(y + 17U), color);
        break;

    case 3U:
        LCD_DrawRectangle((uint16_t)(x + 2U), (uint16_t)(y + 5U), (uint16_t)(x + 16U), (uint16_t)(y + 15U), color);
        LCD_DrawRectangle((uint16_t)(x + 17U), (uint16_t)(y + 8U), (uint16_t)(x + 18U), (uint16_t)(y + 12U), color);
        LCD_DrawLine((uint16_t)(x + 5U), (uint16_t)(y + 7U), (uint16_t)(x + 5U), (uint16_t)(y + 13U), color);
        LCD_DrawLine((uint16_t)(x + 9U), (uint16_t)(y + 7U), (uint16_t)(x + 9U), (uint16_t)(y + 13U), color);
        LCD_DrawLine((uint16_t)(x + 13U), (uint16_t)(y + 7U), (uint16_t)(x + 13U), (uint16_t)(y + 13U), color);
        break;

    case 4U:
    default:
        Draw_Circle((uint16_t)(x + 9U), (uint16_t)(y + 10U), 5U, color);
        LCD_DrawLine((uint16_t)(x + 9U), (uint16_t)(y + 1U), (uint16_t)(x + 9U), (uint16_t)(y + 4U), color);
        LCD_DrawLine((uint16_t)(x + 9U), (uint16_t)(y + 16U), (uint16_t)(x + 9U), (uint16_t)(y + 19U), color);
        LCD_DrawLine((uint16_t)(x + 1U), (uint16_t)(y + 10U), (uint16_t)(x + 4U), (uint16_t)(y + 10U), color);
        LCD_DrawLine((uint16_t)(x + 14U), (uint16_t)(y + 10U), (uint16_t)(x + 17U), (uint16_t)(y + 10U), color);
        LCD_DrawLine((uint16_t)(x + 3U), (uint16_t)(y + 4U), (uint16_t)(x + 5U), (uint16_t)(y + 6U), color);
        LCD_DrawLine((uint16_t)(x + 13U), (uint16_t)(y + 14U), (uint16_t)(x + 15U), (uint16_t)(y + 16U), color);
        LCD_DrawLine((uint16_t)(x + 13U), (uint16_t)(y + 6U), (uint16_t)(x + 15U), (uint16_t)(y + 4U), color);
        LCD_DrawLine((uint16_t)(x + 3U), (uint16_t)(y + 16U), (uint16_t)(x + 5U), (uint16_t)(y + 14U), color);
        break;
    }
}

static void home_draw_banner(void)
{
    ui_renderer_draw_page_intro("Main Menu", "Device Entry", PAGE_UI_ACCENT_COLOR);
}

static void home_draw_items(void)
{
    uint8_t index = 0U;

    for (index = 0U; index < HOME_ITEM_COUNT; ++index)
    {
        home_draw_item(index);
    }
}

/* 鏍规嵁绱㈠紩閲嶇粯棣栭〉涓殑鍗曚釜鑿滃崟椤广€?*/
static void home_draw_item(uint8_t index)
{
    const home_menu_item_t *item = 0;
    uint16_t y = 0U;
    uint16_t x1 = HOME_CARD_LEFT;
    uint16_t x2 = HOME_CARD_RIGHT;
    uint16_t y2 = 0U;
    uint16_t accent = PAGE_UI_ACCENT_COLOR;
    uint16_t fill = PAGE_UI_PANEL_COLOR;
    uint16_t border = PAGE_UI_PANEL_EDGE_COLOR;
    uint16_t icon_color = PAGE_UI_CYAN_COLOR;
    uint16_t title_color = WHITE;
    uint16_t subtitle_color = PAGE_UI_SUBTEXT_COLOR;
    uint16_t chevron_color = PAGE_UI_SUBTEXT_COLOR;

    if (index >= HOME_ITEM_COUNT)
    {
        return;
    }

    item = &s_home_items[index];
    y = home_card_y(index);
    y2 = (uint16_t)(y + HOME_CARD_HEIGHT - 1U);
    accent = item->accent;

    if (s_home_selected == index)
    {
        fill = accent;
        border = PAGE_UI_ACCENT_EDGE_COLOR;
        icon_color = WHITE;
        subtitle_color = PAGE_UI_RGB565(244U, 215U, 184U);
        chevron_color = WHITE;
    }

    app_display_runtime_lock();
    LCD_Fill(x1, y, x2, y2, fill);
    LCD_DrawRectangle(x1, y, x2, y2, border);
    home_draw_icon(item->icon, HOME_CARD_ICON_LEFT, (uint16_t)(y + 3U), icon_color);
    LCD_ShowUTF8String(HOME_CARD_TEXT_LEFT,
                       (uint16_t)(y + 5U),
                       ui_renderer_localize(item->title),
                       title_color,
                       fill,
                       16,
                       0);
    LCD_ShowUTF8String(HOME_CARD_SUBTITLE_LEFT,
                       (uint16_t)(y + 5U),
                       ui_renderer_localize(item->subtitle),
                       subtitle_color,
                       fill,
                       16,
                       0);
    home_draw_chevron(HOME_CARD_CHEVRON_LEFT, (uint16_t)(y + 6U), chevron_color);
    app_display_runtime_unlock();
}

/* 缁樺埗 WiFi 鐘舵€佽锛屽苟鍒╃敤缂撳瓨閬垮厤閲嶅閲嶇粯銆?*/
static void wifi_draw_status_row(uint8_t force_refresh)
{
    static const char * const s_status_labels[3] =
    {
        "WiFi Connection",
        "Bluetooth Connection",
        "Cloud Connection"
    };
    char value_buffer[24];
    uint16_t value_color = BLACK;
    uint8_t row_index = 0U;

    for (row_index = 0U; row_index < 3U; ++row_index)
    {
        if (row_index == 0U)
        {
            page_format_wifi_status(value_buffer, sizeof(value_buffer));
        }
        else if (row_index == 1U)
        {
            page_format_ble_status(value_buffer, sizeof(value_buffer));
        }
        else
        {
            page_format_cloud_status(value_buffer, sizeof(value_buffer));
        }

        value_color = BLACK;
        if (strcmp(value_buffer, "CONNECTED") == 0)
        {
            value_color = GREEN;
        }
        else if (strcmp(value_buffer, "CONNECTING") == 0 ||
                 strcmp(value_buffer, "WAIT") == 0 ||
                 strcmp(value_buffer, "WORKING") == 0)
        {
            value_color = YELLOW;
        }
        else if (strcmp(value_buffer, "NOT CONNECTED") == 0 ||
                 strcmp(value_buffer, "OFFLINE") == 0 ||
                 strcmp(value_buffer, "OFF") == 0)
        {
            value_color = RED;
        }

        if (force_refresh == 0U &&
            s_connectivity_status_cache_valid[row_index] != 0U &&
            s_connectivity_status_color_cache[row_index] == value_color &&
            strcmp(s_connectivity_status_cache[row_index], value_buffer) == 0)
        {
            continue;
        }

        ui_renderer_draw_value_row((uint16_t)(PAGE_INFO_ROW1_Y + ((uint16_t)row_index * 24U)),
                                   s_status_labels[row_index],
                                   value_buffer,
                                   value_color,
                                   WHITE);
        snprintf(s_connectivity_status_cache[row_index],
                 sizeof(s_connectivity_status_cache[row_index]),
                 "%s",
                 value_buffer);
        s_connectivity_status_color_cache[row_index] = value_color;
        s_connectivity_status_cache_valid[row_index] = 1U;
    }
}

/* 缁樺埗 WiFi 寮€鍏抽」锛屽苟鍒╃敤缂撳瓨閬垮厤閲嶅閲嶇粯銆?*/
static void wifi_draw_item(uint8_t force_refresh)
{
    uint8_t index = 0U;

    for (index = 0U; index < WIFI_ITEM_COUNT; ++index)
    {
        device_settings_t settings;
        const char *label = "WiFi";
        uint8_t selected = (s_wifi_selected == index) ? 1U : 0U;
        uint8_t enabled = 0U;
        uint8_t pending = 0U;

        app_rtos_settings_copy(&settings);

        if (index == 0U)
        {
            label = (esp_host_is_forced_deep_sleep() != 0U) ? "WiFi(KEY6)" :
                    ((s_async_state.wifi_set_pending != 0U) ? "WiFi..." : "WiFi");
            enabled = (s_async_state.wifi_set_pending != 0U) ? s_async_state.wifi_target_enabled : settings.wifi_enabled;
            pending = s_async_state.wifi_set_pending;
        }
        else if (index == 1U)
        {
            label = (s_async_state.ble_set_pending != 0U) ? "Bluetooth..." : "Bluetooth";
            enabled = (s_async_state.ble_set_pending != 0U) ? s_async_state.ble_target_enabled : settings.ble_enabled;
            pending = s_async_state.ble_set_pending;
        }
        else
        {
            label = (s_async_state.mqtt_set_pending != 0U) ? "Server..." : "Server";
            enabled = (s_async_state.mqtt_set_pending != 0U) ? s_async_state.mqtt_target_enabled : settings.mqtt_enabled;
            pending = s_async_state.mqtt_set_pending;
        }

        if (force_refresh == 0U &&
            s_connectivity_item_cache_valid[index] != 0U &&
            s_connectivity_item_enabled_cache[index] == enabled &&
            s_connectivity_item_selected_cache[index] == selected &&
            s_connectivity_item_pending_cache[index] == pending &&
            strcmp(s_connectivity_item_label_cache[index], label) == 0)
        {
            continue;
        }

        ui_renderer_draw_toggle_item(page_list_item_y(WIFI_LIST_START_Y, index),
                                     label,
                                     enabled,
                                     selected,
                                     (pending != 0U) ? LGRAY : WHITE);
        snprintf(s_connectivity_item_label_cache[index],
                 sizeof(s_connectivity_item_label_cache[index]),
                 "%s",
                 label);
        s_connectivity_item_enabled_cache[index] = enabled;
        s_connectivity_item_selected_cache[index] = selected;
        s_connectivity_item_pending_cache[index] = pending;
        s_connectivity_item_cache_valid[index] = 1U;
    }
}

static void connectivity_draw_item(uint8_t index)
{
    if (index < WIFI_ITEM_COUNT)
    {
        s_connectivity_item_cache_valid[index] = 0U;
    }
    wifi_draw_item(0U);
}

/* 缁樺埗鐢垫簮椤甸潰椤堕儴淇℃伅琛屻€?*/
static void power_draw_info_rows(void)
{
    power_draw_battery_status();
}

static void power_draw_battery_status(void)
{
    char value_buffer[12];
    uint8_t percent = battery_monitor_get_percent();
    uint16_t value_color = GREEN;

    if (percent < 30U)
    {
        value_color = RED;
    }
    else if (percent < 60U)
    {
        value_color = YELLOW;
    }

    page_format_battery(value_buffer, sizeof(value_buffer));
    ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y,
                               "Battery Level",
                               value_buffer,
                               value_color,
                               WHITE);
}

/* 鏍规嵁绱㈠紩缁樺埗鐢垫簮椤甸潰涓殑鍗曚釜鑿滃崟椤广€?*/
static void power_draw_item(uint8_t index)
{
    device_settings_t settings;
    uint16_t item_y = page_list_item_y(POWER_LIST_START_Y, index);
    const char *value_text = "Off";

    app_rtos_settings_copy(&settings);

    if (index >= POWER_ITEM_COUNT)
    {
        return;
    }

    if (index == 0U)
    {
        char value_buffer[24];

        page_format_timeout_ms(value_buffer, sizeof(value_buffer), settings.screen_off_timeout_ms);
        ui_renderer_draw_option_item(item_y,
                                     s_power_items[0],
                                     value_buffer,
                                     (s_power_selected == 0U) ? 1U : 0U,
                                     WHITE);
    }
    else if (index == 1U)
    {
        ui_renderer_draw_toggle_item(item_y,
                                     s_power_items[1],
                                     settings.standby_enabled,
                                     (s_power_selected == 1U) ? 1U : 0U,
                                     WHITE);
    }
    else if (index == 2U)
    {
        if (s_async_state.forced_deep_sleep_pending != 0U)
        {
            value_text = "WAIT";
        }
        else if (esp_host_is_forced_deep_sleep() != 0U)
        {
            value_text = "KEY6";
        }

        ui_renderer_draw_option_item(item_y,
                                     s_power_items[2],
                                     value_text,
                                     (s_power_selected == 2U) ? 1U : 0U,
                                     WHITE);
    }
}

/* 閲嶇粯鐢垫簮椤甸潰鍏ㄩ儴鑿滃崟椤广€?*/
static void power_draw_items(void)
{
    uint8_t index = 0U;

    for (index = 0U; index < POWER_ITEM_COUNT; ++index)
    {
        power_draw_item(index);
    }
}

/* 缁樺埗绯荤粺椤甸潰椤堕儴淇℃伅琛屻€?*/
/* 鏍规嵁绱㈠紩缁樺埗绯荤粺椤甸潰涓殑鍗曚釜鑿滃崟椤广€?*/
static void system_draw_item(uint8_t index)
{
    device_settings_t settings;
    uint16_t item_y = page_list_item_y(SYSTEM_LIST_START_Y, index);

    app_rtos_settings_copy(&settings);

#if (REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ENABLE != 0U)
    if (index == SYSTEM_ITEM_THERMAL_PAUSE_SEND)
    {
        ui_renderer_draw_toggle_item(item_y,
                                     s_system_items[0],
                                     settings.thermal_pause_send_esp_enabled,
                                     (s_system_selected == SYSTEM_ITEM_THERMAL_PAUSE_SEND) ? 1U : 0U,
                                     WHITE);
    }
    else if (index == SYSTEM_ITEM_DEBUG_MODE)
    {
        ui_renderer_draw_toggle_item(item_y,
                                     s_system_items[3],
                                     settings.debug_mode_enabled,
                                     (s_system_selected == SYSTEM_ITEM_DEBUG_MODE) ? 1U : 0U,
                                     WHITE);
    }
    else if (index == SYSTEM_ITEM_SD_CARD)
    {
        ui_renderer_draw_list_item(item_y,
                                   s_system_items[1],
                                   (s_system_selected == SYSTEM_ITEM_SD_CARD) ? 1U : 0U,
                                   1U,
                                   WHITE);
    }
    else if (index == SYSTEM_ITEM_KEY2_SNAPSHOT)
    {
        ui_renderer_draw_toggle_item(item_y,
                                     s_system_items[1],
                                     settings.key2_snapshot_enabled,
                                     (s_system_selected == SYSTEM_ITEM_KEY2_SNAPSHOT) ? 1U : 0U,
                                     WHITE);
    }
    else if (index == SYSTEM_ITEM_DEBUG_PAGE)
    {
        if (settings.debug_mode_enabled != 0U)
        {
            ui_renderer_draw_list_item(item_y,
                                       s_system_items[4],
                                       (s_system_selected == SYSTEM_ITEM_DEBUG_PAGE) ? 1U : 0U,
                                       1U,
                                       WHITE);
        }
        else
        {
            ui_renderer_clear_row(item_y, PAGE_UI_BG_COLOR);
        }
    }
#else
    if (index == SYSTEM_ITEM_DEBUG_MODE)
    {
        ui_renderer_draw_toggle_item(item_y,
                                     s_system_items[2],
                                     settings.debug_mode_enabled,
                                     (s_system_selected == SYSTEM_ITEM_DEBUG_MODE) ? 1U : 0U,
                                     WHITE);
    }
    else if (index == SYSTEM_ITEM_SD_CARD)
    {
        ui_renderer_draw_list_item(item_y,
                                   s_system_items[0],
                                   (s_system_selected == SYSTEM_ITEM_SD_CARD) ? 1U : 0U,
                                   1U,
                                   WHITE);
    }
    else if (index == SYSTEM_ITEM_KEY2_SNAPSHOT)
    {
        ui_renderer_draw_toggle_item(item_y,
                                     s_system_items[0],
                                     settings.key2_snapshot_enabled,
                                     (s_system_selected == SYSTEM_ITEM_KEY2_SNAPSHOT) ? 1U : 0U,
                                     WHITE);
    }
    else if (index == SYSTEM_ITEM_DEBUG_PAGE)
    {
        if (settings.debug_mode_enabled != 0U)
        {
            ui_renderer_draw_list_item(item_y,
                                       s_system_items[3],
                                       (s_system_selected == SYSTEM_ITEM_DEBUG_PAGE) ? 1U : 0U,
                                       1U,
                                       WHITE);
        }
        else
        {
            ui_renderer_clear_row(item_y, PAGE_UI_BG_COLOR);
        }
    }
#endif
}

/* 閲嶇粯绯荤粺椤甸潰鍏ㄩ儴鑿滃崟椤广€?*/
static void system_draw_items(void)
{
    uint8_t index;

    for (index = 0U; index < SYSTEM_ITEM_MAX_COUNT; ++index)
    {
        system_draw_item(index);
    }
}

static void storage_page_set_notice(const char *line1, const char *line2)
{
    snprintf(s_storage_notice_line1,
             sizeof(s_storage_notice_line1),
             "%s",
             (line1 != 0) ? line1 : "");
    snprintf(s_storage_notice_line2,
             sizeof(s_storage_notice_line2),
             "%s",
             (line2 != 0) ? line2 : "");
}

static void storage_page_refresh_state(storage_info_t *info)
{
    storage_info_t local_info;

    memset(&local_info, 0, sizeof(local_info));
    (void)storage_service_get_info(&local_info);

    if (info != 0)
    {
        *info = local_info;
    }
}

static void storage_draw_info_rows(void)
{
    storage_info_t info;
    char value[24];

    storage_page_refresh_state(&info);

    snprintf(value, sizeof(value), "%s", info.card_present != 0U ? "READY" : "NOT READY");
    ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y, "Card", value, PAGE_UI_CYAN_COLOR, WHITE);

    snprintf(value, sizeof(value), "%s", info.mounted != 0U ? "MOUNTED" : "NOT MOUNTED");
    ui_renderer_draw_value_row(PAGE_INFO_ROW2_Y, "Mount", value, PAGE_UI_CYAN_COLOR, WHITE);

    if (info.total_kb != 0U)
    {
        snprintf(value, sizeof(value), "%lu MB", (unsigned long)(info.total_kb / 1024UL));
    }
    else
    {
        snprintf(value, sizeof(value), "--");
    }
    ui_renderer_draw_value_row(PAGE_INFO_ROW3_Y, "Capacity", value, PAGE_UI_CYAN_COLOR, WHITE);

    if (info.free_kb != 0U)
    {
        snprintf(value, sizeof(value), "%lu MB", (unsigned long)(info.free_kb / 1024UL));
    }
    else
    {
        snprintf(value, sizeof(value), "--");
    }
    ui_renderer_draw_value_row(PAGE_INFO_ROW4_Y, "Free", value, PAGE_UI_CYAN_COLOR, WHITE);
}

static void storage_draw_item(uint8_t index)
{
    uint16_t item_y = page_list_item_y(WIFI_LIST_START_Y, index);

    ui_renderer_draw_list_item(item_y,
                               s_storage_items[index],
                               (s_storage_selected == index) ? 1U : 0U,
                               1U,
                               WHITE);
}

static void storage_draw_items(void)
{
    uint8_t index = 0U;

    for (index = 0U; index < STORAGE_ITEM_COUNT; ++index)
    {
        storage_draw_item(index);
    }
}

static void snapshot_review_draw_info(void)
{
    char line[48];

    if (s_snapshot_review_loaded == 0U)
    {
        ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y,
                                   "Info",
                                   storage_service_status_text(s_snapshot_review_status),
                                   PAGE_UI_CYAN_COLOR,
                                   WHITE);
        return;
    }

    snprintf(line, sizeof(line), "#%06lu", (unsigned long)s_snapshot_review_index);
    ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y, "Snapshot", line, PAGE_UI_CYAN_COLOR, WHITE);

    snprintf(line, sizeof(line), "%d.%dC", s_snapshot_review_snapshot.min_x10 / 10, s_snapshot_review_snapshot.min_x10 % 10);
    ui_renderer_draw_value_row(PAGE_INFO_ROW2_Y, "Min", line, PAGE_UI_CYAN_COLOR, WHITE);

    snprintf(line, sizeof(line), "%d.%dC", s_snapshot_review_snapshot.max_x10 / 10, s_snapshot_review_snapshot.max_x10 % 10);
    ui_renderer_draw_value_row(PAGE_INFO_ROW3_Y, "Max", line, PAGE_UI_CYAN_COLOR, WHITE);

    snprintf(line, sizeof(line), "%d.%dC", s_snapshot_review_snapshot.center_x10 / 10, s_snapshot_review_snapshot.center_x10 % 10);
    ui_renderer_draw_value_row(PAGE_INFO_ROW4_Y, "Center", line, PAGE_UI_CYAN_COLOR, WHITE);
}

/* 鏍规嵁绱㈠紩缁樺埗宸ョ▼椤甸潰涓殑鍗曚釜鑿滃崟椤广€?*/
static void engineering_draw_item(uint8_t index)
{
    device_settings_t settings;
    uint16_t item_y = page_list_item_y(DEBUG_LIST_START_Y, index);

    app_rtos_settings_copy(&settings);

    if (index >= ENGINEERING_ITEM_COUNT)
    {
        return;
    }

    if (index == 0U)
    {
        ui_renderer_draw_list_item(item_y,
                                   s_engineering_items[index],
                                   (s_engineering_selected == index) ? 1U : 0U,
                                   1U,
                                   WHITE);
        return;
    }

    ui_renderer_draw_toggle_item(item_y,
                                 s_engineering_items[index],
                                 (index == 1U) ? settings.esp32_debug_screen_enabled :
                                                 settings.esp32_remote_keys_enabled,
                                 (s_engineering_selected == index) ? 1U : 0U,
                                 WHITE);
}

/* 閲嶇粯宸ョ▼椤甸潰鍏ㄩ儴鑿滃崟椤广€?*/
static void engineering_draw_items(void)
{
    uint8_t index = 0U;

    for (index = 0U; index < ENGINEERING_ITEM_COUNT; ++index)
    {
        engineering_draw_item(index);
    }
}

/* 棣栭〉杩涘叆鍥炶皟锛岀洰鍓嶄笉闇€瑕侀澶栫姸鎬佸垵濮嬪寲銆?*/
static void home_on_enter(ui_page_id_t previous_page)
{
    (void)previous_page;
}

/* 棣栭〉绂诲紑鍥炶皟锛岀洰鍓嶄粎淇濈暀缁熶竴鎺ュ彛銆?*/
static void home_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
}

/* 澶勭悊棣栭〉鎸夐敭锛氱Щ鍔ㄧ劍鐐规垨杩涘叆瀛愰〉闈€€?*/
static void home_on_key(uint8_t key_value)
{
    if (key_value == KEY1_PRES)
    {
        page_move_selection(&s_home_selected, HOME_ITEM_COUNT, 1U, home_draw_item);
    }
    else if (key_value == KEY3_PRES)
    {
        page_move_selection(&s_home_selected, HOME_ITEM_COUNT, 0U, home_draw_item);
    }
    else if (key_value == KEY2_PRES)
    {
        if (s_home_selected < HOME_ITEM_COUNT)
        {
            ui_manager_navigate_to(s_home_items[s_home_selected].target);
        }
    }
}

/* 棣栭〉鍛ㄦ湡鍥炶皟锛屼粎璐熻矗澶勭悊寮傛瓒呮椂銆?*/
static void home_on_tick(void)
{
    page_async_handle_timeouts();
}

/* 娓叉煋棣栭〉闈欐€佸竷灞€涓庤彍鍗曢」銆?*/
static void home_render(uint8_t full_refresh)
{
    if (full_refresh == 0U)
    {
        return;
    }

    ui_renderer_draw_product_background();
    home_draw_banner();
    home_draw_items();
}

/* 鐑垚鍍忛〉闈㈣繘鍏ュ洖璋冿紝鎭㈠鐑垚鍍忚繍琛屽苟寮哄埗鏁撮〉鍒锋柊銆?*/
static void thermal_on_enter(ui_page_id_t previous_page)
{
    (void)previous_page;
    redpic1_thermal_resume();
    ui_manager_force_full_refresh();
}

/* 鐑垚鍍忛〉闈㈢寮€鍥炶皟锛屾殏鍋滅儹鎴愬儚杩愯銆?*/
static void thermal_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
    redpic1_thermal_suspend();
}

/* 澶勭悊鐑垚鍍忛〉闈㈡寜閿紝闀挎寜杩斿洖锛屽叾浣欎氦缁欑儹鎴愬儚瀛愭ā鍧椼€?*/
static void thermal_on_key(uint8_t key_value)
{
    if (key_value == UI_KEY_KEY2_LONG)
    {
        ui_manager_navigate_home();
        return;
    }

    redpic1_thermal_handle_key(key_value);
}

/* 鐑垚鍍忛〉闈㈠懆鏈熷洖璋冿紝浠呰礋璐ｅ鐞嗗紓姝ヨ秴鏃躲€?*/
static void thermal_on_tick(void)
{
    page_async_handle_timeouts();
}

/* 娓叉煋鐑垚鍍忛〉闈㈣儗鏅紝骞惰Е鍙戠儹鎴愬儚妯″潡鑷鍒锋柊銆?*/
static void thermal_render(uint8_t full_refresh)
{
    if (full_refresh != 0U)
    {
        app_display_runtime_lock();
        LCD_Fill(0, 0, LCD_W - 1U, LCD_H - 1U, BLACK);
        app_display_runtime_unlock();
        redpic1_thermal_force_refresh();
    }
}

/* OTA 椤甸潰杩涘叆鍥炶皟锛屽埛鏂板熀纭€淇℃伅骞跺浣嶉〉闈㈡ā寮忋€?*/
static void ota_center_on_enter(ui_page_id_t previous_page)
{
    (void)previous_page;
    ota_service_refresh_info();
    ota_center_reset_menu_state();
}

/* OTA 椤甸潰绂诲紑鍥炶皟锛屾竻鐞嗛〉闈㈡ā寮忎笌鎸傝捣鍔ㄤ綔銆?*/
static void ota_center_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
    ota_center_reset_menu_state();
}

/* 澶勭悊 OTA 椤甸潰鎸夐敭涓庣‘璁ゆ祦绋嬬姸鎬佹満銆?*/
static void ota_center_on_key(uint8_t key_value)
{
    device_settings_t settings;

    app_rtos_settings_copy(&settings);

    if (s_ota_mode == OTA_CENTER_MODE_CONFIRM_WIFI)
    {
        if (key_value == UI_KEY_KEY2_LONG)
        {
            ui_manager_navigate_home();
        }
        else if (key_value == KEY1_PRES)
        {
            ota_center_reset_menu_state();
            ui_manager_force_full_refresh();
        }
        else if (key_value == KEY2_PRES)
        {
            if (page_set_wifi_enabled(1U) == 0U)
            {
                ota_center_show_task_busy_info();
                return;
            }
            s_async_state.ota_wifi_enable_pending = 1U;
            ota_center_show_info_mode("Enabling WiFi", "Please wait");
            ota_center_present_info_now();
        }
        return;
    }

    if (s_ota_mode == OTA_CENTER_MODE_CONFIRM_UPGRADE)
    {
        if (key_value == UI_KEY_KEY2_LONG)
        {
            ui_manager_navigate_home();
        }
        else if (key_value == KEY1_PRES)
        {
            ota_center_return_to_menu();
        }
        else if (key_value == KEY2_PRES)
        {
            ota_center_show_restart_info("Start update");
            delay_ms(200U);
            ota_service_request_upgrade();
        }
        return;
    }

    if (s_ota_mode == OTA_CENTER_MODE_CONFIRM_ROLLBACK)
    {
        if (key_value == UI_KEY_KEY2_LONG)
        {
            ui_manager_navigate_home();
        }
        else if (key_value == KEY1_PRES)
        {
            ota_center_return_to_menu();
        }
        else if (key_value == KEY2_PRES)
        {
            ota_center_show_restart_info("Restore Previous Version");
            delay_ms(200U);
            ota_service_request_rollback();
        }
        return;
    }

    if (s_ota_mode == OTA_CENTER_MODE_INFO)
    {
        if (key_value == UI_KEY_KEY2_LONG)
        {
            ota_center_exit_info_mode(1U);
        }
        else if (key_value == KEY1_PRES || key_value == KEY2_PRES)
        {
            ota_center_exit_info_mode(0U);
        }
        return;
    }

    if (key_value == KEY1_PRES)
    {
        page_move_selection(&s_ota_selected, OTA_ITEM_COUNT, 1U, ota_center_draw_menu_item);
    }
    else if (key_value == KEY3_PRES)
    {
        page_move_selection(&s_ota_selected, OTA_ITEM_COUNT, 0U, ota_center_draw_menu_item);
    }
    else if (key_value == UI_KEY_KEY2_LONG)
    {
        ui_manager_navigate_home();
    }
    else if (key_value == KEY2_PRES)
    {
        if ((s_ota_selected == 0U || s_ota_selected == 1U) && settings.wifi_enabled == 0U)
        {
            s_ota_pending_action = (s_ota_selected == 0U) ? OTA_PENDING_CHECK : OTA_PENDING_UPGRADE;
            ota_center_enter_confirm_mode(OTA_CENTER_MODE_CONFIRM_WIFI);
            return;
        }

        switch (s_ota_selected)
        {
        case 0U:
            if (ota_center_request_latest_async(1U, OTA_PENDING_CHECK) == 0U)
            {
                ota_center_show_task_busy_info();
            }
            break;
        case 1U:
            (void)ota_center_start_upgrade_flow();
            break;
        case 2U:
            ota_center_enter_confirm_mode(OTA_CENTER_MODE_CONFIRM_ROLLBACK);
            break;
        case 3U:
            ota_center_show_local_version_info();
            break;
        }
    }
}

/* OTA 椤甸潰鍛ㄦ湡鍥炶皟锛屼粎璐熻矗澶勭悊寮傛瓒呮椂銆?*/
static void ota_center_on_tick(void)
{
    page_async_handle_timeouts();
}

/* 鏍规嵁褰撳墠 OTA 瀛愮姸鎬佹覆鏌撻〉闈㈠唴瀹广€?*/
static void ota_center_render(uint8_t full_refresh)
{
    if (full_refresh == 0U)
    {
        return;
    }

    if (s_ota_mode == OTA_CENTER_MODE_MENU)
    {
        ui_renderer_draw_product_page("Update", "Check Version / Firmware OTA", PAGE_UI_ACCENT_COLOR);
        ota_center_draw_info_rows();
        ota_center_draw_menu_items();
        return;
    }

    if (s_ota_mode == OTA_CENTER_MODE_CONFIRM_WIFI)
    {
        ui_renderer_draw_product_page("Update", ota_center_child_title(), PAGE_UI_ACCENT_COLOR);
        ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y,
                                   "Need WiFi",
                                   (s_async_state.ota_wifi_enable_pending != 0U) ? "Enabling..." : "Turn on now?",
                                   BLACK,
                                   WHITE);
        ui_renderer_draw_value_row(PAGE_INFO_ROW2_Y,
                                   "Reason",
                                   (s_ota_pending_action == OTA_PENDING_UPGRADE) ? "Required to update" : "Required to check",
                                   BLACK,
                                   WHITE);
        return;
    }

    if (s_ota_mode == OTA_CENTER_MODE_CONFIRM_UPGRADE)
    {
        ui_renderer_draw_product_page("Update", ota_center_child_title(), PAGE_UI_ACCENT_COLOR);
        ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y, "Latest Version", s_ota_latest_version, GREEN, WHITE);
        ui_renderer_draw_value_row(PAGE_INFO_ROW2_Y,
                                   "Target",
                                   ota_service_get_partition_name(ota_service_get_inactive_partition()),
                                   BLACK,
                                   WHITE);
        return;
    }

    if (s_ota_mode == OTA_CENTER_MODE_CONFIRM_ROLLBACK)
    {
        ui_renderer_draw_product_page("Update", ota_center_child_title(), PAGE_UI_ACCENT_COLOR);
        ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y,
                                   "Current Partition",
                                   ota_service_get_partition_name(ota_service_get_active_partition()),
                                   BLACK,
                                   WHITE);
        ui_renderer_draw_value_row(PAGE_INFO_ROW2_Y,
                                   "Old Partition",
                                   ota_service_get_partition_name(ota_service_get_inactive_partition()),
                                   YELLOW,
                                   WHITE);
        ui_renderer_draw_value_row(PAGE_INFO_ROW3_Y,
                                   "Current Version",
                                   ota_service_get_display_version(),
                                   BLACK,
                                   WHITE);
        ui_renderer_draw_value_row(PAGE_INFO_ROW4_Y,
                                   "Previous Version",
                                   ota_service_get_partition_version(ota_service_get_inactive_partition()),
                                   BLACK,
                                   WHITE);
        return;
    }

    ui_renderer_draw_product_page("Update", ota_center_child_title(), PAGE_UI_ACCENT_COLOR);
    if (s_ota_show_partition_rows != 0U)
    {
        ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y, "Current Version", s_ota_info_current_version, BLACK, WHITE);
        ui_renderer_draw_value_row(PAGE_INFO_ROW2_Y, "Current Partition", s_ota_info_partition, BLACK, WHITE);
    }
    else if (s_ota_show_version_rows != 0U)
    {
        ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y, "Version Info", s_ota_notice_line1, BLACK, WHITE);
        ui_renderer_draw_value_row(PAGE_INFO_ROW2_Y, "Current Version", s_ota_info_current_version, BLACK, WHITE);
        ui_renderer_draw_value_row(PAGE_INFO_ROW3_Y, "Latest Version", s_ota_info_latest_version, GREEN, WHITE);
    }
    else
    {
        ui_renderer_draw_value_row(PAGE_INFO_ROW1_Y, "Info", s_ota_notice_line1, BLACK, WHITE);
        ui_renderer_draw_value_row(PAGE_INFO_ROW2_Y, "Detail", s_ota_notice_line2, BLACK, WHITE);
    }
}

/* WiFi 椤甸潰杩涘叆鍥炶皟锛屽垵濮嬪寲閫夋嫨鐘舵€佸苟鎸夐渶鎷夊彇涓绘満鐘舵€併€?*/
static void connectivity_on_enter(ui_page_id_t previous_page)
{
    device_settings_t settings;

    (void)previous_page;
    s_wifi_selected = 0U;
    s_wifi_next_refresh_ms = 0U;
    memset(s_connectivity_status_cache_valid, 0, sizeof(s_connectivity_status_cache_valid));
    memset(s_connectivity_item_cache_valid, 0, sizeof(s_connectivity_item_cache_valid));

    app_rtos_settings_copy(&settings);
    if (settings.wifi_enabled != 0U ||
        settings.ble_enabled != 0U ||
        settings.mqtt_enabled != 0U)
    {
        (void)page_refresh_host_status_async();
    }
}

/* WiFi 椤甸潰绂诲紑鍥炶皟锛屾竻鐞嗘湰椤电紦瀛樸€?*/
static void connectivity_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
    memset(s_connectivity_status_cache_valid, 0, sizeof(s_connectivity_status_cache_valid));
    memset(s_connectivity_item_cache_valid, 0, sizeof(s_connectivity_item_cache_valid));
}

/* 澶勭悊 WiFi 椤甸潰鎸夐敭锛岃礋璐ｅ紑鍏?WiFi 鎴栧敜閱掍富鏈虹姸鎬佸悓姝ャ€?*/
static void connectivity_on_key(uint8_t key_value)
{
    device_settings_t settings;

    app_rtos_settings_copy(&settings);

    if (key_value == UI_KEY_KEY2_LONG)
    {
        ui_manager_navigate_home();
    }
    else if (key_value == KEY1_PRES)
    {
        page_move_selection(&s_wifi_selected, WIFI_ITEM_COUNT, 1U, connectivity_draw_item);
    }
    else if (key_value == KEY3_PRES)
    {
        page_move_selection(&s_wifi_selected, WIFI_ITEM_COUNT, 0U, connectivity_draw_item);
    }
    else if (key_value == KEY2_PRES)
    {
        if (esp_host_is_forced_deep_sleep() != 0U)
        {
            (void)page_refresh_host_status_async();
            (void)page_set_host_state_async(power_manager_get_state());
            return;
        }

        if (s_wifi_selected == 0U)
        {
            if (page_set_wifi_enabled((uint8_t)!settings.wifi_enabled) != 0U)
            {
                wifi_draw_status_row(0U);
                wifi_draw_item(0U);
            }
            else
            {
                wifi_draw_status_row(0U);
            }
        }
        else if (s_wifi_selected == 1U)
        {
            if (page_set_ble_enabled((uint8_t)!settings.ble_enabled) != 0U)
            {
                wifi_draw_item(0U);
            }
        }
        else
        {
            if (page_set_mqtt_enabled((uint8_t)!settings.mqtt_enabled) != 0U)
            {
                wifi_draw_item(0U);
            }
        }
    }
}

/* WiFi 椤甸潰鍛ㄦ湡鍥炶皟锛屾寜鑺傛祦鍛ㄦ湡鍒锋柊杩炴帴鐘舵€併€?*/
static void connectivity_on_tick(void)
{
    device_settings_t settings;

    page_async_handle_timeouts();
    app_rtos_settings_copy(&settings);

    if (settings.wifi_enabled != 0U ||
        settings.ble_enabled != 0U ||
        settings.mqtt_enabled != 0U)
    {
        uint32_t now_ms = power_manager_get_tick_ms();

        if (now_ms >= s_wifi_next_refresh_ms)
        {
            s_wifi_next_refresh_ms = now_ms + WIFI_STATUS_REFRESH_MS;
            (void)page_refresh_host_status_async();
        }
    }
    else if (esp_host_is_forced_deep_sleep() != 0U)
    {
        uint32_t now_ms = power_manager_get_tick_ms();

        if (now_ms >= s_wifi_next_refresh_ms)
        {
            s_wifi_next_refresh_ms = now_ms + WIFI_STATUS_REFRESH_MS;
            (void)page_refresh_host_status_async();
            (void)page_set_host_state_async(power_manager_get_state());
        }
    }
}

/* 娓叉煋 WiFi 椤甸潰闈欐€佸竷灞€涓庡綋鍓嶈繛鎺ョ姸鎬併€?*/
static void connectivity_render(uint8_t full_refresh)
{
    if (full_refresh == 0U)
    {
        return;
    }

    ui_renderer_draw_product_page("Wireless", "Wi-Fi / Bluetooth / Server", PAGE_UI_ACCENT_COLOR);
    wifi_draw_status_row(1U);
    wifi_draw_item(1U);
}

/* 鐢垫簮椤甸潰杩涘叆鍥炶皟锛屽浣嶈彍鍗曠劍鐐广€?*/
static void power_page_on_enter(ui_page_id_t previous_page)
{
    (void)previous_page;
    (void)&page_set_power_policy_async;
    s_power_selected = 0U;
}

/* 鐢垫簮椤甸潰绂诲紑鍥炶皟锛岀洰鍓嶄粎淇濈暀缁熶竴鎺ュ彛銆?*/
static void power_page_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
}

/* 澶勭悊鐢垫簮椤甸潰鎸夐敭锛岃礋璐ｈ疆杞厤缃€佽Е鍙戞繁鐫℃垨鎵嬪姩寰呮満銆?*/
static void power_page_on_key(uint8_t key_value)
{
    device_settings_t updated;

    app_rtos_settings_copy(&updated);

    if (key_value == KEY1_PRES)
    {
        page_move_selection(&s_power_selected, POWER_ITEM_COUNT, 1U, power_draw_item);
    }
    else if (key_value == KEY3_PRES)
    {
        page_move_selection(&s_power_selected, POWER_ITEM_COUNT, 0U, power_draw_item);
    }
    else if (key_value == UI_KEY_KEY2_LONG)
    {
        ui_manager_navigate_home();
    }
    else if (key_value == KEY2_PRES)
    {
        if (s_power_selected == 0U)
        {
            updated.screen_off_timeout_ms = page_next_screen_off_timeout_ms(updated.screen_off_timeout_ms);
        }
        else if (s_power_selected == 1U)
        {
            updated.standby_enabled = (uint8_t)!updated.standby_enabled;
        }
        else if (s_power_selected == 2U)
        {
            if (esp_host_is_forced_deep_sleep() == 0U)
            {
                if (page_enter_forced_deep_sleep_async(POWER_PAGE_HOST_PREP_TIMEOUT_MS) != 0U)
                {
                    power_draw_item(s_power_selected);
                }
            }
            else
            {
                if (page_refresh_host_status_async() != 0U)
                {
                    (void)page_set_host_state_async(power_manager_get_state());
                    power_draw_item(s_power_selected);
                }
            }
            return;
        }

        if (page_store_settings(&updated) != 0U)
        {
            power_draw_item(s_power_selected);
        }
    }
}

/* 鐢垫簮椤甸潰鍛ㄦ湡鍥炶皟锛屼粎璐熻矗澶勭悊寮傛瓒呮椂銆?*/
static void power_page_on_tick(void)
{
    page_async_handle_timeouts();
}

/* 娓叉煋鐢垫簮椤甸潰淇℃伅琛屽拰鍏ㄩ儴鑿滃崟椤广€?*/
static void power_page_render(uint8_t full_refresh)
{
    if (full_refresh == 0U)
    {
        return;
    }

    ui_renderer_draw_product_page("Power", "Screen / Standby / Save", PAGE_UI_ACCENT_COLOR);
    power_draw_info_rows();
    power_draw_items();
}

/* 绯荤粺椤甸潰杩涘叆鍥炶皟锛屾牎姝ｅ綋鍓嶉€夋嫨椤硅寖鍥淬€?*/
static void system_on_enter(ui_page_id_t previous_page)
{
    (void)previous_page;

    if (s_system_selected >= system_item_count())
    {
        s_system_selected = 0U;
    }
}

/* 绯荤粺椤甸潰绂诲紑鍥炶皟锛岀洰鍓嶄粎淇濈暀缁熶竴鎺ュ彛銆?*/
static void system_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
}

/* 澶勭悊绯荤粺椤甸潰鎸夐敭锛岃礋璐ｈ皟璇曟ā寮忓紑鍏冲拰宸ョ▼椤甸潰璺宠浆銆?*/
static void system_on_key(uint8_t key_value)
{
    uint8_t item_count = system_item_count();
    device_settings_t updated;

    app_rtos_settings_copy(&updated);

    if (key_value == KEY1_PRES)
    {
        page_move_selection(&s_system_selected, item_count, 1U, system_draw_item);
    }
    else if (key_value == KEY3_PRES)
    {
        page_move_selection(&s_system_selected, item_count, 0U, system_draw_item);
    }
    else if (key_value == UI_KEY_KEY2_LONG)
    {
        ui_manager_navigate_home();
    }
    else if (key_value == KEY2_PRES)
    {
        if (s_system_selected == SYSTEM_ITEM_THERMAL_PAUSE_SEND)
        {
#if (REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ENABLE != 0U)
            updated.thermal_pause_send_esp_enabled = (uint8_t)!updated.thermal_pause_send_esp_enabled;
            if (page_store_settings(&updated) != 0U)
            {
                system_draw_item(SYSTEM_ITEM_THERMAL_PAUSE_SEND);
            }
#endif
        }
        else if (s_system_selected == SYSTEM_ITEM_KEY2_SNAPSHOT)
        {
            updated.key2_snapshot_enabled = (uint8_t)!updated.key2_snapshot_enabled;
            if (page_store_settings(&updated) != 0U)
            {
                system_draw_item(SYSTEM_ITEM_KEY2_SNAPSHOT);
            }
        }
        else if (s_system_selected == SYSTEM_ITEM_SD_CARD)
        {
            ui_manager_navigate_to(UI_PAGE_STORAGE);
        }
        else if (s_system_selected == SYSTEM_ITEM_DEBUG_MODE)
        {
            if (updated.debug_mode_enabled != 0U)
            {
                if (updated.esp32_remote_keys_enabled != 0U)
                {
                    (void)page_set_remote_keys_enabled(0U);
                }
                if (updated.esp32_debug_screen_enabled != 0U)
                {
                    (void)page_set_debug_screen_enabled(0U);
                }
                app_rtos_settings_copy(&updated);
                updated.esp32_remote_keys_enabled = 0U;
                updated.esp32_debug_screen_enabled = 0U;
            }

            updated.debug_mode_enabled = (uint8_t)!updated.debug_mode_enabled;
            if (page_store_settings(&updated) != 0U)
            {
                if (s_system_selected >= system_item_count())
                {
                    s_system_selected = system_item_count() - 1U;
                }
                system_draw_items();
            }
        }
        else if (updated.debug_mode_enabled != 0U)
        {
            ui_manager_navigate_to(UI_PAGE_PERF_BASELINE);
        }
    }
}

/* 绯荤粺椤甸潰鍛ㄦ湡鍥炶皟锛屼粎璐熻矗澶勭悊寮傛瓒呮椂銆?*/
static void system_on_tick(void)
{
    page_async_handle_timeouts();
}

/* 娓叉煋绯荤粺椤甸潰淇℃伅琛屽拰鑿滃崟椤广€?*/
static void system_render(uint8_t full_refresh)
{
    if (full_refresh == 0U)
    {
        return;
    }

    ui_renderer_draw_product_page("System", "Storage / Debug / Params", PAGE_UI_ACCENT_COLOR);
    system_draw_items();
}

/* 宸ョ▼椤甸潰杩涘叆鍥炶皟锛屾牎楠岃皟璇曟ā寮忓苟鍒濆鍖栫劍鐐广€?*/
static void storage_page_on_enter(ui_page_id_t previous_page)
{
    (void)previous_page;

    if (s_storage_selected >= STORAGE_ITEM_COUNT)
    {
        s_storage_selected = 0U;
    }

    storage_page_set_notice("Select Mount/Info first", "Long KEY2 Home");
}

static void storage_page_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
}

static void storage_page_on_key(uint8_t key_value)
{
    storage_status_t status = STORAGE_STATUS_OK;
    storage_info_t info;
    char detail[24];

    if (key_value == KEY1_PRES)
    {
        page_move_selection(&s_storage_selected, STORAGE_ITEM_COUNT, 1U, storage_draw_item);
    }
    else if (key_value == KEY3_PRES)
    {
        page_move_selection(&s_storage_selected, STORAGE_ITEM_COUNT, 0U, storage_draw_item);
    }
    else if (key_value == UI_KEY_KEY2_LONG)
    {
        ui_manager_navigate_home();
    }
    else if (key_value == KEY2_PRES)
    {
        if (s_storage_selected == 0U)
        {
            memset(&info, 0, sizeof(info));
            status = (storage_service_mount() != 0U) ? STORAGE_STATUS_OK : storage_service_get_info(0);
            if (status == STORAGE_STATUS_OK)
            {
                status = storage_service_query_capacity(&info);
            }

            if (status == STORAGE_STATUS_OK)
            {
                snprintf(detail, sizeof(detail), "%luMB / %luMB",
                         (unsigned long)(info.free_kb / 1024UL),
                         (unsigned long)(info.total_kb / 1024UL));
                storage_page_set_notice("Mount OK", detail);
            }
            else
            {
                storage_page_set_notice("Mount failed", storage_service_status_text(status));
            }
        }
        else if (s_storage_selected == 1U)
        {
            status = storage_service_write_test_file();
            storage_page_set_notice((status == STORAGE_STATUS_OK) ? "Write OK" : "Write failed",
                                    storage_service_status_text(status));
        }
        else if (s_storage_selected == 2U)
        {
            status = storage_service_read_test_file();
            storage_page_set_notice((status == STORAGE_STATUS_OK) ? "Read OK" : "Read failed",
                                    storage_service_status_text(status));
        }
        else if (s_storage_selected == 3U)
        {
            ui_manager_navigate_to(UI_PAGE_SNAPSHOT_REVIEW);
            return;
        }

        ui_manager_force_full_refresh();
    }
}

static void storage_page_on_tick(void)
{
    page_async_handle_timeouts();
}

static void storage_page_render(uint8_t full_refresh)
{
    if (full_refresh == 0U)
    {
        return;
    }

    ui_renderer_draw_product_page("SD Card", "Mount / Test / Snapshot", PAGE_UI_ACCENT_COLOR);
    storage_draw_info_rows();
    storage_draw_items();
    ui_renderer_draw_footer(s_storage_notice_line1, s_storage_notice_line2);
}

static void snapshot_review_on_enter(ui_page_id_t previous_page)
{
    (void)previous_page;

    memset(&s_snapshot_review_snapshot, 0, sizeof(s_snapshot_review_snapshot));
    memset(s_snapshot_review_gray_frame, 0, sizeof(s_snapshot_review_gray_frame));
    s_snapshot_review_index = 0U;
    s_snapshot_review_loaded = 0U;
    s_snapshot_review_status = snapshot_storage_load_latest(&s_snapshot_review_snapshot,
                                                            &s_snapshot_review_index);

    if (s_snapshot_review_status == STORAGE_STATUS_OK)
    {
        s_snapshot_review_status = snapshot_storage_build_gray_preview(&s_snapshot_review_snapshot,
                                                                      s_snapshot_review_gray_frame);
        s_snapshot_review_loaded = (s_snapshot_review_status == STORAGE_STATUS_OK) ? 1U : 0U;
    }
}

static void snapshot_review_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
}

static void snapshot_review_on_key(uint8_t key_value)
{
    if (key_value == UI_KEY_KEY2_LONG)
    {
        ui_manager_navigate_home();
    }
    else if (key_value == KEY1_PRES || key_value == KEY2_PRES || key_value == KEY3_PRES)
    {
        ui_manager_navigate_back();
    }
}

static void snapshot_review_on_tick(void)
{
    page_async_handle_timeouts();
}

static void snapshot_review_render(uint8_t full_refresh)
{
    char info_line[48];

    if (full_refresh == 0U)
    {
        return;
    }

    memset(info_line, 0, sizeof(info_line));

    if (s_snapshot_review_loaded != 0U)
    {
        power_manager_acquire_lock(POWER_LOCK_DISPLAY_DMA);
        (void)LCD_Disp_Thermal_Interpolated_DMA(s_snapshot_review_gray_frame);
        LCD_Fill(0U, (uint16_t)(LCD_H - 20U), (uint16_t)(LCD_W - 1U), (uint16_t)(LCD_H - 1U), PAGE_UI_PANEL_COLOR);
        LCD_DrawLine(0U,
                     (uint16_t)(LCD_H - 20U),
                     (uint16_t)(LCD_W - 1U),
                     (uint16_t)(LCD_H - 20U),
                     PAGE_UI_PANEL_EDGE_COLOR);
        snprintf(info_line,
                 sizeof(info_line),
                 "#%06lu L%d.%d H%d.%d C%d.%d",
                 (unsigned long)s_snapshot_review_index,
                 s_snapshot_review_snapshot.min_x10 / 10,
                 (s_snapshot_review_snapshot.min_x10 < 0) ? -(s_snapshot_review_snapshot.min_x10 % 10) : (s_snapshot_review_snapshot.min_x10 % 10),
                 s_snapshot_review_snapshot.max_x10 / 10,
                 (s_snapshot_review_snapshot.max_x10 < 0) ? -(s_snapshot_review_snapshot.max_x10 % 10) : (s_snapshot_review_snapshot.max_x10 % 10),
                 s_snapshot_review_snapshot.center_x10 / 10,
                 (s_snapshot_review_snapshot.center_x10 < 0) ? -(s_snapshot_review_snapshot.center_x10 % 10) : (s_snapshot_review_snapshot.center_x10 % 10));
        LCD_ShowString(4U,
                       (uint16_t)(LCD_H - 16U),
                       (const u8 *)info_line,
                       WHITE,
                       PAGE_UI_PANEL_COLOR,
                       12,
                       0);
        power_manager_release_lock(POWER_LOCK_DISPLAY_DMA);
        return;
    }

    ui_renderer_draw_product_page("SD Card", "View Latest", PAGE_UI_ACCENT_COLOR);
    snapshot_review_draw_info();
    ui_renderer_draw_footer("No snapshot", storage_service_status_text(s_snapshot_review_status));
}

static void engineering_on_enter(ui_page_id_t previous_page)
{
    device_settings_t settings;

    (void)previous_page;
    app_rtos_settings_copy(&settings);

    if (settings.debug_mode_enabled == 0U)
    {
        ui_manager_navigate_home();
        return;
    }

    s_engineering_selected = 0U;
    page_refresh_host_status_async();
}

/* 宸ョ▼椤甸潰绂诲紑鍥炶皟锛岀洰鍓嶄粎淇濈暀缁熶竴鎺ュ彛銆?*/
static void engineering_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
}

/* 澶勭悊宸ョ▼椤甸潰鎸夐敭锛岃礋璐ｅ瓙椤佃烦杞拰宸ョ▼寮€鍏抽」鍒囨崲銆?*/
static void engineering_on_key(uint8_t key_value)
{
    device_settings_t settings;

    app_rtos_settings_copy(&settings);

    if (key_value == KEY1_PRES)
    {
        page_move_selection(&s_engineering_selected, ENGINEERING_ITEM_COUNT, 1U, engineering_draw_item);
    }
    else if (key_value == KEY3_PRES)
    {
        page_move_selection(&s_engineering_selected, ENGINEERING_ITEM_COUNT, 0U, engineering_draw_item);
    }
    else if (key_value == UI_KEY_KEY2_LONG)
    {
        ui_manager_navigate_home();
    }
    else if (key_value == KEY2_PRES)
    {
        uint8_t ok = 0U;

        if (s_engineering_selected == 0U)
        {
            ui_manager_navigate_to(UI_PAGE_PERF_BASELINE);
            return;
        }
        else if (s_engineering_selected == 1U)
        {
            ok = page_set_debug_screen_enabled((uint8_t)!settings.esp32_debug_screen_enabled);
        }
        else
        {
            ok = page_set_remote_keys_enabled((uint8_t)!settings.esp32_remote_keys_enabled);
        }

        if (ok != 0U)
        {
            engineering_draw_item(s_engineering_selected);
        }
    }
}

/* 宸ョ▼椤甸潰鍛ㄦ湡鍥炶皟锛岃嫢璋冭瘯妯″紡鍏抽棴鍒欑洿鎺ヨ繑鍥為椤点€?*/
static void engineering_on_tick(void)
{
    if (perf_baseline_debug_visible() == 0U)
    {
        ui_manager_navigate_home();
        return;
    }

    page_async_handle_timeouts();
}

/* 娓叉煋宸ョ▼椤甸潰鑿滃崟鍐呭銆?*/
static void engineering_render(uint8_t full_refresh)
{
    if (full_refresh == 0U)
    {
        return;
    }

    ui_renderer_draw_product_page("Debug Page", "Debug / Advanced Tools", PAGE_UI_ACCENT_COLOR);
    engineering_draw_items();
}

/* 鍒ゆ柇鎬ц兘鍩虹嚎椤甸潰鏄惁鍏佽鏄剧ず銆?*/
static uint8_t perf_baseline_debug_visible(void)
{
    device_settings_t settings;

    app_rtos_settings_copy(&settings);
    return (settings.debug_mode_enabled != 0U) ? 1U : 0U;
}

/* 鏍规嵁褰撳墠瀛愰〉绱㈠紩杩斿洖鎬ц兘鍩虹嚎椤甸潰鏍囬銆?*/
/* 灏嗘枃鏈ˉ绌烘牸瀵归綈鍒板浐瀹氶暱搴︼紝渚夸簬灞€閮ㄥ埛鏂拌鐩栨棫鍐呭銆?*/
static void perf_baseline_pad_text(char *buffer,
                                   uint16_t buffer_len,
                                   const char *value,
                                   uint8_t pad_chars)
{
    uint16_t i = 0U;
    uint16_t copy_len = 0U;

    if (buffer == 0 || buffer_len == 0U)
    {
        return;
    }

    memset(buffer, 0, buffer_len);
    if (pad_chars >= buffer_len)
    {
        pad_chars = (uint8_t)(buffer_len - 1U);
    }

    if (value != 0)
    {
        copy_len = (uint16_t)strlen(value);
        if (copy_len > pad_chars)
        {
            copy_len = pad_chars;
        }

        if (copy_len > 0U)
        {
            memcpy(buffer, value, copy_len);
        }
    }

    for (i = copy_len; i < pad_chars; ++i)
    {
        buffer[i] = ' ';
    }
    buffer[pad_chars] = '\0';
}

/* 缁樺埗鎬ц兘鍩虹嚎椤甸潰涓殑鍗曡甯冨眬搴曟澘鍜屾爣绛俱€?*/
static void perf_baseline_draw_layout_row(uint16_t y, const char *label)
{
    app_display_runtime_lock();
    LCD_Fill(8U, y, LCD_W - 8U, (uint16_t)(y + UI_ROW_HEIGHT - 2U), WHITE);
    if (label != 0 && label[0] != '\0')
    {
        LCD_ShowUTF8String(PERF_LABEL_X, y, ui_renderer_localize(label), BLACK, WHITE, 16, 0);
    }
    app_display_runtime_unlock();
}

/* 鍦ㄦ€ц兘鍩虹嚎椤甸潰鎸囧畾浣嶇疆缁樺埗涓€涓榻愬悗鐨勫€兼枃鏈€?*/
static void perf_baseline_draw_value_text_ex(uint16_t y,
                                             const char *value,
                                             uint16_t value_color,
                                             uint16_t value_x,
                                             uint8_t pad_chars,
                                             uint8_t font_size)
{
    char padded[32];
    const char *display_value = ui_renderer_localize(value);

    perf_baseline_pad_text(padded, sizeof(padded), display_value, pad_chars);

    app_display_runtime_lock();
    LCD_ShowUTF8String(value_x, y, padded, value_color, WHITE, font_size, 0);
    app_display_runtime_unlock();
}

/* 浠ラ粯璁ゅ竷灞€鍙傛暟缁樺埗鎬ц兘鍩虹嚎鍊兼枃鏈€?*/
static void perf_baseline_draw_value_text(uint16_t y, const char *value, uint16_t value_color)
{
    perf_baseline_draw_value_text_ex(y,
                                     value,
                                     value_color,
                                     PERF_VALUE_X,
                                     PERF_VALUE_PAD_CHARS,
                                     PERF_VALUE_FONT_SIZE);
}

/* 缁樺埗鎬ц兘鍩虹嚎鏃跺簭缁熻鍊兼枃鏈€?*/
static void perf_baseline_draw_timing_value_text(uint16_t y, const char *value, uint16_t value_color)
{
    perf_baseline_draw_value_text_ex(y,
                                     value,
                                     value_color,
                                     PERF_TIMING_VALUE_X,
                                     PERF_TIMING_VALUE_PAD_CHARS,
                                     PERF_TIMING_VALUE_FONT_SIZE);
}

static uint8_t perf_baseline_text_has_non_ascii(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (cursor == 0)
    {
        return 0U;
    }

    while (*cursor != '\0')
    {
        if (*cursor >= 0x80U)
        {
            return 1U;
        }
        ++cursor;
    }

    return 0U;
}

/* 缁樺埗鎬ц兘鍩虹嚎椤甸潰搴曢儴涓よ璇存槑鏂囨湰銆?*/
static void perf_baseline_draw_footer_text(const char *line1, const char *line2)
{
    char padded1[48];
    char padded2[48];
    const char *display_line1 = ui_renderer_localize(line1);
    const char *display_line2 = ui_renderer_localize(line2);
    uint8_t use_utf8 = 0U;

    if (perf_baseline_text_has_non_ascii(display_line1) != 0U ||
        perf_baseline_text_has_non_ascii(display_line2) != 0U)
    {
        use_utf8 = 1U;
    }

    app_display_runtime_lock();
    if (use_utf8 != 0U)
    {
        LCD_Fill(8U, UI_FOOTER_LINE1_Y, LCD_W - 8U, LCD_H - 1U, WHITE);
        if (display_line1 != 0 && display_line1[0] != '\0')
        {
            LCD_ShowUTF8String(8U, UI_FOOTER_LINE1_Y, display_line1, DARKBLUE, WHITE, 16, 0);
        }
        if (display_line2 != 0 && display_line2[0] != '\0')
        {
            LCD_ShowUTF8String(8U, UI_FOOTER_LINE2_Y, display_line2, DARKBLUE, WHITE, 16, 0);
        }
    }
    else
    {
        perf_baseline_pad_text(padded1, sizeof(padded1), display_line1, PERF_FOOTER_PAD_CHARS);
        perf_baseline_pad_text(padded2, sizeof(padded2), display_line2, PERF_FOOTER_PAD_CHARS);
        LCD_ShowString(8U, UI_FOOTER_LINE1_Y, (const u8 *)padded1, DARKBLUE, WHITE, UI_FOOTER_FONT_SIZE, 0);
        LCD_ShowString(8U, UI_FOOTER_LINE2_Y, (const u8 *)padded2, DARKBLUE, WHITE, UI_FOOTER_FONT_SIZE, 0);
    }
    app_display_runtime_unlock();
}

/* 鎸夊綋鍓嶅瓙椤垫ā寮忕粯鍒舵€ц兘鍩虹嚎椤甸潰鐨勫浐瀹氬竷灞€銆?*/
static void perf_baseline_draw_layout(uint8_t enabled)
{
    static const char *s_snapshot_labels[4] = { "FPS", "MinT", "MaxT", "CtrT" };
    static const char *s_timing_labels[4] = { "Frame L/A/M", "Temp  L/A/M", "Gray  L/A/M", "DMA   L/A/M" };
    static const char *s_lcd_dma_detail_labels[4] = { "RndrF L/A/M", "DStrt L/A/M", "DWait L/A/M", "SPIId L/A/M" };
    static const char *s_thermal_flow_labels[4] = { "ReadyRpl", "DispCncl", "3D Claim", "DoneCncl" };
    static const char *s_counter_labels[4] = { "KeyQ Drop", "UIQ Drop", "SvcQ Fail", "UART Err" };
    static const char *s_health_labels[4] = { "Wdg Fault", "Miss Prog", "Therm Act", "Screen" };
    static const char *s_i2c_dma_labels[4] = { "I2C AF", "I2C BERR", "I2C ARLO", "I2C OVR" };
    static const char *s_i2c_dma_to_diag_labels[4] = { "TO State", "TO NDTR", "TO SR1", "TO SR2" };
    static const char *s_i2c_dma_tc_diag_labels[4] = { "TC State", "TC NDTR", "TC SR1", "TC SR2" };
    static const char *s_i2c_timeout_src_labels[4] = { "R/W/V TMO", "ADDRW", "8000", "800D" };
    static const char *s_i2c_poll_timeout_labels[4] = { "R8000AW", "W8000AW", "R800DAW", "R800DRX" };
    static const char *s_pair_diag_labels[4] = { "PairOK", "PairWait", "PairTO", "Streak" };
    static const char *s_pair_bucket_labels[4] = { "TO80-120", "TO120-160", "TO160-240", "TO240+" };
    static const char *s_bus_clear_labels[4] = { "BusClrRead", "BusClrWrite", "BusClrDma", "BusClrBusy" };
    static const char *s_disabled_labels[4] = { "Status", "Switch", "Action", "Scope" };
    const char **labels = s_snapshot_labels;
    uint8_t index = 0U;

    if (enabled == 0U)
    {
        labels = s_disabled_labels;
    }
    else
    {
        switch (s_perf_baseline_subpage)
        {
        case 1U:
            labels = s_timing_labels;
            break;
        case 2U:
            labels = s_counter_labels;
            break;
        case 3U:
            labels = s_health_labels;
            break;
        case 4U:
            labels = s_lcd_dma_detail_labels;
            break;
        case 5U:
            labels = s_thermal_flow_labels;
            break;
        case 6U:
            labels = s_i2c_dma_labels;
            break;
        case 7U:
            labels = s_i2c_dma_to_diag_labels;
            break;
        case 8U:
            labels = s_i2c_dma_tc_diag_labels;
            break;
        case 9U:
            labels = s_i2c_timeout_src_labels;
            break;
        case 10U:
            labels = s_i2c_poll_timeout_labels;
            break;
        case 11U:
            labels = s_pair_diag_labels;
            break;
        case 12U:
            labels = s_bus_clear_labels;
            break;
        case 13U:
            labels = s_pair_bucket_labels;
            break;
        case 0U:
        default:
            labels = s_snapshot_labels;
            break;
        }
    }

    for (index = 0U; index < 4U; ++index)
    {
        perf_baseline_draw_layout_row(page_list_item_y(UI_CONTENT_TOP, index), labels[index]);
    }

    app_display_runtime_lock();
    LCD_Fill(0U, UI_FOOTER_LINE1_Y, LCD_W - 1U, LCD_H - 1U, WHITE);
    app_display_runtime_unlock();
}

static void perf_baseline_format_temp(char *buffer,
                                      uint16_t buffer_len,
                                      float temp,
                                      uint8_t has_value)
{
    int32_t scaled = 0;
    int32_t whole = 0;
    int32_t frac = 0;

    if (buffer == 0 || buffer_len == 0U)
    {
        return;
    }

    if (has_value == 0U)
    {
        snprintf(buffer, buffer_len, "%s", "--.-C");
        return;
    }

    scaled = (temp >= 0.0f) ?
             (int32_t)(temp * 10.0f + 0.5f) :
             (int32_t)(temp * 10.0f - 0.5f);
    whole = scaled / 10;
    frac = scaled % 10;
    if (frac < 0)
    {
        frac = -frac;
    }

    snprintf(buffer, buffer_len, "%ld.%ldC", (long)whole, (long)frac);
}

/* 灏?last/avg/max 涓夊厓缁熻鍊兼牸寮忓寲涓烘枃鏈€?*/
static void perf_baseline_format_triplet(char *buffer,
                                         uint16_t buffer_len,
                                         uint32_t last,
                                         uint32_t avg,
                                         uint32_t max,
                                         uint8_t has_value)
{
    if (buffer == 0 || buffer_len == 0U)
    {
        return;
    }

    if (has_value == 0U)
    {
        snprintf(buffer, buffer_len, "%s", "-/-/-");
        return;
    }

    snprintf(buffer,
             buffer_len,
             "%lu/%lu/%lu",
             (unsigned long)last,
             (unsigned long)avg,
             (unsigned long)max);
}

/* 灏?32 浣嶆暟鍊兼牸寮忓寲涓哄崄鍏繘鍒舵枃鏈€?*/
static void perf_baseline_format_hex32(char *buffer, uint16_t buffer_len, uint32_t value)
{
    if (buffer == 0 || buffer_len == 0U)
    {
        return;
    }

    snprintf(buffer, buffer_len, "0x%lX", (unsigned long)value);
}

/* 灏?LCD DMA 鐘舵€佹灇涓捐浆鎹负椤甸潰鏄剧ず鏂囨湰銆?*/
static const char *perf_baseline_dma_status_text(uint8_t status)
{
    switch ((app_perf_lcd_dma_status_t)status)
    {
    case APP_PERF_LCD_DMA_STATUS_OK:
        return "OK";
    case APP_PERF_LCD_DMA_STATUS_ERROR:
        return "ERR";
    case APP_PERF_LCD_DMA_STATUS_TIMEOUT:
    case APP_PERF_LCD_DMA_STATUS_NONE:
    default:
        return "WAIT";
    }
}

/* 缁樺埗鎬ц兘鍩虹嚎蹇収椤点€?*/
static void perf_baseline_draw_snapshot(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[32];
    char footer2[32];
    uint8_t has_frame = 0U;

    if (snapshot == 0)
    {
        return;
    }

    has_frame = (snapshot->thermal_capture_frames != 0U) ? 1U : 0U;

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->lcd_present_fps);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U), value, DARKBLUE);

    perf_baseline_format_temp(value, sizeof(value), snapshot->latest_min_temp, has_frame);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U), value, BLUE);

    perf_baseline_format_temp(value, sizeof(value), snapshot->latest_max_temp, has_frame);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U), value, RED);

    perf_baseline_format_temp(value, sizeof(value), snapshot->latest_center_temp, has_frame);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U), value, GREEN);

    snprintf(footer1,
             sizeof(footer1),
             "Cap/Disp %lu/%lu",
             (unsigned long)snapshot->thermal_capture_frames,
             (unsigned long)snapshot->thermal_display_frames);
    snprintf(footer2,
             sizeof(footer2),
             "Fail:%lu DMA:%s",
             (unsigned long)snapshot->thermal_capture_failures,
             perf_baseline_dma_status_text(snapshot->last_dma_status));
    perf_baseline_draw_footer_text(footer1, footer2);
}

/* 缁樺埗鎬ц兘鍩虹嚎鏃跺簭缁熻椤点€?*/
static void perf_baseline_draw_timing(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[32];
    char footer2[40];
    const char *refresh_state = "16";
    const char *pair_state = "ON";

    if (snapshot == 0)
    {
        return;
    }

    perf_baseline_format_triplet(value,
                                 sizeof(value),
                                 snapshot->thermal_frame_period_last_ms,
                                 snapshot->thermal_frame_period_avg_ms,
                                 snapshot->thermal_frame_period_max_ms,
                                 (snapshot->thermal_frame_period_samples != 0U) ? 1U : 0U);
    perf_baseline_draw_timing_value_text(page_list_item_y(UI_CONTENT_TOP, 0U), value, DARKBLUE);

    perf_baseline_format_triplet(value,
                                 sizeof(value),
                                 snapshot->get_temp_last_us,
                                 snapshot->get_temp_avg_us,
                                 snapshot->get_temp_max_us,
                                 (snapshot->get_temp_samples != 0U) ? 1U : 0U);
    perf_baseline_draw_timing_value_text(page_list_item_y(UI_CONTENT_TOP, 1U), value, DARKBLUE);

    perf_baseline_format_triplet(value,
                                 sizeof(value),
                                 snapshot->gray_last_us,
                                 snapshot->gray_avg_us,
                                 snapshot->gray_max_us,
                                 (snapshot->gray_samples != 0U) ? 1U : 0U);
    perf_baseline_draw_timing_value_text(page_list_item_y(UI_CONTENT_TOP, 2U), value, DARKBLUE);

    perf_baseline_format_triplet(value,
                                 sizeof(value),
                                 snapshot->lcd_dma_last_us,
                                 snapshot->lcd_dma_avg_us,
                                 snapshot->lcd_dma_max_us,
                                 (snapshot->lcd_dma_samples != 0U) ? 1U : 0U);
    perf_baseline_draw_timing_value_text(page_list_item_y(UI_CONTENT_TOP, 3U), value, DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "Power:%s",
             ui_renderer_power_state_text(snapshot->power_state));
    snprintf(footer2,
             sizeof(footer2),
             "Clock:%s R:%s P:%s",
             ui_renderer_clock_profile_text(snapshot->clock_profile),
             refresh_state,
             pair_state);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_lcd_dma_detail(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char overlay[24];
    char footer1[32];
    char footer2[40];

    if (snapshot == 0)
    {
        return;
    }

    perf_baseline_format_triplet(value,
                                 sizeof(value),
                                 snapshot->lcd_dma_render_last_us,
                                 snapshot->lcd_dma_render_avg_us,
                                 snapshot->lcd_dma_render_max_us,
                                 (snapshot->lcd_dma_render_samples != 0U) ? 1U : 0U);
    perf_baseline_draw_timing_value_text(page_list_item_y(UI_CONTENT_TOP, 0U), value, DARKBLUE);

    perf_baseline_format_triplet(value,
                                 sizeof(value),
                                 snapshot->lcd_dma_start_last_us,
                                 snapshot->lcd_dma_start_avg_us,
                                 snapshot->lcd_dma_start_max_us,
                                 (snapshot->lcd_dma_start_samples != 0U) ? 1U : 0U);
    perf_baseline_draw_timing_value_text(page_list_item_y(UI_CONTENT_TOP, 1U), value, DARKBLUE);

    perf_baseline_format_triplet(value,
                                 sizeof(value),
                                 snapshot->lcd_dma_wait_last_us,
                                 snapshot->lcd_dma_wait_avg_us,
                                 snapshot->lcd_dma_wait_max_us,
                                 (snapshot->lcd_dma_wait_samples != 0U) ? 1U : 0U);
    perf_baseline_draw_timing_value_text(page_list_item_y(UI_CONTENT_TOP, 2U), value, DARKBLUE);

    perf_baseline_format_triplet(value,
                                 sizeof(value),
                                 snapshot->lcd_dma_spi_idle_last_us,
                                 snapshot->lcd_dma_spi_idle_avg_us,
                                 snapshot->lcd_dma_spi_idle_max_us,
                                 (snapshot->lcd_dma_spi_idle_samples != 0U) ? 1U : 0U);
    perf_baseline_draw_timing_value_text(page_list_item_y(UI_CONTENT_TOP, 3U), value, DARKBLUE);

    perf_baseline_format_triplet(overlay,
                                 sizeof(overlay),
                                 snapshot->lcd_dma_overlay_last_us,
                                 snapshot->lcd_dma_overlay_avg_us,
                                 snapshot->lcd_dma_overlay_max_us,
                                 (snapshot->lcd_dma_overlay_samples != 0U) ? 1U : 0U);
    snprintf(footer1, sizeof(footer1), "Ovly %s", overlay);
    snprintf(footer2,
             sizeof(footer2),
             "Area:%lux%lu",
             (unsigned long)LCD_W,
             (unsigned long)(LCD_H - 20U));
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_thermal_flow_detail(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[40];
    char footer2[40];

    if (snapshot == 0)
    {
        return;
    }

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_ready_replace_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  value,
                                  (snapshot->thermal_ready_replace_count != 0U) ? BROWN : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_display_cancel_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->thermal_display_cancel_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_3d_claim_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_3d_done_cancel_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->thermal_3d_done_cancel_count != 0U) ? RED : DARKBLUE);

    perf_baseline_format_triplet(footer1,
                                 sizeof(footer1),
                                 snapshot->thermal_display_age_last_ms,
                                 snapshot->thermal_display_age_avg_ms,
                                 snapshot->thermal_display_age_max_ms,
                                 (snapshot->thermal_display_age_samples != 0U) ? 1U : 0U);
    snprintf(footer2,
             sizeof(footer2),
             "Age %s D %lu/%lu",
             footer1,
             (unsigned long)snapshot->thermal_3d_done_ok_count,
             (unsigned long)snapshot->thermal_3d_done_error_count);
    snprintf(footer1,
             sizeof(footer1),
             "3D:%lu/%lu/%lu W:%lu",
             (unsigned long)snapshot->thermal_3d_sync_present_attempt_count,
             (unsigned long)snapshot->thermal_3d_sync_present_ok_count,
             (unsigned long)snapshot->thermal_3d_sync_present_fail_count,
             (unsigned long)snapshot->thermal_3d_wait_timeout_count);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_i2c_dma_detail(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[40];
    char footer2[40];

    if (snapshot == 0)
    {
        return;
    }

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_af_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  value,
                                  (snapshot->i2c_af_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_berr_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->i2c_berr_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_arlo_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  (snapshot->i2c_arlo_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_ovr_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->i2c_ovr_count != 0U) ? RED : DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "TMO:%lu BUSY:%lu DMAE:%lu",
             (unsigned long)snapshot->i2c_timeout_count,
             (unsigned long)snapshot->i2c_busy_stuck_count,
             (unsigned long)snapshot->i2c_dma_err_count);
    snprintf(footer2,
             sizeof(footer2),
             "I2:%lu PairTO:%lu",
             (unsigned long)snapshot->i2c_failure_count,
             (unsigned long)snapshot->thermal_pair_timeout_count);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static const char *perf_baseline_i2c_dma_state_name(uint32_t state)
{
    switch (state)
    {
    case 0U:
        return "IDLE";
    case 1U:
        return "STW";
    case 2U:
        return "AWR";
    case 3U:
        return "MHI";
    case 4U:
        return "MLO";
    case 5U:
        return "STR";
    case 6U:
        return "ARD";
    case 7U:
        return "DRX";
    case 8U:
        return "DONE";
    case 9U:
        return "ERR";
    default:
        return "?";
    }
}

static const char *perf_baseline_pair_result_name(uint32_t result)
{
    switch ((app_perf_thermal_pair_result_t)result)
    {
    case APP_PERF_THERMAL_PAIR_RESULT_WAIT_OTHER:
        return "WAIT";
    case APP_PERF_THERMAL_PAIR_RESULT_TIMEOUT:
        return "TMO";
    case APP_PERF_THERMAL_PAIR_RESULT_GRACE_OK:
        return "GOK";
    case APP_PERF_THERMAL_PAIR_RESULT_COMPOSE_OK:
        return "OK";
    case APP_PERF_THERMAL_PAIR_RESULT_NONE:
    default:
        return "NONE";
    }
}

static void perf_baseline_draw_i2c_dma_timeout_diag(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[40];
    char footer2[40];

    if (snapshot == 0)
    {
        return;
    }

    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  perf_baseline_i2c_dma_state_name(snapshot->i2c_dma_timeout_state),
                                  (snapshot->i2c_dma_timeout_state == 0U) ? DARKBLUE : RED);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_dma_timeout_ndtr);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->i2c_dma_timeout_ndtr != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "0x%04lX", (unsigned long)(snapshot->i2c_dma_timeout_sr1 & 0xFFFFUL));
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  (snapshot->i2c_dma_timeout_sr1 != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "0x%04lX", (unsigned long)(snapshot->i2c_dma_timeout_sr2 & 0xFFFFUL));
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->i2c_dma_timeout_sr2 != 0U) ? RED : DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "EV:%lu TC:%lu",
             (unsigned long)snapshot->i2c_dma_ev_irq_count,
             (unsigned long)snapshot->i2c_dma_tc_irq_count);
    snprintf(footer2,
             sizeof(footer2),
             "TMO:%lu BUSY:%lu",
             (unsigned long)snapshot->i2c_timeout_count,
             (unsigned long)snapshot->i2c_busy_stuck_count);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_i2c_dma_tc_diag(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[40];
    char footer2[40];

    if (snapshot == 0)
    {
        return;
    }

    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  perf_baseline_i2c_dma_state_name(snapshot->i2c_dma_tc_state),
                                  (snapshot->i2c_dma_tc_state == 0U) ? DARKBLUE : RED);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_dma_tc_ndtr);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->i2c_dma_tc_ndtr != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "0x%04lX", (unsigned long)(snapshot->i2c_dma_tc_sr1 & 0xFFFFUL));
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  (snapshot->i2c_dma_tc_sr1 != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "0x%04lX", (unsigned long)(snapshot->i2c_dma_tc_sr2 & 0xFFFFUL));
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->i2c_dma_tc_sr2 != 0U) ? RED : DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "TC:%lu TMO:%lu",
             (unsigned long)snapshot->i2c_dma_tc_irq_count,
             (unsigned long)snapshot->i2c_timeout_count);
    snprintf(footer2,
             sizeof(footer2),
             "DMAE:%lu I2:%lu",
             (unsigned long)snapshot->i2c_dma_err_count,
             (unsigned long)snapshot->i2c_failure_count);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_i2c_timeout_sources(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[32];
    char footer1[40];
    char footer2[40];

    if (snapshot == 0)
    {
        return;
    }

    snprintf(value,
             sizeof(value),
             "%lu/%lu/%lu",
             (unsigned long)snapshot->i2c_poll_timeout_read_count,
             (unsigned long)snapshot->i2c_poll_timeout_write_count,
             (unsigned long)snapshot->i2c_poll_timeout_verify_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  value,
                                  ((snapshot->i2c_poll_timeout_read_count +
                                    snapshot->i2c_poll_timeout_write_count +
                                    snapshot->i2c_poll_timeout_verify_count) != 0U) ? RED : DARKBLUE);

    snprintf(value,
             sizeof(value),
             "%lu/%lu",
             (unsigned long)snapshot->i2c_addrw_timeout_read_count,
             (unsigned long)snapshot->i2c_addrw_timeout_write_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  ((snapshot->i2c_addrw_timeout_read_count +
                                    snapshot->i2c_addrw_timeout_write_count) != 0U) ? RED : DARKBLUE);

    snprintf(value,
             sizeof(value),
             "%lu/%lu",
             (unsigned long)snapshot->i2c_addr_8000_timeout_read_count,
             (unsigned long)snapshot->i2c_addr_8000_timeout_write_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  ((snapshot->i2c_addr_8000_timeout_read_count +
                                    snapshot->i2c_addr_8000_timeout_write_count) != 0U) ? RED : DARKBLUE);

    snprintf(value,
             sizeof(value),
             "%lu/%lu",
             (unsigned long)snapshot->i2c_addr_800d_timeout_read_count,
             (unsigned long)snapshot->i2c_addr_800d_timeout_write_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  ((snapshot->i2c_addr_800d_timeout_read_count +
                                    snapshot->i2c_addr_800d_timeout_write_count) != 0U) ? RED : DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "BUSY R/W/D:%lu/%lu/%lu",
             (unsigned long)snapshot->i2c_busy_timeout_read_count,
             (unsigned long)snapshot->i2c_busy_timeout_write_count,
             (unsigned long)snapshot->i2c_busy_timeout_verify_count);
    snprintf(footer2,
             sizeof(footer2),
             "BusClear:%lu",
             (unsigned long)snapshot->i2c_bus_clear_count);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_i2c_poll_timeout_detail(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[40];
    char footer2[40];

    if (snapshot == 0)
    {
        return;
    }

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_r8000_addrw_timeout_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  value,
                                  (snapshot->i2c_r8000_addrw_timeout_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_w8000_addrw_timeout_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->i2c_w8000_addrw_timeout_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_r800d_addrw_timeout_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  (snapshot->i2c_r800d_addrw_timeout_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_r800d_rx_timeout_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->i2c_r800d_rx_timeout_count != 0U) ? RED : DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "I2:%lu TMO:%lu",
             (unsigned long)snapshot->i2c_failure_count,
             (unsigned long)snapshot->i2c_timeout_count);
    snprintf(footer2,
             sizeof(footer2),
             "PairTO:%lu",
             (unsigned long)snapshot->thermal_pair_timeout_count);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_pair_diag(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[40];
    char footer2[40];
    char subpage_text[8];
    char missing_text[8];

    if (snapshot == 0)
    {
        return;
    }

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_pair_compose_ok_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  value,
                                  (snapshot->thermal_pair_compose_ok_count != 0U) ? DARKBLUE : GRAYBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_pair_wait_other_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->thermal_pair_wait_other_count != 0U) ? DARKBLUE : GRAYBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_pair_timeout_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  (snapshot->thermal_pair_timeout_count != 0U) ? RED : DARKBLUE);

    snprintf(value,
             sizeof(value),
             "%lu/%lu",
             (unsigned long)snapshot->thermal_pair_same_subpage_streak_last,
             (unsigned long)snapshot->thermal_pair_same_subpage_streak_max);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->thermal_pair_same_subpage_streak_max > 1U) ? RED : DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "OKG:%lu/%lu TOG:%lu/%lu",
             (unsigned long)snapshot->thermal_pair_compose_gap_last_ms,
             (unsigned long)snapshot->thermal_pair_compose_gap_max_ms,
             (unsigned long)snapshot->thermal_pair_timeout_gap_last_ms,
             (unsigned long)snapshot->thermal_pair_timeout_gap_max_ms);

    if (snapshot->thermal_pair_last_subpage <= 1U)
    {
        snprintf(subpage_text, sizeof(subpage_text), "%lu", (unsigned long)snapshot->thermal_pair_last_subpage);
    }
    else
    {
        snprintf(subpage_text, sizeof(subpage_text), "-");
    }

    if (snapshot->thermal_pair_last_missing_subpage <= 1U)
    {
        snprintf(missing_text, sizeof(missing_text), "%lu", (unsigned long)snapshot->thermal_pair_last_missing_subpage);
    }
    else
    {
        snprintf(missing_text, sizeof(missing_text), "-");
    }

    snprintf(footer2,
             sizeof(footer2),
             "S:%s M:%s R:%s GT:%lu ST:%lu",
             subpage_text,
             missing_text,
             perf_baseline_pair_result_name(snapshot->thermal_pair_last_result),
             (unsigned long)snapshot->thermal_pair_timeout_get_temp_last_us,
             (unsigned long)snapshot->thermal_pair_timeout_step_last_us);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_pair_bucket_diag(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[40];
    char footer2[40];

    if (snapshot == 0)
    {
        return;
    }

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_pair_timeout_gap_80_120_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  value,
                                  (snapshot->thermal_pair_timeout_gap_80_120_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_pair_timeout_gap_120_160_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->thermal_pair_timeout_gap_120_160_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_pair_timeout_gap_160_240_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  (snapshot->thermal_pair_timeout_gap_160_240_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->thermal_pair_timeout_gap_240_plus_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->thermal_pair_timeout_gap_240_plus_count != 0U) ? RED : DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "Grace:%lu Wait:%lu",
             (unsigned long)snapshot->thermal_pair_grace_ok_count,
             (unsigned long)snapshot->thermal_pair_wait_other_count);
    snprintf(footer2,
             sizeof(footer2),
             "Soft:%lu Back:%lu",
             (unsigned long)snapshot->thermal_pair_soft_timeout_count,
             (unsigned long)snapshot->thermal_pair_back_slot_null_count);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_bus_clear_diag(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[40];
    char footer2[40];

    if (snapshot == 0)
    {
        return;
    }

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_bus_clear_read_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  value,
                                  (snapshot->i2c_bus_clear_read_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_bus_clear_write_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->i2c_bus_clear_write_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_bus_clear_dma_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  (snapshot->i2c_bus_clear_dma_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->i2c_bus_clear_busy_timeout_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->i2c_bus_clear_busy_timeout_count != 0U) ? RED : DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "BusClr:%lu StopRel:%lu",
             (unsigned long)snapshot->i2c_bus_clear_count,
             (unsigned long)snapshot->i2c_stop_release_timeout_count);
    snprintf(footer2,
             sizeof(footer2),
             "PairTO:%lu I2:%lu",
             (unsigned long)snapshot->thermal_pair_timeout_count,
             (unsigned long)snapshot->i2c_failure_count);
    perf_baseline_draw_footer_text(footer1, footer2);
}

static void perf_baseline_draw_counters(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[64];
    char footer2[64];

    if (snapshot == 0)
    {
        return;
    }

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->key_queue_drop_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  value,
                                  (snapshot->key_queue_drop_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->ui_msg_drop_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->ui_msg_drop_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->service_queue_fail_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  (snapshot->service_queue_fail_count != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot->uart_error_count);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->uart_error_count != 0U) ? RED : DARKBLUE);

    snprintf(footer1,
             sizeof(footer1),
             "DT:%lu I2:%lu B:%lu DE:%lu T:%lu/%lu/%lu",
             (unsigned long)snapshot->dma_timeout_count,
             (unsigned long)snapshot->i2c_failure_count,
             (unsigned long)snapshot->thermal_backoff_count,
             (unsigned long)snapshot->lcd_dma_enter_count,
             (unsigned long)snapshot->dma_irq_tc_count,
             (unsigned long)snapshot->dma_irq_te_count,
             (unsigned long)snapshot->dma_wait_take_count);
    snprintf(footer2,
             sizeof(footer2),
             "3D:%lu/%lu/%lu C:%lu D:%lu/%lu/%lu W:%lu",
             (unsigned long)snapshot->thermal_3d_sync_present_attempt_count,
             (unsigned long)snapshot->thermal_3d_sync_present_ok_count,
             (unsigned long)snapshot->thermal_3d_sync_present_fail_count,
             (unsigned long)snapshot->thermal_3d_claim_count,
             (unsigned long)snapshot->thermal_3d_done_ok_count,
             (unsigned long)snapshot->thermal_3d_done_error_count,
             (unsigned long)snapshot->thermal_3d_done_cancel_count,
             (unsigned long)snapshot->thermal_3d_wait_timeout_count);
    perf_baseline_draw_footer_text(footer1, footer2);
}

/* 缁樺埗鎬ц兘鍩虹嚎鍋ュ悍鐘舵€侀〉銆?*/
static void perf_baseline_draw_health(const app_perf_baseline_snapshot_t *snapshot)
{
    char value[24];
    char footer1[48];
    char footer2[48];

    if (snapshot == 0)
    {
        return;
    }

    perf_baseline_format_hex32(value, sizeof(value), snapshot->watchdog_fault_flags);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U),
                                  value,
                                  (snapshot->watchdog_fault_flags != 0U) ? RED : DARKBLUE);

    perf_baseline_format_hex32(value, sizeof(value), snapshot->watchdog_missing_progress_mask);
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U),
                                  value,
                                  (snapshot->watchdog_missing_progress_mask != 0U) ? RED : DARKBLUE);

    snprintf(value, sizeof(value), "%s", (snapshot->thermal_active != 0U) ? "ON" : "OFF");
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U),
                                  value,
                                  (snapshot->thermal_active != 0U) ? GREEN : GRAYBLUE);

    snprintf(value, sizeof(value), "%s", (snapshot->screen_off != 0U) ? "OFF" : "ON");
    perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U),
                                  value,
                                  (snapshot->screen_off != 0U) ? BROWN : GREEN);

    snprintf(footer1,
             sizeof(footer1),
             "I:%lu S:%lu U:%lu",
             (unsigned long)snapshot->input_stack_words,
             (unsigned long)snapshot->service_stack_words,
             (unsigned long)snapshot->ui_stack_words);
    snprintf(footer2,
             sizeof(footer2),
             "D:%lu T:%lu P:%lu",
             (unsigned long)snapshot->display_stack_words,
             (unsigned long)snapshot->thermal_stack_words,
             (unsigned long)snapshot->power_stack_words);
    perf_baseline_draw_footer_text(footer1, footer2);
}

/* 鎬ц兘鍩虹嚎椤甸潰杩涘叆鍥炶皟锛屾牎楠屽彲瑙佹€у苟鍒濆鍖栧瓙椤电姸鎬併€?*/
static void perf_baseline_on_enter(ui_page_id_t previous_page)
{
    (void)previous_page;

    if (perf_baseline_debug_visible() == 0U)
    {
        ui_manager_navigate_home();
        return;
    }

    s_perf_baseline_subpage = 0U;
    s_perf_baseline_next_refresh_ms = 0U;
    ui_manager_force_full_refresh();
}

/* 鎬ц兘鍩虹嚎椤甸潰绂诲紑鍥炶皟锛屾竻鐞嗕笅涓€娆″埛鏂版椂闂淬€?*/
static void perf_baseline_on_leave(ui_page_id_t next_page)
{
    (void)next_page;
    s_perf_baseline_next_refresh_ms = 0U;
}

/* 澶勭悊鎬ц兘鍩虹嚎椤甸潰鎸夐敭锛氱炕椤点€佹竻闆舵垨杩斿洖棣栭〉銆?*/
static void perf_baseline_on_key(uint8_t key_value)
{
    if (key_value == KEY1_PRES)
    {
        s_perf_baseline_subpage = page_cycle_prev_index(s_perf_baseline_subpage, PERF_SUBPAGE_COUNT);
        ui_manager_force_full_refresh();
    }
    else if (key_value == KEY3_PRES)
    {
        s_perf_baseline_subpage = page_cycle_next_index(s_perf_baseline_subpage, PERF_SUBPAGE_COUNT);
        ui_manager_force_full_refresh();
    }
    else if (key_value == KEY2_PRES)
    {
        app_perf_baseline_reset();
        ui_manager_force_full_refresh();
    }
    else if (key_value == UI_KEY_KEY2_LONG)
    {
        ui_manager_navigate_home();
    }
}

/* 鎬ц兘鍩虹嚎椤甸潰鍛ㄦ湡鍥炶皟锛屾寜鑺傛祦鍛ㄦ湡瑙﹀彂鍒锋柊銆?*/
static void perf_baseline_on_tick(void)
{
    uint32_t now_ms = 0U;

    if (perf_baseline_debug_visible() == 0U)
    {
        ui_manager_navigate_home();
        return;
    }

    now_ms = power_manager_get_tick_ms();
    if (s_perf_baseline_next_refresh_ms == 0U ||
        now_ms >= s_perf_baseline_next_refresh_ms)
    {
        s_perf_baseline_next_refresh_ms = now_ms + PERF_BASELINE_REFRESH_MS;
        ui_manager_request_render();
    }
}

/* 鏍规嵁褰撳墠瀛愰〉缁樺埗鎬ц兘鍩虹嚎椤甸潰鍐呭銆?*/
static void perf_baseline_render(uint8_t full_refresh)
{
    app_perf_baseline_snapshot_t snapshot;

    app_perf_baseline_get_snapshot(&snapshot);

    if (full_refresh != 0U)
    {
        ui_renderer_draw_header_path("System", "Debug Page", GRAYBLUE);
        ui_renderer_clear_body(WHITE);
        perf_baseline_draw_layout(snapshot.enabled);
    }

    if (snapshot.enabled == 0U)
    {
        perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 0U), "DISABLED", RED);
        perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 1U), "APP_PERF=0", DARKBLUE);
        perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 2U), "Enable build", DARKBLUE);
        perf_baseline_draw_value_text(page_list_item_y(UI_CONTENT_TOP, 3U), "Screen only", DARKBLUE);
        perf_baseline_draw_footer_text("Perf baseline off", "Screen only");
        return;
    }

    switch (s_perf_baseline_subpage)
    {
    case 1U:
        perf_baseline_draw_timing(&snapshot);
        break;
    case 2U:
        perf_baseline_draw_counters(&snapshot);
        break;
    case 3U:
        perf_baseline_draw_health(&snapshot);
        break;
    case 4U:
        perf_baseline_draw_lcd_dma_detail(&snapshot);
        break;
    case 5U:
        perf_baseline_draw_thermal_flow_detail(&snapshot);
        break;
    case 6U:
        perf_baseline_draw_i2c_dma_detail(&snapshot);
        break;
    case 7U:
        perf_baseline_draw_i2c_dma_timeout_diag(&snapshot);
        break;
    case 8U:
        perf_baseline_draw_i2c_dma_tc_diag(&snapshot);
        break;
    case 9U:
        perf_baseline_draw_i2c_timeout_sources(&snapshot);
        break;
    case 10U:
        perf_baseline_draw_i2c_poll_timeout_detail(&snapshot);
        break;
    case 11U:
        perf_baseline_draw_pair_diag(&snapshot);
        break;
    case 12U:
        perf_baseline_draw_bus_clear_diag(&snapshot);
        break;
    case 13U:
        perf_baseline_draw_pair_bucket_diag(&snapshot);
        break;
    case 0U:
    default:
        perf_baseline_draw_snapshot(&snapshot);
        break;
    }
}

/* 鏍规嵁椤甸潰缂栧彿杩斿洖椤甸潰鍥炶皟琛ㄣ€?*/
const ui_page_ops_t *page_registry_get_ops(ui_page_id_t page_id)
{
    if (page_id >= UI_PAGE_COUNT)
    {
        return 0;
    }

    return &s_page_ops[page_id];
}

/* 杩斿洖鎸囧畾椤甸潰鐨勯€昏緫鐖堕〉闈€€?*/
ui_page_id_t page_registry_get_parent(ui_page_id_t page_id)
{
    switch (page_id)
    {
    case UI_PAGE_PERF_BASELINE:
        return UI_PAGE_SYSTEM;
    case UI_PAGE_STORAGE:
        return UI_PAGE_SYSTEM;
    case UI_PAGE_SNAPSHOT_REVIEW:
        return UI_PAGE_STORAGE;
    case UI_PAGE_ENGINEERING:
        return UI_PAGE_SYSTEM;
    case UI_PAGE_HOME:
        return UI_PAGE_HOME;
    default:
        return UI_PAGE_HOME;
    }
}

/* 灏嗘湇鍔″搷搴斿洖娴佺粰椤甸潰鍐呴儴澶勭悊閫昏緫銆?*/
void page_registry_on_service_response(const app_service_rsp_t *rsp)
{
    page_handle_service_response(rsp);
}
