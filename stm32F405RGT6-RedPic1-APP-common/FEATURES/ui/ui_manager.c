#include "ui_manager.h"

#include "FreeRTOS.h"
#include "task.h"

#include "app_display_runtime.h"
#include "page_registry.h"
#include "power_manager.h"

static ui_page_id_t s_active_page = UI_PAGE_HOME;
static uint8_t s_render_requested = 0U;
static uint8_t s_full_refresh_requested = 0U;

/* 根据页面编号查询对应的页面回调表。 */
static const ui_page_ops_t *ui_manager_get_page_ops(ui_page_id_t page_id)
{
    return page_registry_get_ops(page_id);
}

/*
 * 统一置位渲染请求标志。
 * 这里只在临界区内交接标志位，不在锁内运行页面逻辑，避免改变调度语义。
 */
static void ui_manager_set_render_request(uint8_t full_refresh_requested)
{
    taskENTER_CRITICAL();
    s_render_requested = 1U;
    if (full_refresh_requested != 0U)
    {
        s_full_refresh_requested = 1U;
    }
    taskEXIT_CRITICAL();
}

/* 取走当前待处理的渲染请求，并在同一临界区内完成清零。 */
static uint8_t ui_manager_take_render_request(uint8_t *full_refresh_requested)
{
    uint8_t render_requested = 0U;
    uint8_t full_refresh = 0U;

    taskENTER_CRITICAL();
    render_requested = s_render_requested;
    full_refresh = s_full_refresh_requested;
    if (render_requested != 0U || full_refresh != 0U)
    {
        s_render_requested = 0U;
        s_full_refresh_requested = 0U;
    }
    taskEXIT_CRITICAL();

    if (full_refresh_requested != 0)
    {
        *full_refresh_requested = full_refresh;
    }

    return ((render_requested != 0U) || (full_refresh != 0U)) ? 1U : 0U;
}

/* 判断当前电源状态是否允许继续驱动 UI 渲染。 */
static uint8_t ui_manager_is_render_blocked(void)
{
    return (power_manager_get_state() == POWER_STATE_SCREEN_OFF_IDLE) ? 1U : 0U;
}

/* 初始化 UI 管理器状态，并触发默认首页的进入回调。 */
void ui_manager_init(void)
{
    const ui_page_ops_t *ops = 0;

    s_active_page = UI_PAGE_HOME;
    s_render_requested = 1U;
    s_full_refresh_requested = 1U;

    ops = ui_manager_get_page_ops(s_active_page);
    if (ops != 0 && ops->on_enter != 0)
    {
        ops->on_enter(UI_PAGE_HOME);
    }
}

/* 将按键事件转发给当前活动页面。 */
void ui_manager_handle_key(uint8_t key_value)
{
    const ui_page_ops_t *ops = ui_manager_get_page_ops(s_active_page);

    if (ops != 0 && ops->on_key != 0)
    {
        ops->on_key(key_value);
    }
}

/* 处理异步服务响应，并补发一次页面刷新请求。 */
void ui_manager_handle_service_response(const app_service_rsp_t *rsp)
{
    page_registry_on_service_response(rsp);
    ui_manager_set_render_request(0U);
}

/* 执行一次 UI 调度循环。 */
void ui_manager_step(void)
{
    const ui_page_ops_t *ops = 0;
    uint8_t full_refresh_requested = 0U;
    uint8_t render_requested = 0U;

    if (ui_manager_is_render_blocked() != 0U)
    {
        return;
    }

    ops = ui_manager_get_page_ops(s_active_page);
    if (ops != 0 && ops->on_tick != 0)
    {
        ops->on_tick();
    }

    render_requested = ui_manager_take_render_request(&full_refresh_requested);
    if (ops != 0 && ops->render != 0 && render_requested != 0U)
    {
        (void)app_display_runtime_request_ui_render(ops->render, full_refresh_requested);
    }
}

/* 返回当前活动页面编号。 */
ui_page_id_t ui_manager_get_active_page(void)
{
    return s_active_page;
}

/* 请求一次普通页面刷新。 */
void ui_manager_request_render(void)
{
    ui_manager_set_render_request(0U);
}

/* 请求一次整页强制刷新。 */
void ui_manager_force_full_refresh(void)
{
    ui_manager_set_render_request(1U);
}

/* 执行页面切换，并按顺序调用离开/进入回调。 */
void ui_manager_navigate_to(ui_page_id_t page_id)
{
    const ui_page_ops_t *old_ops = 0;
    const ui_page_ops_t *new_ops = 0;
    ui_page_id_t previous_page = s_active_page;

    if (page_id >= UI_PAGE_COUNT || page_id == s_active_page)
    {
        return;
    }

    old_ops = ui_manager_get_page_ops(s_active_page);
    if (old_ops != 0 && old_ops->on_leave != 0)
    {
        old_ops->on_leave(page_id);
    }

    s_active_page = page_id;
    ui_manager_set_render_request(1U);

    new_ops = ui_manager_get_page_ops(s_active_page);
    if (new_ops != 0 && new_ops->on_enter != 0)
    {
        new_ops->on_enter(previous_page);
    }
}

/* 按页面注册表中的父页面关系执行返回。 */
void ui_manager_navigate_back(void)
{
    ui_page_id_t parent_page = page_registry_get_parent(s_active_page);

    if (parent_page != s_active_page)
    {
        ui_manager_navigate_to(parent_page);
    }
}

/* 直接导航回首页。 */
void ui_manager_navigate_home(void)
{
    ui_manager_navigate_to(UI_PAGE_HOME);
}
