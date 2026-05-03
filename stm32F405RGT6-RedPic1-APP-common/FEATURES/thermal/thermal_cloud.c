/**
 * @file    thermal_cloud.c
 * @brief   热成像云端上传模块
 * @note    本模块负责将热成像温度摘要数据异步提交到 ESP32 主机，
 *          用于云端上传或远程显示。
 *
 * @par 上传流程
 *      1. 用户按下 KEY2 暂停送显时触发
 *      2. 从性能基线快照中提取 min/max/center 温度
 *      3. 温度值转换为 ×10 定点格式（int16_t）
 *      4. 通过服务总线异步提交到 ESP32
 *
 * @par 节流机制
 *      可通过宏 REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS 配置最小上传间隔。
 *      默认为 0（不节流）。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "thermal_cloud.h"

#include <string.h>

#include "app_perf_baseline.h"
#include "power_manager.h"
#include "redpic1_app.h"

/* =========================================================================
 *  2. 内部宏定义
 * ======================================================================= */

/** 上传节流间隔（ms），0 表示不节流 */
#ifndef REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS
    #define REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS    0UL
#endif

/* =========================================================================
 *  3. 模块级静态变量
 * ======================================================================= */

#if (REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS > 0UL)
static uint32_t s_last_upload_tick_ms = 0U;    /**< 上次上传时间戳（ms）    */
#endif

/* =========================================================================
 *  4. 内部函数实现 —— 温度转换与节流
 * ======================================================================= */

/**
 * @brief  将浮点温度转换为 ×10 定点格式
 * @note   四舍五入，钳位到 int16_t 范围。
 * @param  temp — 温度值（°C）
 * @return ×10 定点温度值
 */
static int16_t redpic1_thermal_cloud_temp_to_x10(float temp)
{
    int32_t scaled = 0;

    scaled = (temp >= 0.0f) ?
             (int32_t)(temp * 10.0f + 0.5f) :
             (int32_t)(temp * 10.0f - 0.5f);

    /* 钳位到 int16_t 范围 */
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

/**
 * @brief  检查上传是否被节流
 * @param  now_ms — 当前系统时间（ms）
 * @retval 1 — 被节流（应跳过）；0 — 可以上传
 */
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

/* =========================================================================
 *  5. 公共接口实现 —— 初始化与重置
 * ======================================================================= */

/**
 * @brief  初始化云端上传模块
 */
void redpic1_thermal_cloud_init(void)
{
    redpic1_thermal_cloud_reset();
}

/**
 * @brief  重置云端上传模块状态
 * @note   清除节流定时器。
 */
void redpic1_thermal_cloud_reset(void)
{
#if (REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS > 0UL)
    s_last_upload_tick_ms = 0U;
#endif
}

/* =========================================================================
 *  6. 公共接口实现 —— 设置查询
 * ======================================================================= */

/**
 * @brief  查询"暂停时发送温度到 ESP"功能是否启用
 * @retval 1 — 已启用；0 — 未启用
 */
uint8_t redpic1_thermal_cloud_pause_send_esp_enabled(void)
{
    device_settings_t settings;
    app_rtos_settings_copy(&settings);
    return settings.thermal_pause_send_esp_enabled;
}

/* =========================================================================
 *  7. 公共接口实现 —— 快照提交
 * ======================================================================= */

/**
 * @brief  将热成像温度摘要异步提交到 ESP32
 * @note   从性能基线快照提取 min/max/center 温度，打包为服务总线命令。
 *         命令格式：
 *         - cmd_id: APP_SERVICE_CMD_SEND_THERMAL_SNAPSHOT
 *         - value:  [31:16] max_temp_x10 | [15:0] min_temp_x10
 *         - arg0:   center_temp_x10 低字节
 *         - arg1:   center_temp_x10 高字节
 * @retval 1 — 提交成功；0 — 提交失败或被节流
 */
uint8_t redpic1_thermal_cloud_submit_snapshot_to_esp(void)
{
    app_perf_baseline_snapshot_t snapshot;
    app_service_cmd_t cmd;
    int16_t min_temp_x10    = 0;
    int16_t max_temp_x10    = 0;
    int16_t center_temp_x10 = 0;
    uint32_t now_ms = power_manager_get_tick_ms();
    uint8_t ok = 0U;

    /* 节流检查 */
    if (redpic1_thermal_cloud_upload_throttled(now_ms) != 0U)
    {
        return 0U;
    }

    /* 获取性能基线快照 */
    app_perf_baseline_get_snapshot(&snapshot);

    /* 无采集数据时跳过 */
    if (snapshot.thermal_capture_frames == 0U)
    {
        return 0U;
    }

    /* 温度转换为 ×10 定点格式 */
    min_temp_x10    = redpic1_thermal_cloud_temp_to_x10(snapshot.latest_min_temp);
    max_temp_x10    = redpic1_thermal_cloud_temp_to_x10(snapshot.latest_max_temp);
    center_temp_x10 = redpic1_thermal_cloud_temp_to_x10(snapshot.latest_center_temp);

    /* 构建服务总线命令 */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_id = APP_SERVICE_CMD_SEND_THERMAL_SNAPSHOT;
    cmd.value  = ((uint32_t)(uint16_t)min_temp_x10) |
                 (((uint32_t)(uint16_t)max_temp_x10) << 16);
    cmd.arg0   = (uint8_t)((uint16_t)center_temp_x10 & 0xFFU);
    cmd.arg1   = (uint8_t)(((uint16_t)center_temp_x10 >> 8) & 0xFFU);

    /* 异步提交 */
    ok = app_service_submit_async(&cmd);
    if (ok != 0U)
    {
#if (REDPIC1_THERMAL_CLOUD_UPLOAD_THROTTLE_MS > 0UL)
        s_last_upload_tick_ms = now_ms;
#endif
    }

    return ok;
}
