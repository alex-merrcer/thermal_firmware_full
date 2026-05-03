/**
 * @file    ui_manager.c
 * @brief   UI 页面管理器 —— 导航状态机与渲染调度
 * @note    本模块负责管理 UI 页面的生命周期、导航切换和渲染调度。
 *
 * @par 页面生命周期
 *      每个页面通过 ui_page_ops_t 回调表注册：
 *      - on_enter: 进入页面时调用（携带前一页面 ID）
 *      - on_leave: 离开页面时调用（携带目标页面 ID）
 *      - on_key:   按键事件转发
 *      - on_tick:  周期性调度回调
 *      - render:   渲染回调（由显示运行时异步调用）
 *
 * @par 渲染请求机制
 *      使用标志位（临界区保护）实现延迟渲染：
 *      1. 外部模块通过 request_render() / force_full_refresh() 置位
 *      2. ui_manager_step() 中取走标志并提交到显示运行时
 *      3. 支持普通刷新和整页强制刷新两种模式
 *
 * @par 电源状态阻塞
 *      当电源状态为 SCREEN_OFF_IDLE 时，跳过所有渲染和调度。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "ui_manager.h"

#include "FreeRTOS.h"
#include "task.h"

#include "app_display_runtime.h"
#include "page_registry.h"
#include "power_manager.h"

/* =========================================================================
 *  2. 模块级静态变量
 * ======================================================================= */

static ui_page_id_t s_active_page          = UI_PAGE_HOME; /**< 当前活动页面 ID     */
static uint8_t      s_render_requested     = 0U;           /**< 渲染请求标志        */
static uint8_t      s_full_refresh_requested = 0U;         /**< 整页刷新请求标志    */

/* =========================================================================
 *  3. 内部函数实现 —— 页面回调表查询
 * ======================================================================= */

/**
 * @brief  根据页面编号查询对应的页面回调表
 * @param  page_id — 页面编号
 * @return 页面回调表指针（未找到时返回 0）
 */
static const ui_page_ops_t *ui_manager_get_page_ops(ui_page_id_t page_id)
{
    return page_registry_get_ops(page_id);
}

/* =========================================================================
 *  4. 内部函数实现 —— 渲染请求标志管理
 * ======================================================================= */

/**
 * @brief  统一置位渲染请求标志
 * @note   仅在临界区内交接标志位，不在锁内运行页面逻辑。
 * @param  full_refresh_requested — 是否请求整页强制刷新
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

/**
 * @brief  取走当前待处理的渲染请求并清零
 * @param  full_refresh_requested — 输出：是否为整页刷新
 * @retval 1 — 有待处理的渲染请求；0 — 无请求
 */
static uint8_t ui_manager_take_render_request(uint8_t *full_refresh_requested)
{
    uint8_t render_requested = 0U;
    uint8_t full_refresh = 0U;

    /* 在同一临界区内读取并清零，防止竞态 */
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

/* =========================================================================
 *  5. 内部函数实现 —— 电源状态检查
 * ======================================================================= */

/**
 * @brief  判断当前电源状态是否阻塞 UI 渲染
 * @retval 1 — 渲染被阻塞（熄屏空闲状态）；0 — 允许渲染
 */
static uint8_t ui_manager_is_render_blocked(void)
{
    return (power_manager_get_state() == POWER_STATE_SCREEN_OFF_IDLE) ? 1U : 0U;
}

/* =========================================================================
 *  6. 公共接口实现 —— 初始化
 * ======================================================================= */

/**
 * @brief  初始化 UI 管理器
 * @note   设置默认首页，触发首次全页刷新和进入回调。
 */
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

/* =========================================================================
 *  7. 公共接口实现 —— 事件分发
 * ======================================================================= */

/**
 * @brief  将按键事件转发给当前活动页面
 * @param  key_value — 按键值
 */
void ui_manager_handle_key(uint8_t key_value)
{
    const ui_page_ops_t *ops = ui_manager_get_page_ops(s_active_page);

    if (ops != 0 && ops->on_key != 0)
    {
        ops->on_key(key_value);
    }
}

/**
 * @brief  处理异步服务响应并请求页面刷新
 * @param  rsp — 服务响应指针
 */
void ui_manager_handle_service_response(const app_service_rsp_t *rsp)
{
    page_registry_on_service_response(rsp);
    ui_manager_set_render_request(0U);
}

/* =========================================================================
 *  8. 公共接口实现 —— 调度循环
 * ======================================================================= */

/**
 * @brief  执行一次 UI 调度循环
 * @note   流程：
 *         1. 检查电源状态是否阻塞渲染
 *         2. 调用当前页面的 on_tick 回调
 *         3. 取走渲染请求标志并提交到显示运行时
 */
void ui_manager_step(void)
{
    const ui_page_ops_t *ops = 0;
    uint8_t full_refresh_requested = 0U;
    uint8_t render_requested = 0U;

    /* 熄屏空闲状态跳过所有调度 */
    if (ui_manager_is_render_blocked() != 0U)
    {
        return;
    }

    /* 调用页面周期回调 */
    ops = ui_manager_get_page_ops(s_active_page);
    if (ops != 0 && ops->on_tick != 0)
    {
        ops->on_tick();
    }

    /* 取走渲染请求并提交到显示运行时 */
    render_requested = ui_manager_take_render_request(&full_refresh_requested);
    if (ops != 0 && ops->render != 0 && render_requested != 0U)
    {
        (void)app_display_runtime_request_ui_render(ops->render, full_refresh_requested);
    }
}

/* =========================================================================
 *  9. 公共接口实现 —— 页面状态查询
 * ======================================================================= */

/**
 * @brief  获取当前活动页面编号
 * @return 活动页面 ID
 */
ui_page_id_t ui_manager_get_active_page(void)
{
    return s_active_page;
}

/* =========================================================================
 *  10. 公共接口实现 —— 渲染请求
 * ======================================================================= */

/**
 * @brief  请求一次普通页面刷新
 */
void ui_manager_request_render(void)
{
    ui_manager_set_render_request(0U);
}

/**
 * @brief  请求一次整页强制刷新
 */
void ui_manager_force_full_refresh(void)
{
    ui_manager_set_render_request(1U);
}

/* =========================================================================
 *  11. 公共接口实现 —— 页面导航
 * ======================================================================= */

/**
 * @brief  执行页面切换
 * @note   按顺序调用：旧页面 on_leave → 更新活动页面 → 新页面 on_enter。
 *         切换后自动触发整页刷新。
 * @param  page_id — 目标页面编号
 */
void ui_manager_navigate_to(ui_page_id_t page_id)
{
    const ui_page_ops_t *old_ops = 0;
    const ui_page_ops_t *new_ops = 0;
    ui_page_id_t previous_page = s_active_page;

    /* 边界检查：无效页面或重复导航 */
    if (page_id >= UI_PAGE_COUNT || page_id == s_active_page)
    {
        return;
    }

    /* 调用旧页面的离开回调 */
    old_ops = ui_manager_get_page_ops(s_active_page);
    if (old_ops != 0 && old_ops->on_leave != 0)
    {
        old_ops->on_leave(page_id);
    }

    /* 更新活动页面并触发整页刷新 */
    s_active_page = page_id;
    ui_manager_set_render_request(1U);

    /* 调用新页面的进入回调 */
    new_ops = ui_manager_get_page_ops(s_active_page);
    if (new_ops != 0 && new_ops->on_enter != 0)
    {
        new_ops->on_enter(previous_page);
    }
}

/**
 * @brief  按页面注册表中的父页面关系执行返回
 */
void ui_manager_navigate_back(void)
{
    ui_page_id_t parent_page = page_registry_get_parent(s_active_page);

    if (parent_page != s_active_page)
    {
        ui_manager_navigate_to(parent_page);
    }
}

/**
 * @brief  直接导航回首页
 */
void ui_manager_navigate_home(void)
{
    ui_manager_navigate_to(UI_PAGE_HOME);
}
