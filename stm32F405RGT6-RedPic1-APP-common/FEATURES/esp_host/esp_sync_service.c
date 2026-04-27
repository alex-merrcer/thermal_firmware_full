#include "esp_sync_service.h"

#include <string.h>

#include "esp_host_service_priv.h"
#include "power_manager.h"
#include "settings_service.h"

typedef struct
{
    uint8_t pending;
    uint8_t tries;
    uint32_t next_ms;
    uint32_t retry_ms;
    uint8_t max_tries;
    power_state_t last_power_state;
} esp_sync_state_t;

static esp_sync_state_t s_esp_sync_state;
static esp_sync_service_settings_copy_fn_t s_settings_copy_fn = 0;

static uint8_t esp_sync_service_sync_now(void);

void esp_sync_service_register_settings_copy(esp_sync_service_settings_copy_fn_t settings_copy_fn)
{
    s_settings_copy_fn = settings_copy_fn;
}

void esp_sync_service_reset(power_state_t initial_power_state)
{
    memset(&s_esp_sync_state, 0, sizeof(s_esp_sync_state));
    s_esp_sync_state.last_power_state = initial_power_state;
}

void esp_sync_service_schedule(uint32_t delay_ms, uint32_t retry_ms, uint8_t max_tries)
{
    s_esp_sync_state.pending = 1U;
    s_esp_sync_state.tries = 0U;
    s_esp_sync_state.next_ms = power_manager_get_tick_ms() + delay_ms;
    s_esp_sync_state.retry_ms = retry_ms;
    s_esp_sync_state.max_tries = max_tries;
}

void esp_sync_service_step(uint32_t now_ms)
{
    if (s_esp_sync_state.pending == 0U ||
        now_ms < s_esp_sync_state.next_ms)
    {
        return;
    }

    if (esp_sync_service_sync_now() != 0U)
    {
        s_esp_sync_state.pending = 0U;
        return;
    }

    s_esp_sync_state.tries++;
    if (s_esp_sync_state.tries >= s_esp_sync_state.max_tries)
    {
        s_esp_sync_state.pending = 0U;
        return;
    }

    s_esp_sync_state.next_ms = now_ms + s_esp_sync_state.retry_ms;
}

void esp_sync_service_handle_power_state(power_state_t current_state,
                                         uint32_t resume_delay_ms,
                                         uint32_t retry_ms,
                                         uint8_t max_tries)
{
    if (current_state == s_esp_sync_state.last_power_state)
    {
        return;
    }

    if (s_esp_sync_state.last_power_state == POWER_STATE_SCREEN_OFF_IDLE &&
        current_state != POWER_STATE_SCREEN_OFF_IDLE)
    {
        esp_sync_service_schedule(resume_delay_ms, retry_ms, max_tries);
    }

    s_esp_sync_state.last_power_state = current_state;
    (void)esp_host_set_host_state_now(current_state);
}

uint8_t esp_sync_service_is_pending(void)
{
    return s_esp_sync_state.pending;
}

static uint8_t esp_sync_service_sync_now(void)
{
    device_settings_t settings;
    uint8_t debug_screen_enabled = 0U;
    uint8_t remote_keys_enabled = 0U;

    memset(&settings, 0, sizeof(settings));
    if (s_settings_copy_fn != 0)
    {
        s_settings_copy_fn(&settings);
    }
    else
    {
        settings = *settings_service_get();
    }

    if (settings.debug_mode_enabled != 0U)
    {
        debug_screen_enabled = settings.esp32_debug_screen_enabled;
        remote_keys_enabled = settings.esp32_remote_keys_enabled;
    }

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
    if (esp_host_set_wifi_now(settings.wifi_enabled,
                              (settings.wifi_enabled != 0U) ? 500UL : 0UL) == 0U)
    {
        return 0U;
    }

    return 1U;
}
