//#include "app_shell.h"

//#include <string.h>

//#include "battery_monitor.h"
//#include "esp_host_service_priv.h"
//#include "exti_key.h"
//#include "key.h"
//#include "low_power_runtime.h"
//#include "ota_service.h"
//#include "power_manager.h"
//#include "redpic1_thermal.h"
//#include "settings_service.h"
//#include "ui_manager.h"
//#include "watchdog_service.h"
//#include "uart_rx_ring.h"

//#define APP_SHELL_KEY2_LONG_MS   600UL
//#define APP_SHELL_ESP_BOOT_SYNC_DELAY_MS   1200UL
//#define APP_SHELL_ESP_BOOT_SYNC_RETRY_MS   1000UL
//#define APP_SHELL_ESP_BOOT_SYNC_MAX_TRIES  3U
//#define APP_SHELL_ESP_RESUME_SYNC_DELAY_MS 20UL
//#define APP_SHELL_ESP_RESUME_SYNC_RETRY_MS 150UL
//#define APP_SHELL_ESP_RESUME_SYNC_MAX_TRIES 6U

//typedef struct
//{
//    uint8_t key2_pending;
//    uint32_t key2_press_start_ms;
//    uint8_t esp_boot_sync_pending;
//    uint8_t esp_boot_sync_tries;
//    uint32_t esp_boot_sync_next_ms;
//    uint32_t esp_boot_sync_retry_ms;
//    uint8_t esp_boot_sync_max_tries;
//    power_state_t last_power_state;
//} app_shell_state_t;

//static app_shell_state_t s_app_shell;

//static void app_shell_apply_persisted_settings(void)
//{
//    const device_settings_t *settings = settings_service_get();

//    power_manager_set_policy(settings->power_policy);
//    power_manager_set_screen_off_timeout_ms(settings->screen_off_timeout_ms);
//}

//static uint8_t app_shell_sync_esp_settings_now(void)
//{
//    const device_settings_t *settings = settings_service_get();
//    uint8_t debug_screen_enabled = 0U;
//    uint8_t remote_keys_enabled = 0U;

//    if (settings->debug_mode_enabled != 0U)
//    {
//        debug_screen_enabled = settings->esp32_debug_screen_enabled;
//        remote_keys_enabled = settings->esp32_remote_keys_enabled;
//    }

//    if (esp_host_refresh_status() == 0U)
//    {
//        return 0U;
//    }

//    if (esp_host_set_power_policy_now(settings->power_policy) == 0U)
//    {
//        return 0U;
//    }
//    if (esp_host_set_host_state_now(power_manager_get_state()) == 0U)
//    {
//        return 0U;
//    }
//    if (esp_host_set_ble_now(settings->ble_enabled) == 0U)
//    {
//        return 0U;
//    }
//    if (esp_host_set_debug_screen_now(debug_screen_enabled) == 0U)
//    {
//        return 0U;
//    }
//    if (esp_host_set_remote_keys_now(remote_keys_enabled) == 0U)
//    {
//        return 0U;
//    }
//    if (esp_host_set_wifi_now(settings->wifi_enabled,
//                              (settings->wifi_enabled != 0U) ? 500UL : 0U) == 0U)
//    {
//        return 0U;
//    }

//    return 1U;
//}

//static void app_shell_schedule_esp_sync(uint32_t delay_ms, uint32_t retry_ms, uint8_t max_tries)
//{
//    s_app_shell.esp_boot_sync_pending = 1U;
//    s_app_shell.esp_boot_sync_tries = 0U;
//    s_app_shell.esp_boot_sync_next_ms = power_manager_get_tick_ms() + delay_ms;
//    s_app_shell.esp_boot_sync_retry_ms = retry_ms;
//    s_app_shell.esp_boot_sync_max_tries = max_tries;
//}

//static void app_shell_step_esp_sync(uint32_t now_ms)
//{
//    if (s_app_shell.esp_boot_sync_pending == 0U ||
//        now_ms < s_app_shell.esp_boot_sync_next_ms)
//    {
//        return;
//    }

//    if (app_shell_sync_esp_settings_now() != 0U)
//    {
//        s_app_shell.esp_boot_sync_pending = 0U;
//        return;
//    }

//    s_app_shell.esp_boot_sync_tries++;
//    if (s_app_shell.esp_boot_sync_tries >= s_app_shell.esp_boot_sync_max_tries)
//    {
//        s_app_shell.esp_boot_sync_pending = 0U;
//        return;
//    }

//    s_app_shell.esp_boot_sync_next_ms = now_ms + s_app_shell.esp_boot_sync_retry_ms;
//}

//static void app_shell_wake_if_needed(void)
//{
//    if (power_manager_get_state() == POWER_STATE_SCREEN_OFF_IDLE)
//    {
//        power_manager_step();
//        ui_manager_force_full_refresh();
//    }
//}

//static void app_shell_dispatch_key(uint8_t key_value)
//{
//    app_shell_wake_if_needed();
//    ui_manager_handle_key(key_value);
//}

//static void app_shell_handle_key2_machine(uint32_t now_ms)
//{
//    if (s_app_shell.key2_pending == 0U)
//    {
//        return;
//    }

//    if (KEY_IsLogicalPressed(KEY2_PRES) != 0U)
//    {
//        if ((now_ms - s_app_shell.key2_press_start_ms) >= APP_SHELL_KEY2_LONG_MS)
//        {
//            s_app_shell.key2_pending = 0U;
//            app_shell_dispatch_key(UI_KEY_KEY2_LONG);
//        }
//        return;
//    }

//    s_app_shell.key2_pending = 0U;
//    app_shell_dispatch_key(KEY2_PRES);
//}

//void app_shell_init(void)
//{
//    memset(&s_app_shell, 0, sizeof(s_app_shell));

//    redpic1_thermal_suspend();
//    power_manager_notify_activity();
//    esp_host_init();
//    app_shell_apply_persisted_settings();
//    ui_manager_init();
//    app_shell_schedule_esp_sync(APP_SHELL_ESP_BOOT_SYNC_DELAY_MS,
//                                APP_SHELL_ESP_BOOT_SYNC_RETRY_MS,
//                                APP_SHELL_ESP_BOOT_SYNC_MAX_TRIES);
//    s_app_shell.last_power_state = power_manager_get_state();
//}

//void app_shell_step(void)
//{
//    uint8_t key_value = KEY_GetValue();
//    uint32_t now_ms = power_manager_get_tick_ms();
//    uint32_t uart_error_flags = 0U;

//    watchdog_service_begin_cycle();
//    watchdog_service_mark_progress(WATCHDOG_PROGRESS_MAIN_LOOP);

//    if (key_value != 0U)
//    {
//        power_manager_notify_activity();
//        app_shell_wake_if_needed();

//        if (key_value == KEY2_PRES)
//        {
//            s_app_shell.key2_pending = 1U;
//            s_app_shell.key2_press_start_ms = now_ms;
//        }
//        else
//        {
//            app_shell_dispatch_key(key_value);
//        }
//    }

//    app_shell_handle_key2_machine(now_ms);
//    watchdog_service_report_key_health(KEY_EXTI_IsHealthy());
//    app_shell_step_esp_sync(now_ms);

//    ota_service_poll();
//    watchdog_service_mark_progress(WATCHDOG_PROGRESS_OTA);
//    esp_host_step();
//    watchdog_service_mark_progress(WATCHDOG_PROGRESS_ESP_HOST);
//    battery_monitor_step();
//    watchdog_service_mark_progress(WATCHDOG_PROGRESS_BATTERY);
//    ui_manager_step();
//    watchdog_service_mark_progress(WATCHDOG_PROGRESS_UI);
//    redpic1_thermal_step();
//    power_manager_step();
//    watchdog_service_mark_progress(WATCHDOG_PROGRESS_POWER);
//    uart_error_flags = uart_rx_ring_take_error_flags();
//    watchdog_service_report_uart_errors(uart_error_flags);

//    if (power_manager_get_state() != s_app_shell.last_power_state)
//    {
//        power_state_t previous_state = s_app_shell.last_power_state;

//        s_app_shell.last_power_state = power_manager_get_state();
//        if (previous_state == POWER_STATE_SCREEN_OFF_IDLE &&
//            s_app_shell.last_power_state != POWER_STATE_SCREEN_OFF_IDLE)
//        {
//            ui_manager_force_full_refresh();
//            app_shell_schedule_esp_sync(APP_SHELL_ESP_RESUME_SYNC_DELAY_MS,
//                                        APP_SHELL_ESP_RESUME_SYNC_RETRY_MS,
//                                        APP_SHELL_ESP_RESUME_SYNC_MAX_TRIES);
//        }
//        (void)esp_host_set_host_state_now(s_app_shell.last_power_state);
//    }

//    watchdog_service_step();
//    low_power_runtime_step();
//}
