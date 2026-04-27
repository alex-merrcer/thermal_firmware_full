#ifndef ESP_SYNC_SERVICE_H
#define ESP_SYNC_SERVICE_H

#include <stdint.h>

#include "power_manager.h"
#include "settings_service.h"

typedef void (*esp_sync_service_settings_copy_fn_t)(device_settings_t *out_settings);

void esp_sync_service_register_settings_copy(esp_sync_service_settings_copy_fn_t settings_copy_fn);
void esp_sync_service_reset(power_state_t initial_power_state);
void esp_sync_service_schedule(uint32_t delay_ms, uint32_t retry_ms, uint8_t max_tries);
void esp_sync_service_step(uint32_t now_ms);
void esp_sync_service_handle_power_state(power_state_t current_state,
                                         uint32_t resume_delay_ms,
                                         uint32_t retry_ms,
                                         uint8_t max_tries);
uint8_t esp_sync_service_is_pending(void);

#endif
