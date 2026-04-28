#include "settings_service.h"

#include <string.h>

#include "ota_service.h"

#define SETTINGS_TIMEOUT_MIN_MS                 5000UL
#define SETTINGS_TIMEOUT_MAX_MS                 600000UL
#define SETTINGS_TIMEOUT_LEGACY_DEFAULT_MS      15000UL
#define SETTINGS_RTC_WAKE_MIN_MS                500UL
#define SETTINGS_RTC_WAKE_MAX_MS                5000UL

static device_settings_t s_settings;
static clock_profile_policy_t settings_clock_policy_from_power_policy(power_policy_t power_policy)
{
    switch (power_policy)
    {
    case POWER_POLICY_PERFORMANCE:
        return CLOCK_PROFILE_POLICY_HIGH_ONLY;
    case POWER_POLICY_ECO:
        return CLOCK_PROFILE_POLICY_MEDIUM_ONLY;
    case POWER_POLICY_BALANCED:
    default:
        return CLOCK_PROFILE_POLICY_AUTO;
    }
}

static uint32_t settings_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    uint32_t i = 0U;
    uint32_t j = 0U;

    crc = ~crc;
    for (i = 0U; i < length; ++i)
    {
        crc ^= (uint32_t)data[i];
        for (j = 0U; j < 8U; ++j)
        {
            if ((crc & 1UL) != 0UL)
            {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return ~crc;
}

static uint32_t settings_blob_compute_crc(const device_settings_blob_t *blob)
{
    if (blob == 0)
    {
        return 0U;
    }

    return settings_crc32_update(0U,
                                 (const uint8_t *)blob,
                                 (uint32_t)(sizeof(device_settings_blob_t) - sizeof(blob->crc32)));
}

static void settings_service_load_defaults(device_settings_t *settings)
{
    if (settings == 0)
    {
        return;
    }

    memset(settings, 0, sizeof(*settings));
    settings->wifi_enabled = 0U;
    settings->ble_enabled = 0U;
    settings->mqtt_enabled = 0U;
    settings->debug_mode_enabled = 0U;
    settings->esp32_debug_screen_enabled = 0U;
    settings->esp32_remote_keys_enabled = 0U;
    settings->low_power_enabled = 1U;
    settings->standby_enabled = DEVICE_SETTINGS_DEFAULT_STANDBY_ENABLED;
    settings->thermal_pause_send_esp_enabled = 1U;
    settings->power_policy = DEVICE_SETTINGS_DEFAULT_POWER_POLICY;
    settings->screen_off_timeout_ms = DEVICE_SETTINGS_DEFAULT_SCREEN_OFF_TIMEOUT_MS;
    settings->rtc_stop_wake_ms = DEVICE_SETTINGS_DEFAULT_RTC_STOP_WAKE_MS;
    settings->clock_profile_policy = settings_clock_policy_from_power_policy(settings->power_policy);
}

static void settings_service_sanitize(device_settings_t *settings)
{
    if (settings == 0)
    {
        return;
    }

    settings->wifi_enabled = (settings->wifi_enabled != 0U) ? 1U : 0U;
    settings->ble_enabled = (settings->ble_enabled != 0U) ? 1U : 0U;
    settings->mqtt_enabled = (settings->mqtt_enabled != 0U) ? 1U : 0U;
    settings->debug_mode_enabled = (settings->debug_mode_enabled != 0U) ? 1U : 0U;
    settings->esp32_debug_screen_enabled = (settings->esp32_debug_screen_enabled != 0U) ? 1U : 0U;
    settings->esp32_remote_keys_enabled = (settings->esp32_remote_keys_enabled != 0U) ? 1U : 0U;
    settings->standby_enabled = (settings->standby_enabled != 0U) ? 1U : 0U;
    settings->thermal_pause_send_esp_enabled = (settings->thermal_pause_send_esp_enabled != 0U) ? 1U : 0U;

    if ((uint32_t)settings->power_policy >= (uint32_t)POWER_POLICY_COUNT)
    {
        settings->power_policy = DEVICE_SETTINGS_DEFAULT_POWER_POLICY;
    }

    settings->low_power_enabled = (settings->power_policy != POWER_POLICY_PERFORMANCE) ? 1U : 0U;
    settings->clock_profile_policy = settings_clock_policy_from_power_policy(settings->power_policy);

    if (settings->screen_off_timeout_ms < SETTINGS_TIMEOUT_MIN_MS ||
        settings->screen_off_timeout_ms > SETTINGS_TIMEOUT_MAX_MS)
    {
        settings->screen_off_timeout_ms = DEVICE_SETTINGS_DEFAULT_SCREEN_OFF_TIMEOUT_MS;
    }

    if (settings->rtc_stop_wake_ms < SETTINGS_RTC_WAKE_MIN_MS ||
        settings->rtc_stop_wake_ms > SETTINGS_RTC_WAKE_MAX_MS)
    {
        settings->rtc_stop_wake_ms = DEVICE_SETTINGS_DEFAULT_RTC_STOP_WAKE_MS;
    }


}

static void settings_blob_from_settings(device_settings_blob_t *blob,
                                        const device_settings_t *settings)
{
    uint32_t flags = 0U;

    if (blob == 0 || settings == 0)
    {
        return;
    }

    memset(blob, 0, sizeof(*blob));
    if (settings->wifi_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_WIFI_ENABLED;
    }
    if (settings->ble_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_BLE_ENABLED;
    }
    if (settings->debug_mode_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_DEBUG_MODE_ENABLED;
    }
    if (settings->esp32_debug_screen_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_ESP32_DEBUG_SCREEN;
    }
    if (settings->esp32_remote_keys_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_ESP32_REMOTE_KEYS;
    }
    if (settings->low_power_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_LOW_POWER_ENABLED;
    }
    if (settings->standby_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_STANDBY_ENABLED;
    }
    if (settings->thermal_pause_send_esp_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_THERMAL_PAUSE_SEND_ESP;
    }
    if (settings->mqtt_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_MQTT_ENABLED;
    }

    blob->magic = DEVICE_SETTINGS_BLOB_MAGIC;
    blob->version = DEVICE_SETTINGS_BLOB_VERSION;
    blob->size = (uint16_t)sizeof(*blob);
    blob->flags = flags;
    blob->screen_off_timeout_ms = settings->screen_off_timeout_ms;
    blob->power_policy = (uint32_t)settings->power_policy;
    blob->rtc_stop_wake_ms = settings->rtc_stop_wake_ms;
    blob->clock_profile_policy = (uint32_t)settings->clock_profile_policy;
    blob->crc32 = settings_blob_compute_crc(blob);
}

static uint8_t settings_blob_is_valid(const device_settings_blob_t *blob)
{
    if (blob == 0)
    {
        return 0U;
    }

    if (blob->magic != DEVICE_SETTINGS_BLOB_MAGIC ||
        (blob->version != DEVICE_SETTINGS_BLOB_VERSION &&
         blob->version != DEVICE_SETTINGS_BLOB_VERSION_V4 &&
         blob->version != DEVICE_SETTINGS_BLOB_VERSION_V3 &&
         blob->version != DEVICE_SETTINGS_BLOB_VERSION_V2 &&
         blob->version != DEVICE_SETTINGS_BLOB_VERSION_V1) ||
        blob->size != sizeof(device_settings_blob_t))
    {
        return 0U;
    }

    return (blob->crc32 == settings_blob_compute_crc(blob)) ? 1U : 0U;
}

static void settings_from_blob(device_settings_t *settings, const device_settings_blob_t *blob)
{
    if (settings == 0 || blob == 0)
    {
        return;
    }

    settings->wifi_enabled = ((blob->flags & DEVICE_SETTINGS_FLAG_WIFI_ENABLED) != 0U) ? 1U : 0U;
    settings->ble_enabled = ((blob->flags & DEVICE_SETTINGS_FLAG_BLE_ENABLED) != 0U) ? 1U : 0U;
    settings->mqtt_enabled = ((blob->flags & DEVICE_SETTINGS_FLAG_MQTT_ENABLED) != 0U) ? 1U : 0U;
    settings->debug_mode_enabled = ((blob->flags & DEVICE_SETTINGS_FLAG_DEBUG_MODE_ENABLED) != 0U) ? 1U : 0U;
    settings->esp32_debug_screen_enabled = ((blob->flags & DEVICE_SETTINGS_FLAG_ESP32_DEBUG_SCREEN) != 0U) ? 1U : 0U;
    settings->esp32_remote_keys_enabled = ((blob->flags & DEVICE_SETTINGS_FLAG_ESP32_REMOTE_KEYS) != 0U) ? 1U : 0U;
    settings->thermal_pause_send_esp_enabled = ((blob->flags & DEVICE_SETTINGS_FLAG_THERMAL_PAUSE_SEND_ESP) != 0U) ? 1U : 0U;
    settings->standby_enabled = DEVICE_SETTINGS_DEFAULT_STANDBY_ENABLED;
    settings->screen_off_timeout_ms = blob->screen_off_timeout_ms;

    if (blob->version >= DEVICE_SETTINGS_BLOB_VERSION_V2)
    {
        settings->power_policy = (power_policy_t)blob->power_policy;
    }
    else
    {
        settings->power_policy =
            ((blob->flags & DEVICE_SETTINGS_FLAG_LOW_POWER_ENABLED) != 0U) ?
            POWER_POLICY_BALANCED :
            POWER_POLICY_PERFORMANCE;
    }

    if (blob->version >= DEVICE_SETTINGS_BLOB_VERSION_V4)
    {
        settings->standby_enabled = ((blob->flags & DEVICE_SETTINGS_FLAG_STANDBY_ENABLED) != 0U) ? 1U : 0U;
        settings->rtc_stop_wake_ms = blob->rtc_stop_wake_ms;
        settings->clock_profile_policy = (clock_profile_policy_t)blob->clock_profile_policy;
    }
    else
    {
        settings->rtc_stop_wake_ms = DEVICE_SETTINGS_DEFAULT_RTC_STOP_WAKE_MS;
        settings->clock_profile_policy = DEVICE_SETTINGS_DEFAULT_CLOCK_POLICY;
    }

    settings_service_sanitize(settings);
}

void settings_service_init(void)
{
    BootInfoTypeDef boot_info;
    device_settings_blob_t blob;
    uint8_t should_resave = 0U;

    settings_service_load_defaults(&s_settings);
    ota_service_load_boot_info_copy(&boot_info);
    memcpy(&blob, boot_info.reserved, sizeof(blob));

    if (settings_blob_is_valid(&blob) != 0U)
    {
        settings_from_blob(&s_settings, &blob);
        if (blob.version < DEVICE_SETTINGS_BLOB_VERSION_V4)
        {
            /* V4 aligns the shipped product behavior: KEY2 pause-send is enabled by default. */
            s_settings.thermal_pause_send_esp_enabled = 1U;
            should_resave = 1U;
        }
        if (blob.version < DEVICE_SETTINGS_BLOB_VERSION_V4 &&
            s_settings.screen_off_timeout_ms == SETTINGS_TIMEOUT_LEGACY_DEFAULT_MS)
        {
            s_settings.screen_off_timeout_ms = DEVICE_SETTINGS_DEFAULT_SCREEN_OFF_TIMEOUT_MS;
            should_resave = 1U;
        }
        if (blob.version < DEVICE_SETTINGS_BLOB_VERSION)
        {
            /* V5 adds user-controlled cloud connectivity; keep it disabled by default. */
            s_settings.mqtt_enabled = 0U;
            should_resave = 1U;
        }
        if (blob.version != DEVICE_SETTINGS_BLOB_VERSION)
        {
            should_resave = 1U;
        }
        if (should_resave != 0U)
        {
            (void)settings_service_save();
        }
        return;
    }

    (void)settings_service_save();
}

const device_settings_t *settings_service_get(void)
{
    return &s_settings;
}

uint8_t settings_service_save(void)
{
    BootInfoTypeDef boot_info;
    device_settings_blob_t blob;

    settings_service_sanitize(&s_settings);
    ota_service_load_boot_info_copy(&boot_info);
    settings_blob_from_settings(&blob, &s_settings);
    memcpy(boot_info.reserved, &blob, sizeof(blob));

    return (ota_service_store_boot_info(&boot_info) == 0U) ? 1U : 0U;
}

uint8_t settings_service_update(const device_settings_t *settings)
{
    device_settings_t previous_settings;

    if (settings == 0)
    {
        return 0U;
    }

    previous_settings = s_settings;
    s_settings = *settings;
    settings_service_sanitize(&s_settings);
    if (settings_service_save() != 0U)
    {
        return 1U;
    }

    s_settings = previous_settings;
    return 0U;
}
