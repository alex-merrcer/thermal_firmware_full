/**
 * @file    ota_menu.c
 * @brief   OTA 菜单 UI 模块
 * @note    本模块实现 OTA 升级/回滚的 LCD 菜单界面，支持：
 *          - 主菜单：升级、回滚、分区信息、退出
 *          - 升级检查：查询最新版本，确认升级
 *          - 回滚确认：显示目标分区信息，确认回滚
 *          - 分区信息：显示当前活跃分区、版本、试运行状态
 *          - 电池状态：实时显示电池电压和电量等级
 *
 * @par 按键映射
 *      - KEY1：上移选择 / 返回
 *      - KEY2：确认 / 执行
 *      - KEY3：下移选择
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "ota_menu.h"

#include <stdio.h>
#include <string.h>

#include "delay.h"
#include "battery_monitor.h"
#include "lcd_init.h"
#include "lcd.h"
#include "key.h"
#include "ota_service.h"
#include "ota_ctrl_protocol.h"

/* =========================================================================
 *  2. 内部枚举类型定义
 * ======================================================================= */

/**
 * @brief OTA 菜单状态枚举
 */
typedef enum
{
    OTA_MENU_STATE_HIDDEN = 0,          /**< 菜单隐藏（未激活）          */
    OTA_MENU_STATE_HOME,                /**< 主菜单                      */
    OTA_MENU_STATE_CONFIRM_UPGRADE,     /**< 升级确认页面                */
    OTA_MENU_STATE_CONFIRM_ROLLBACK,    /**< 回滚确认页面                */
    OTA_MENU_STATE_INFO                 /**< 信息展示页面                */
} ota_menu_state_t;

/**
 * @brief OTA 菜单项枚举
 */
typedef enum
{
    OTA_MENU_ITEM_UPGRADE = 0,          /**< 升级                        */
    OTA_MENU_ITEM_ROLLBACK,             /**< 回滚                        */
    OTA_MENU_ITEM_INFO,                 /**< 分区信息                    */
    OTA_MENU_ITEM_EXIT,                 /**< 退出菜单                    */
    OTA_MENU_ITEM_COUNT                 /**< 菜单项总数                  */
} ota_menu_item_t;

/* =========================================================================
 *  3. 模块级静态变量
 * ======================================================================= */

static ota_menu_state_t s_menu_state = OTA_MENU_STATE_HIDDEN;   /**< 当前菜单状态  */
static uint8_t s_selected_item = OTA_MENU_ITEM_UPGRADE;         /**< 当前选中项    */

/* ---- 信息页面文本缓冲区 ---- */
static char     s_latest_version[BOOT_INFO_VERSION_LEN];        /**< 最新版本号    */
static char     s_info_line1[32];                               /**< 信息行 1      */
static char     s_info_line2[32];                               /**< 信息行 2      */
static char     s_info_line3[32];                               /**< 信息行 3      */
static char     s_info_line4[32];                               /**< 信息行 4      */
static char     s_info_footer1[32];                             /**< 底部行 1      */
static char     s_info_footer2[32];                             /**< 底部行 2      */
static u16      s_info_title_color = BLUE;                      /**< 标题栏颜色    */
static char     s_info_title[24];                               /**< 标题文本      */

/* =========================================================================
 *  4. 布局常量定义
 * ======================================================================= */

/** @defgroup OTA_MENU_LAYOUT  菜单布局参数
 *  @{ */
#define OTA_MENU_HEADER_HEIGHT          32U     /**< 标题栏高度（像素）      */
#define OTA_MENU_HOME_STATUS_Y1         44U     /**< 状态行 1 Y 坐标        */
#define OTA_MENU_HOME_STATUS_Y2         68U     /**< 状态行 2 Y 坐标        */
#define OTA_MENU_HOME_ITEMS_Y           108U    /**< 菜单项起始 Y 坐标      */
#define OTA_MENU_HOME_ITEM_SPACING      24U     /**< 菜单项垂直间距（像素） */
#define OTA_MENU_HOME_ITEM_HEIGHT       18U     /**< 菜单项高度（像素）     */
#define OTA_MENU_HOME_ITEM_X            12U     /**< 菜单项文本 X 坐标      */
#define OTA_MENU_HOME_ITEM_CLEAR_X      8U      /**< 菜单项清除区域起始 X   */
#define OTA_MENU_HOME_ITEM_CLEAR_END    8U      /**< 菜单项清除区域末端偏移 */
/** @} */

/* =========================================================================
 *  5. 菜单项文本
 * ======================================================================= */

static const char *s_home_items[OTA_MENU_ITEM_COUNT] =
{
    "Upgrade",                          /**< 升级                        */
    "Rollback",                         /**< 回滚                        */
    "Partition Info",                   /**< 分区信息                    */
    "Exit Menu"                         /**< 退出菜单                    */
};

/* =========================================================================
 *  6. 电池状态显示
 * ======================================================================= */

/**
 * @brief  获取电池电量等级的文本标签
 * @param  level — 电池电量等级
 * @return 电量文本字符串
 */
static const char *ota_menu_battery_level_text(battery_level_t level)
{
    switch (level)
    {
    case BATTERY_LEVEL_FULL:
        return "FULL";

    case BATTERY_LEVEL_HIGH:
        return "HIGH";

    case BATTERY_LEVEL_MEDIUM:
        return "MID";

    case BATTERY_LEVEL_LOW:
        return "LOW";

    case BATTERY_LEVEL_ALERT:
    default:
        return "ALM";
    }
}

/**
 * @brief  获取电池电量等级的显示颜色
 * @param  level — 电池电量等级
 * @return LCD 颜色值
 */
static u16 ota_menu_battery_level_color(battery_level_t level)
{
    switch (level)
    {
    case BATTERY_LEVEL_FULL:
    case BATTERY_LEVEL_HIGH:
        return GREEN;

    case BATTERY_LEVEL_MEDIUM:
        return YELLOW;

    case BATTERY_LEVEL_LOW:
        return BRED;

    case BATTERY_LEVEL_ALERT:
    default:
        return RED;
    }
}

/**
 * @brief  绘制电池状态信息
 * @param  title_color — 标题栏背景色
 */
static void ota_menu_draw_battery_status(u16 title_color)
{
    char     battery_text[20];
    uint16_t battery_mv  = battery_monitor_get_mv();
    battery_level_t battery_level = battery_monitor_get_level();
    uint16_t text_len    = 0U;
    uint16_t text_x      = 0U;

    snprintf(battery_text,
             sizeof(battery_text),
             "BAT:%umV %s",
             battery_mv,
             ota_menu_battery_level_text(battery_level));

    text_len = (uint16_t)strlen(battery_text);
    text_x   = (uint16_t)(LCD_W - 8U - (text_len * 8U));

    LCD_ShowString(text_x, 8,
                   (const u8 *)battery_text,
                   ota_menu_battery_level_color(battery_level),
                   title_color, 16, 0);
}

/**
 * @brief  刷新电池状态区域
 * @param  title_color — 标题栏背景色
 */
static void ota_menu_refresh_battery_status(u16 title_color)
{
    uint16_t clear_x = 0U;

    clear_x = (LCD_W > 160U) ? (uint16_t)(LCD_W - 160U) : 0U;
    LCD_Fill(clear_x, 0, LCD_W - 1U, OTA_MENU_HEADER_HEIGHT - 1U, title_color);
    ota_menu_draw_battery_status(title_color);
}

/* =========================================================================
 *  7. 缓冲区管理
 * ======================================================================= */

/**
 * @brief  清零所有信息页面文本缓冲区
 */
static void ota_menu_clear_buffers(void)
{
    memset(s_latest_version, 0, sizeof(s_latest_version));
    memset(s_info_line1, 0, sizeof(s_info_line1));
    memset(s_info_line2, 0, sizeof(s_info_line2));
    memset(s_info_line3, 0, sizeof(s_info_line3));
    memset(s_info_line4, 0, sizeof(s_info_line4));
    memset(s_info_footer1, 0, sizeof(s_info_footer1));
    memset(s_info_footer2, 0, sizeof(s_info_footer2));
    memset(s_info_title, 0, sizeof(s_info_title));
}

/* =========================================================================
 *  8. 页面绘制基础函数
 * ======================================================================= */

/**
 * @brief  绘制页面画布（白色背景 + 标题栏）
 * @param  title       — 标题文本
 * @param  title_color — 标题栏背景色
 */
static void ota_menu_draw_canvas(const char *title, u16 title_color)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, WHITE);
    LCD_Fill(0, 0, LCD_W, OTA_MENU_HEADER_HEIGHT, title_color);

    if (title != 0)
    {
        LCD_ShowString(8, 8, (const u8 *)title, WHITE, title_color, 16, 0);
    }

    ota_menu_draw_battery_status(title_color);
}

/**
 * @brief  绘制页面底部提示（当前为空实现）
 * @param  line1 — 底部行 1
 * @param  line2 — 底部行 2
 */
static void ota_menu_draw_footer(const char *line1, const char *line2)
{
    (void)line1;
    (void)line2;
}

/* =========================================================================
 *  9. 主菜单页面绘制
 * ======================================================================= */

/**
 * @brief  绘制主菜单状态信息（运行分区、版本、试运行状态）
 */
static void ota_menu_draw_home_status(void)
{
    const char *current_version = 0;
    const char *active_name     = 0;
    char line1[32];
    char line2[32];

    current_version = ota_service_get_display_version();
    active_name     = ota_service_get_partition_name(ota_service_get_active_partition());

    /* 清除状态区域 */
    LCD_Fill(8, OTA_MENU_HOME_STATUS_Y1, LCD_W - 8U, OTA_MENU_HOME_STATUS_Y1 + 16U, WHITE);
    LCD_Fill(8, OTA_MENU_HOME_STATUS_Y2, LCD_W - 8U, OTA_MENU_HOME_STATUS_Y2 + 16U, WHITE);

    /* 格式化状态文本 */
    snprintf(line1, sizeof(line1), "Run: %s  Ver: %s", active_name, current_version);
    snprintf(line2, sizeof(line2), "Tries: %lu  Trial: %s",
             (unsigned long)ota_service_get_boot_info()->boot_tries,
             (ota_service_get_boot_info()->trial_state == BOOT_INFO_TRIAL_PENDING) ? "PEND" : "OK");

    LCD_ShowString(8, OTA_MENU_HOME_STATUS_Y1, (const u8 *)line1, BLACK, WHITE, 16, 0);
    LCD_ShowString(8, OTA_MENU_HOME_STATUS_Y2, (const u8 *)line2, BLACK, WHITE, 16, 0);
}

/**
 * @brief  绘制单个主菜单项
 * @param  item_index — 菜单项索引
 */
static void ota_menu_draw_home_item(uint8_t item_index)
{
    char item_line[24];
    u16  item_y = 0U;
    u16  color  = BLACK;

    if (item_index >= OTA_MENU_ITEM_COUNT)
    {
        return;
    }

    item_y = (u16)(OTA_MENU_HOME_ITEMS_Y + (item_index * OTA_MENU_HOME_ITEM_SPACING));
    color  = (item_index == s_selected_item) ? RED : BLACK;

    /* 清除菜单项区域 */
    LCD_Fill(OTA_MENU_HOME_ITEM_CLEAR_X, item_y,
             LCD_W - OTA_MENU_HOME_ITEM_CLEAR_END,
             (u16)(item_y + OTA_MENU_HOME_ITEM_HEIGHT), WHITE);

    /* 绘制菜单项文本（选中项前显示 '>'） */
    snprintf(item_line, sizeof(item_line), "%c %s",
             (item_index == s_selected_item) ? '>' : ' ',
             s_home_items[item_index]);

    LCD_ShowString(OTA_MENU_HOME_ITEM_X, item_y,
                   (const u8 *)item_line, color, WHITE, 16, 0);
}

/**
 * @brief  绘制所有主菜单项
 */
static void ota_menu_draw_home_items(void)
{
    uint8_t item_index = 0U;

    for (item_index = 0U; item_index < OTA_MENU_ITEM_COUNT; ++item_index)
    {
        ota_menu_draw_home_item(item_index);
    }
}

/**
 * @brief  刷新主菜单选中状态（重绘上一项和当前项）
 * @param  previous_item — 上一个选中项索引
 */
static void ota_menu_redraw_home_selection(uint8_t previous_item)
{
    ota_menu_draw_home_item(previous_item);
    ota_menu_draw_home_item(s_selected_item);
    ota_menu_refresh_battery_status(BLUE);
}

/* =========================================================================
 *  10. 信息页面绘制
 * ======================================================================= */

/**
 * @brief  渲染当前信息页面
 */
static void ota_menu_render_info_page(void)
{
    ota_menu_draw_canvas(s_info_title, s_info_title_color);
    LCD_ShowString(8, 52,  (const u8 *)s_info_line1, BLACK, WHITE, 16, 0);
    LCD_ShowString(8, 78,  (const u8 *)s_info_line2, BLACK, WHITE, 16, 0);
    LCD_ShowString(8, 104, (const u8 *)s_info_line3, BLACK, WHITE, 16, 0);
    LCD_ShowString(8, 130, (const u8 *)s_info_line4, BLACK, WHITE, 16, 0);
    ota_menu_draw_footer(s_info_footer1, s_info_footer2);
}

/**
 * @brief  设置信息页面内容并渲染
 * @param  title       — 标题文本
 * @param  title_color — 标题栏颜色
 * @param  line1~line4 — 信息行文本
 * @param  footer1     — 底部行 1
 * @param  footer2     — 底部行 2
 */
static void ota_menu_set_info_page(const char *title,
                                   u16 title_color,
                                   const char *line1,
                                   const char *line2,
                                   const char *line3,
                                   const char *line4,
                                   const char *footer1,
                                   const char *footer2)
{
    ota_menu_clear_buffers();
    s_info_title_color = title_color;

    if (title != 0)   { snprintf(s_info_title,  sizeof(s_info_title),  "%s", title);  }
    if (line1 != 0)   { snprintf(s_info_line1,   sizeof(s_info_line1),   "%s", line1);  }
    if (line2 != 0)   { snprintf(s_info_line2,   sizeof(s_info_line2),   "%s", line2);  }
    if (line3 != 0)   { snprintf(s_info_line3,   sizeof(s_info_line3),   "%s", line3);  }
    if (line4 != 0)   { snprintf(s_info_line4,   sizeof(s_info_line4),   "%s", line4);  }
    if (footer1 != 0) { snprintf(s_info_footer1, sizeof(s_info_footer1), "%s", footer1);}
    if (footer2 != 0) { snprintf(s_info_footer2, sizeof(s_info_footer2), "%s", footer2);}

    ota_menu_render_info_page();
}

/* =========================================================================
 *  11. 各页面显示逻辑
 * ======================================================================= */

/**
 * @brief  显示主菜单页面
 */
static void ota_menu_show_home(void)
{
    ota_service_refresh_info();
    ota_menu_draw_canvas("OTA MENU", BLUE);
    ota_menu_draw_home_status();
    ota_menu_draw_home_items();
    ota_menu_draw_footer("KEY1 Up   KEY3 Down", "KEY2 Enter");
}

/**
 * @brief  显示"正在检查"等待页面
 */
static void ota_menu_show_checking(void)
{
    char line[32];

    snprintf(line, sizeof(line), "Current: %s", ota_service_get_display_version());
    ota_menu_set_info_page("CHECK OTA", BLUE,
                           line, "Wait ESP32...", "", "", "", "");
}

/**
 * @brief  显示升级检查结果页面
 * @note   查询最新版本，根据比较结果显示：
 *         - 有更新：进入确认升级页面
 *         - 已是最新：显示提示
 *         - 查询失败：显示错误原因
 */
static void ota_menu_show_upgrade_result(void)
{
    char current_line[32];
    char latest_line[32];
    uint16_t reject_reason = 0U;
    const char *current_version = ota_service_get_display_version();

    ota_menu_show_checking();

    if (ota_service_query_latest_version(s_latest_version,
                                         sizeof(s_latest_version),
                                         &reject_reason) != 0U)
    {
        if (ota_service_compare_version(s_latest_version, current_version) > 0)
        {
            /* 有新版本：进入确认升级页面 */
            s_menu_state = OTA_MENU_STATE_CONFIRM_UPGRADE;
            snprintf(current_line, sizeof(current_line), "Current: %s", current_version);
            snprintf(latest_line,  sizeof(latest_line),  "Latest: %s",  s_latest_version);
            ota_menu_set_info_page("UPGRADE", GREEN,
                                   "Update found", current_line, latest_line, "",
                                   "KEY1 Back", "KEY2 Upgrade");
            return;
        }

        /* 已是最新版本 */
        snprintf(current_line, sizeof(current_line), "Current: %s", current_version);
        snprintf(latest_line,  sizeof(latest_line),  "Latest: %s",  s_latest_version);
        s_menu_state = OTA_MENU_STATE_INFO;
        ota_menu_set_info_page("UPGRADE", GREEN,
                               "Already latest", current_line, latest_line, "",
                               "KEY1 Back", "");
        return;
    }

    if (reject_reason == OTA_CTRL_ERR_NO_UPDATE)
    {
        /* 无可用更新 */
        snprintf(current_line, sizeof(current_line), "Current: %s", current_version);
        s_menu_state = OTA_MENU_STATE_INFO;
        ota_menu_set_info_page("UPGRADE", GREEN,
                               "Already latest", current_line, "", "",
                               "KEY1 Back", "");
        return;
    }

    /* 查询失败：显示错误原因 */
    s_menu_state = OTA_MENU_STATE_INFO;
    ota_menu_set_info_page("UPGRADE", RED,
                           "Check failed",
                           ota_service_reason_text(reject_reason), "", "",
                           "KEY1 Back", "");
}

/**
 * @brief  显示回滚确认页面
 */
static void ota_menu_show_rollback_prompt(void)
{
    char line1[32];
    char line2[32];
    uint32_t rollback_partition = 0U;

    ota_service_refresh_info();
    rollback_partition = ota_service_get_inactive_partition();

    snprintf(line1, sizeof(line1), "Target: %s",
             ota_service_get_partition_name(rollback_partition));
    snprintf(line2, sizeof(line2), "Version: %s",
             ota_service_get_partition_version(rollback_partition));

    s_menu_state = OTA_MENU_STATE_CONFIRM_ROLLBACK;
    ota_menu_set_info_page("ROLLBACK", YELLOW,
                           line1, line2, "Bootloader will switch", "",
                           "KEY1 Back", "KEY2 Rollback");
}

/**
 * @brief  显示分区信息页面
 */
static void ota_menu_show_partition_info(void)
{
    char line1[32];
    char line2[32];
    char line3[32];
    char line4[32];
    uint32_t active_partition   = 0U;
    uint32_t inactive_partition = 0U;

    ota_service_refresh_info();
    active_partition   = ota_service_get_active_partition();
    inactive_partition = ota_service_get_inactive_partition();

    snprintf(line1, sizeof(line1), "Active: %s",
             ota_service_get_partition_name(active_partition));
    snprintf(line2, sizeof(line2), "CurVer: %s",
             ota_service_get_display_version());
    snprintf(line3, sizeof(line3), "%s: %s",
             ota_service_get_partition_name(inactive_partition),
             ota_service_get_partition_version(inactive_partition));
    snprintf(line4, sizeof(line4), "Trial:%s Try:%lu",
             (ota_service_get_boot_info()->trial_state == BOOT_INFO_TRIAL_PENDING) ? "PEND" : "OK",
             (unsigned long)ota_service_get_boot_info()->boot_tries);

    s_menu_state = OTA_MENU_STATE_INFO;
    ota_menu_set_info_page("PARTITION", BLUE,
                           line1, line2, line3, line4,
                           "KEY1 Back", "");
}

/* =========================================================================
 *  12. 公共接口实现
 * ======================================================================= */

/**
 * @brief  初始化 OTA 菜单
 */
void ota_menu_init(void)
{
    ota_menu_clear_buffers();
    s_menu_state   = OTA_MENU_STATE_HIDDEN;
    s_selected_item = OTA_MENU_ITEM_UPGRADE;
}

/**
 * @brief  进入 OTA 菜单
 */
void ota_menu_enter(void)
{
    s_menu_state   = OTA_MENU_STATE_HOME;
    s_selected_item = OTA_MENU_ITEM_UPGRADE;
    ota_menu_show_home();
}

/**
 * @brief  退出 OTA 菜单
 */
void ota_menu_exit(void)
{
    s_menu_state = OTA_MENU_STATE_HIDDEN;
    ota_menu_clear_buffers();
}

/**
 * @brief  查询菜单是否处于激活状态
 * @retval 1 — 菜单激活；0 — 菜单隐藏
 */
uint8_t ota_menu_is_active(void)
{
    return (s_menu_state != OTA_MENU_STATE_HIDDEN) ? 1U : 0U;
}

/* =========================================================================
 *  13. 按键事件处理
 * ======================================================================= */

/**
 * @brief  处理按键事件
 * @note   根据当前菜单状态分发按键处理：
 *         - HOME 状态：KEY1 上移、KEY3 下移、KEY2 确认
 *         - CONFIRM_UPGRADE 状态：KEY1 返回、KEY2 执行升级
 *         - CONFIRM_ROLLBACK 状态：KEY1 返回、KEY2 执行回滚
 *         - INFO 状态：KEY1 返回主菜单
 * @param  key_value — 按键值
 */
void ota_menu_handle_key(uint8_t key_value)
{
    if (s_menu_state == OTA_MENU_STATE_HIDDEN)
    {
        return;
    }

    /* ---- 主菜单状态 ---- */
    if (s_menu_state == OTA_MENU_STATE_HOME)
    {
        if (key_value == KEY1_PRES)
        {
            /* 上移选择 */
            uint8_t previous_item = s_selected_item;
            s_selected_item = (uint8_t)((s_selected_item + OTA_MENU_ITEM_COUNT - 1U) % OTA_MENU_ITEM_COUNT);
            ota_menu_redraw_home_selection(previous_item);
        }
        else if (key_value == KEY3_PRES)
        {
            /* 下移选择 */
            uint8_t previous_item = s_selected_item;
            s_selected_item = (uint8_t)((s_selected_item + 1U) % OTA_MENU_ITEM_COUNT);
            ota_menu_redraw_home_selection(previous_item);
        }
        else if (key_value == KEY2_PRES)
        {
            /* 确认选中项 */
            switch (s_selected_item)
            {
            case OTA_MENU_ITEM_UPGRADE:
                ota_menu_show_upgrade_result();
                break;

            case OTA_MENU_ITEM_ROLLBACK:
                ota_menu_show_rollback_prompt();
                break;

            case OTA_MENU_ITEM_INFO:
                ota_menu_show_partition_info();
                break;

            default:
                ota_menu_exit();
                break;
            }
        }
        return;
    }

    /* ---- 升级确认状态 ---- */
    if (s_menu_state == OTA_MENU_STATE_CONFIRM_UPGRADE)
    {
        if (key_value == KEY1_PRES)
        {
            s_menu_state = OTA_MENU_STATE_HOME;
            ota_menu_show_home();
        }
        else if (key_value == KEY2_PRES)
        {
            ota_menu_set_info_page("UPGRADE", GREEN,
                                   "Reboot to bootloader", s_latest_version, "", "", "", "");
            delay_ms(200);
            ota_service_request_upgrade();
        }
        return;
    }

    /* ---- 回滚确认状态 ---- */
    if (s_menu_state == OTA_MENU_STATE_CONFIRM_ROLLBACK)
    {
        if (key_value == KEY1_PRES)
        {
            s_menu_state = OTA_MENU_STATE_HOME;
            ota_menu_show_home();
        }
        else if (key_value == KEY2_PRES)
        {
            ota_menu_set_info_page("ROLLBACK", YELLOW,
                                   "Reboot to bootloader", "", "", "", "", "");
            delay_ms(200);
            ota_service_request_rollback();
        }
        return;
    }

    /* ---- 信息页面状态：KEY1 返回主菜单 ---- */
    if (key_value == KEY1_PRES)
    {
        s_menu_state = OTA_MENU_STATE_HOME;
        ota_menu_show_home();
    }
}
