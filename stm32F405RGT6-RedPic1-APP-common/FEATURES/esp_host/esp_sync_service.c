/**
 * @file    esp_sync_service.c
 * @brief   ESP 主机设置同步服务模块
 * @note    本模块负责将 STM32 本地设备设置同步到 ESP32 主机。
 *          支持延迟调度、自动重试和电源状态变化触发同步。
 *
 * @par 同步流程
 *      1. 通过 esp_sync_service_schedule 调度一次同步任务
 *      2. esp_sync_service_step 在主循环中检测到时间到达后执行同步
 *      3. 同步失败时自动重试，直到达到最大重试次数
 *      4. 电源状态从灭屏空闲恢复时自动触发一次同步
 *
 * @par 设置来源
 *      优先使用注册的 settings_copy_fn 回调获取设置副本，
 *      未注册时直接读取 settings_service_get() 全局指针。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "esp_sync_service.h"

#include <string.h>

#include "esp_host_service_priv.h"
#include "power_manager.h"
#include "settings_service.h"

/* =========================================================================
 *  2. 内部数据类型定义
 * ======================================================================= */

/**
 * @brief 同步服务状态结构体
 * @note  跟踪同步调度状态、重试参数和电源状态。
 */
typedef struct
{
    uint8_t         pending;            /**< 是否有待执行的同步任务          */
    uint8_t         tries;              /**< 当前已尝试次数                  */
    uint32_t        next_ms;            /**< 下次执行时间（系统 tick ms）    */
    uint32_t        retry_ms;           /**< 重试间隔（ms）                  */
    uint8_t         max_tries;          /**< 最大重试次数                    */
    power_state_t   last_power_state;   /**< 上次电源状态（用于变化检测）    */
} esp_sync_state_t;

/* =========================================================================
 *  3. 模块级静态变量
 * ======================================================================= */

static esp_sync_state_t s_esp_sync_state;                              /**< 同步服务状态          */
static esp_sync_service_settings_copy_fn_t s_settings_copy_fn = 0;     /**< 设置副本回调函数指针  */

/* =========================================================================
 *  4. 内部函数前向声明
 * ======================================================================= */

static uint8_t esp_sync_service_sync_now(void);

/* =========================================================================
 *  5. 公共接口实现 —— 回调注册与初始化
 * ======================================================================= */

/**
 * @brief  注册设置副本获取回调函数
 * @note   回调函数负责将当前设备设置拷贝到输出参数。
 *         未注册时使用 settings_service_get() 直接读取。
 * @param  settings_copy_fn — 回调函数指针
 */
void esp_sync_service_register_settings_copy(esp_sync_service_settings_copy_fn_t settings_copy_fn)
{
    s_settings_copy_fn = settings_copy_fn;
}

/**
 * @brief  重置同步服务状态
 * @note   清零所有内部状态，并设置初始电源状态。
 * @param  initial_power_state — 初始电源状态
 */
void esp_sync_service_reset(power_state_t initial_power_state)
{
    memset(&s_esp_sync_state, 0, sizeof(s_esp_sync_state));
    s_esp_sync_state.last_power_state = initial_power_state;
}

/* =========================================================================
 *  6. 公共接口实现 —— 同步调度与执行
 * ======================================================================= */

/**
 * @brief  调度一次同步任务
 * @note   设置延迟时间和重试参数，待 step 函数检测到时间到达后执行。
 * @param  delay_ms  — 首次执行延迟（ms）
 * @param  retry_ms  — 重试间隔（ms）
 * @param  max_tries — 最大重试次数
 */
void esp_sync_service_schedule(uint32_t delay_ms, uint32_t retry_ms, uint8_t max_tries)
{
    s_esp_sync_state.pending    = 1U;
    s_esp_sync_state.tries      = 0U;
    s_esp_sync_state.next_ms    = power_manager_get_tick_ms() + delay_ms;
    s_esp_sync_state.retry_ms   = retry_ms;
    s_esp_sync_state.max_tries  = max_tries;
}

/**
 * @brief  同步服务周期处理
 * @note   在主循环中调用，检测是否有待执行的同步任务。
 *         时间到达后尝试同步，失败时按配置重试。
 * @param  now_ms — 当前系统时间（ms）
 */
void esp_sync_service_step(uint32_t now_ms)
{
    /* 无待执行任务或时间未到，跳过 */
    if (s_esp_sync_state.pending == 0U ||
        now_ms < s_esp_sync_state.next_ms)
    {
        return;
    }

    /* 尝试同步 */
    if (esp_sync_service_sync_now() != 0U)
    {
        s_esp_sync_state.pending = 0U;
        return;
    }

    /* 同步失败：检查是否还有重试次数 */
    s_esp_sync_state.tries++;

    if (s_esp_sync_state.tries >= s_esp_sync_state.max_tries)
    {
        s_esp_sync_state.pending = 0U;
        return;
    }

    /* 安排下次重试 */
    s_esp_sync_state.next_ms = now_ms + s_esp_sync_state.retry_ms;
}

/* =========================================================================
 *  7. 公共接口实现 —— 电源状态变化处理
 * ======================================================================= */

/**
 * @brief  处理电源状态变化事件
 * @note   当电源状态从灭屏空闲（SCREEN_OFF_IDLE）恢复到其他状态时，
 *         自动调度一次同步任务，确保 ESP32 获取到最新的设置和状态。
 *         同时立即将当前电源状态通知 ESP32。
 * @param  current_state    — 当前电源状态
 * @param  resume_delay_ms  — 恢复后同步延迟（ms）
 * @param  retry_ms         — 重试间隔（ms）
 * @param  max_tries        — 最大重试次数
 */
void esp_sync_service_handle_power_state(power_state_t current_state,
                                         uint32_t resume_delay_ms,
                                         uint32_t retry_ms,
                                         uint8_t max_tries)
{
    if (current_state == s_esp_sync_state.last_power_state)
    {
        return;
    }

    /* 从灭屏空闲恢复时触发同步 */
    if (s_esp_sync_state.last_power_state == POWER_STATE_SCREEN_OFF_IDLE &&
        current_state != POWER_STATE_SCREEN_OFF_IDLE)
    {
        esp_sync_service_schedule(resume_delay_ms, retry_ms, max_tries);
    }

    s_esp_sync_state.last_power_state = current_state;
    (void)esp_host_set_host_state_now(current_state);
}

/* =========================================================================
 *  8. 公共接口实现 —— 状态查询
 * ======================================================================= */

/**
 * @brief  查询是否有待执行的同步任务
 * @retval 1 — 有待执行任务；0 — 空闲
 */
uint8_t esp_sync_service_is_pending(void)
{
    return s_esp_sync_state.pending;
}

/* =========================================================================
 *  9. 内部函数实现 —— 立即同步
 * ======================================================================= */

/**
 * @brief  立即执行一次完整同步
 * @note   按顺序将所有本地设置同步到 ESP32：
 *         1. 获取设备设置（通过回调或直接读取）
 *         2. 刷新 ESP32 状态
 *         3. 设置电源策略
 *         4. 设置主机状态
 *         5. 设置 BLE 开关
 *         6. 设置调试屏幕开关（仅调试模式下）
 *         7. 设置远程按键开关（仅调试模式下）
 *         8. 设置 WiFi 开关（开启时等待 500ms 连接）
 *         9. 设置 MQTT 开关
 * @retval 1 — 全部同步成功；0 — 任一步骤失败
 */
static uint8_t esp_sync_service_sync_now(void)
{
    device_settings_t settings;
    uint8_t debug_screen_enabled = 0U;
    uint8_t remote_keys_enabled  = 0U;

    memset(&settings, 0, sizeof(settings));

    /* 获取设备设置副本 */
    if (s_settings_copy_fn != 0)
    {
        s_settings_copy_fn(&settings);
    }
    else
    {
        settings = *settings_service_get();
    }

    /* 调试模式下才同步调试屏幕和远程按键设置 */
    if (settings.debug_mode_enabled != 0U)
    {
        debug_screen_enabled = settings.esp32_debug_screen_enabled;
        remote_keys_enabled  = settings.esp32_remote_keys_enabled;
    }

    /* 按顺序同步各项设置到 ESP32 */
    if (esp_host_refresh_status() == 0U)
    {
        return 0U;
    }

    if (esp_host_set_power_policy_now(settings.power_policy) == 0U)
    {
        return 0U;
    }

    if (esp_host_set_host_state_now(power_manager_get_state()) == 0U)
    {
        return 0U;
    }

    if (esp_host_set_ble_now(settings.ble_enabled) == 0U)
    {
        return 0U;
    }

    if (esp_host_set_debug_screen_now(debug_screen_enabled) == 0U)
    {
        return 0U;
    }

    if (esp_host_set_remote_keys_now(remote_keys_enabled) == 0U)
    {
        return 0U;
    }

    /* WiFi 开启时等待 500ms 建立连接 */
    if (esp_host_set_wifi_now(settings.wifi_enabled,
                              (settings.wifi_enabled != 0U) ? 500UL : 0UL) == 0U)
    {
        return 0U;
    }

    if (esp_host_set_mqtt_now(settings.mqtt_enabled) == 0U)
    {
        return 0U;
    }

    return 1U;
}
