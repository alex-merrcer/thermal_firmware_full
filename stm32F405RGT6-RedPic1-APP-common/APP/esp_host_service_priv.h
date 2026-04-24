#ifndef ESP_HOST_SERVICE_PRIV_H
#define ESP_HOST_SERVICE_PRIV_H

#include <stdint.h>

#include "esp_host_service.h"
#include "power_manager.h"

void esp_host_init(void);
void esp_host_step(void);
uint8_t esp_host_refresh_status(void);
uint8_t esp_host_set_wifi_now(uint8_t enabled, uint32_t wait_connected_ms);
uint8_t esp_host_set_ble_now(uint8_t enabled);
uint8_t esp_host_set_debug_screen_now(uint8_t enabled);
uint8_t esp_host_set_remote_keys_now(uint8_t enabled);
uint8_t esp_host_set_power_policy_now(power_policy_t policy);
uint8_t esp_host_set_host_state_now(power_state_t state);
uint8_t esp_host_prepare_for_stop(uint32_t timeout_ms);
uint8_t esp_host_prepare_for_standby(uint32_t timeout_ms);
uint8_t esp_host_enter_forced_deep_sleep_now(uint32_t timeout_ms);
void esp_host_set_wifi(uint8_t enabled);
void esp_host_set_ble(uint8_t enabled);
void esp_host_set_debug_screen(uint8_t enabled);
void esp_host_set_remote_keys(uint8_t enabled);

#endif
