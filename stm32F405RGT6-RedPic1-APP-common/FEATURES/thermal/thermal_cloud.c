#include "thermal_cloud.h"

#include <string.h>

#include "app_perf_baseline.h"
#include "power_manager.h"
#include "redpic1_app.h"

#ifndef REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS
    #define REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS 0UL
#endif

#if (REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS > 0UL)
static uint32_t s_last_upload_tick_ms = 0U;
#endif

static int16_t redpic1_thermal_cloud_temp_to_x10(float temp)
{
    int32_t scaled = 0;

    scaled = (temp >= 0.0f) ?
             (int32_t)(temp * 10.0f + 0.5f) :
             (int32_t)(temp * 10.0f - 0.5f);

    if (scaled > 32767)
    {
        scaled = 32767;
    }
    else if (scaled < -32768)
    {
        scaled = -32768;
    }

    return (int16_t)scaled;
}

static uint8_t redpic1_thermal_cloud_upload_throttled(uint32_t now_ms)
{
#if (REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS > 0UL)
    if (s_last_upload_tick_ms != 0U &&
        (uint32_t)(now_ms - s_last_upload_tick_ms) < REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS)
    {
        return 1U;
    }

    return 0U;
#else
    (void)now_ms;
    return 0U;
#endif
}

void redpic1_thermal_cloud_init(void)
{
    redpic1_thermal_cloud_reset();
}

void redpic1_thermal_cloud_reset(void)
{
#if (REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS > 0UL)
    s_last_upload_tick_ms = 0U;
#endif
}

uint8_t redpic1_thermal_cloud_pause_send_esp_enabled(void)
{
#if (REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ENABLE != 0U)
    device_settings_t settings;

    app_rtos_settings_copy(&settings);
    return settings.thermal_pause_send_esp_enabled;
#else
    return 0U;
#endif
}

uint8_t redpic1_thermal_cloud_submit_snapshot_to_esp(void)
{
    app_perf_baseline_snapshot_t snapshot;
    app_service_cmd_t cmd;
    int16_t min_temp_x10 = 0;
    int16_t max_temp_x10 = 0;
    int16_t center_temp_x10 = 0;
    uint32_t now_ms = power_manager_get_tick_ms();
    uint8_t ok = 0U;

    if (redpic1_thermal_cloud_upload_throttled(now_ms) != 0U)
    {
        return 0U;
    }

    app_perf_baseline_get_snapshot(&snapshot);
    if (snapshot.thermal_capture_frames == 0U)
    {
        return 0U;
    }

    min_temp_x10 = redpic1_thermal_cloud_temp_to_x10(snapshot.latest_min_temp);
    max_temp_x10 = redpic1_thermal_cloud_temp_to_x10(snapshot.latest_max_temp);
    center_temp_x10 = redpic1_thermal_cloud_temp_to_x10(snapshot.latest_center_temp);

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SEND_THERMAL_SNAPSHOT;
    cmd.value = ((uint32_t)(uint16_t)min_temp_x10) |
                (((uint32_t)(uint16_t)max_temp_x10) << 16);
    cmd.arg0 = (uint8_t)((uint16_t)center_temp_x10 & 0xFFU);
    cmd.arg1 = (uint8_t)(((uint16_t)center_temp_x10 >> 8) & 0xFFU);

    ok = app_service_submit_async(&cmd);
    if (ok != 0U)
    {
#if (REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS > 0UL)
        s_last_upload_tick_ms = now_ms;
#endif
    }

    return ok;
}
