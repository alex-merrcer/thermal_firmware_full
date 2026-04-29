#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdint.h>

#include "redpic1_app.h"

/* 页面标识枚举，顺序必须与页面注册表中的回调表索引保持一致。 */
typedef enum
{
    UI_PAGE_HOME = 0,
    UI_PAGE_THERMAL,
    UI_PAGE_OTA_CENTER,
    UI_PAGE_CONNECTIVITY,
    UI_PAGE_POWER,
    UI_PAGE_SYSTEM,
    UI_PAGE_STORAGE,
    UI_PAGE_ENGINEERING,
    UI_PAGE_PERF_BASELINE,
    UI_PAGE_COUNT
} ui_page_id_t;

/* 页面回调集合，所有回调都运行在 UI 任务上下文中。 */
typedef struct
{
    /* 页面进入时回调，参数为上一个页面。 */
    void (*on_enter)(ui_page_id_t previous_page);
    /* 页面离开时回调，参数为即将进入的页面。 */
    void (*on_leave)(ui_page_id_t next_page);
    /* 页面按键处理回调。 */
    void (*on_key)(uint8_t key_value);
    /* 页面周期调度回调。 */
    void (*on_tick)(void);
    /* 页面渲染回调，参数指示是否需要整页刷新。 */
    void (*render)(uint8_t full_refresh);
} ui_page_ops_t;

/* 输入链路上报的 KEY2 长按合成键值。 */
#define UI_KEY_KEY2_LONG   0x82U

/* 初始化 UI 状态机，并进入默认首页。 */
void ui_manager_init(void);
/* 将按键事件转发给当前活动页面。 */
void ui_manager_handle_key(uint8_t key_value);
/* 在 UI 任务上下文中，让当前页面处理异步服务响应。 */
void ui_manager_handle_service_response(const app_service_rsp_t *rsp);
/* 执行一次 UI 调度步进：驱动页面 tick，并在需要时提交渲染请求。 */
void ui_manager_step(void);
/* 获取当前由 UI 管理器持有的活动页面。 */
ui_page_id_t ui_manager_get_active_page(void);
/* 请求一次增量刷新，不强制重绘整页布局。 */
void ui_manager_request_render(void);
/* 请求一次整页刷新，同时重放页面布局绘制。 */
void ui_manager_force_full_refresh(void);
/* 跳转到指定的已注册页面。 */
void ui_manager_navigate_to(ui_page_id_t page_id);
/* 按页面注册表声明的父页面关系执行返回导航。 */
void ui_manager_navigate_back(void);
/* 直接跳转回首页。 */
void ui_manager_navigate_home(void);

#endif
