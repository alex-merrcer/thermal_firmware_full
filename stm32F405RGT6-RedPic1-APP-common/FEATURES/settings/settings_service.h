#ifndef SETTINGS_SERVICE_H
#define SETTINGS_SERVICE_H

#include <stdint.h>

#include "clock_profile_service.h"
#include "power_manager.h"

#define DEVICE_SETTINGS_FLAG_WIFI_ENABLED              (1UL << 0)
#define DEVICE_SETTINGS_FLAG_BLE_ENABLED               (1UL << 1)
#define DEVICE_SETTINGS_FLAG_DEBUG_MODE_ENABLED        (1UL << 2)
#define DEVICE_SETTINGS_FLAG_ESP32_DEBUG_SCREEN        (1UL << 3)
#define DEVICE_SETTINGS_FLAG_ESP32_REMOTE_KEYS         (1UL << 4)
#define DEVICE_SETTINGS_FLAG_LOW_POWER_ENABLED         (1UL << 5)
#define DEVICE_SETTINGS_FLAG_STANDBY_ENABLED           (1UL << 6)
#define DEVICE_SETTINGS_FLAG_THERMAL_PAUSE_SEND_ESP    (1UL << 7)

#define DEVICE_SETTINGS_BLOB_MAGIC                     0x44535447UL
#define DEVICE_SETTINGS_BLOB_VERSION_V1                1U
#define DEVICE_SETTINGS_BLOB_VERSION_V2                2U
#define DEVICE_SETTINGS_BLOB_VERSION                   3U
#define DEVICE_SETTINGS_DEFAULT_SCREEN_OFF_TIMEOUT_MS  180000UL
#define DEVICE_SETTINGS_DEFAULT_POWER_POLICY           POWER_POLICY_BALANCED
#define DEVICE_SETTINGS_DEFAULT_RTC_STOP_WAKE_MS       5000UL
#define DEVICE_SETTINGS_DEFAULT_STANDBY_ENABLED        1U
#define DEVICE_SETTINGS_DEFAULT_CLOCK_POLICY           CLOCK_PROFILE_POLICY_AUTO

typedef struct
{
    uint8_t wifi_enabled;
    uint8_t ble_enabled;
    uint8_t debug_mode_enabled;
    uint8_t esp32_debug_screen_enabled;
    uint8_t esp32_remote_keys_enabled;
    uint8_t low_power_enabled;
    uint8_t standby_enabled;
    uint8_t thermal_pause_send_esp_enabled;
    power_policy_t power_policy;
    uint32_t screen_off_timeout_ms;
    uint32_t rtc_stop_wake_ms;
    clock_profile_policy_t clock_profile_policy;
} device_settings_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t flags;
    uint32_t screen_off_timeout_ms;
    uint32_t power_policy;
    uint32_t rtc_stop_wake_ms;
    uint32_t clock_profile_policy;
    uint32_t crc32;
} device_settings_blob_t;

void settings_service_init(void);
const device_settings_t *settings_service_get(void);
uint8_t settings_service_update(const device_settings_t *settings);
uint8_t settings_service_save(void);

#endif
