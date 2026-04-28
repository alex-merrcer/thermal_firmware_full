#ifndef ESP_HOST_SERVICE_H
#define ESP_HOST_SERVICE_H

#include <stdint.h>

typedef struct
{
    uint8_t online;
    uint8_t wifi_enabled;
    uint8_t wifi_connected;
    uint8_t ble_enabled;
    uint8_t ble_connected;
    uint8_t mqtt_enabled;
    uint8_t debug_screen_enabled;
    uint8_t remote_keys_enabled;
    uint8_t has_credentials;
    uint8_t ready_for_sleep;
    uint32_t last_seen_ms;
} esp_host_status_t;

const esp_host_status_t *esp_host_get_status(void);
void esp_host_get_status_copy(esp_host_status_t *out_status);
uint8_t esp_host_is_forced_deep_sleep(void);

#endif
