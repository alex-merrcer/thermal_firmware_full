/**
 * @file    settings_service.c
 * @brief   设备设置服务模块
 * @note    本模块负责设备设置的持久化存储与管理。
 *          设置以二进制 Blob 形式嵌入 BootInfo 的 reserved 字段中，
 *          通过 CRC32 校验保证数据完整性。
 *
 * @par 存储格式
 *      设置以 device_settings_blob_t 结构体序列化后写入 Flash。
 *      Blob 包含 magic、版本号、大小、标志位和 CRC32 校验码。
 *      支持多版本向后兼容（V1 ~ V6）。
 *
 * @par 版本迁移
 *      - V1 → V2: power_policy 从 low_power_enabled 标志位迁移为独立字段
 *      - V2 → V4: 新增 standby_enabled、rtc_stop_wake_ms、clock_profile_policy
 *      - V4 → V5: BLE/MQTT 改为用户可控，默认关闭
 *      - V5 → V6: 新增 KEY2 快照功能，默认关闭
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "settings_service.h"

#include <string.h>

#include "ota_service.h"

/* =========================================================================
 *  2. 内部宏定义
 * ======================================================================= */

#define SETTINGS_TIMEOUT_MIN_MS                 5000UL      /**< 灭屏超时最小值（ms）      */
#define SETTINGS_TIMEOUT_MAX_MS                 600000UL    /**< 灭屏超时最大值（ms）      */
#define SETTINGS_TIMEOUT_LEGACY_DEFAULT_MS      15000UL     /**< 旧版灭屏超时默认值（ms）  */
#define SETTINGS_RTC_WAKE_MIN_MS                500UL       /**< RTC 唤醒间隔最小值（ms）  */
#define SETTINGS_RTC_WAKE_MAX_MS                5000UL      /**< RTC 唤醒间隔最大值（ms）  */

/* =========================================================================
 *  3. 模块级静态变量
 * ======================================================================= */

static device_settings_t s_settings;    /**< 当前设备设置副本 */

/* =========================================================================
 *  4. 内部函数实现 —— 时钟策略映射
 * ======================================================================= */

/**
 * @brief  根据电源策略推导时钟配置策略
 * @param  power_policy — 电源策略
 * @return 对应的时钟配置策略
 */
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

/* =========================================================================
 *  5. 内部函数实现 —— CRC32 校验
 * ======================================================================= */

/**
 * @brief  计算 CRC32 校验值（逐字节更新）
 * @note   采用 CRC-32/MPEG-2 算法，多项式 0xEDB88320。
 * @param  crc    — 初始 CRC 值（通常传 0）
 * @param  data   — 输入数据指针
 * @param  length — 数据长度（字节）
 * @return 计算得到的 CRC32 值
 */
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

/**
 * @brief  计算设置 Blob 的 CRC32 校验值
 * @note   校验范围为整个 Blob 结构体（不含 crc32 字段本身）。
 * @param  blob — 设置 Blob 指针
 * @return 计算得到的 CRC32 值；指针为空时返回 0
 */
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

/* =========================================================================
 *  6. 内部函数实现 —— 默认值加载
 * ======================================================================= */

/**
 * @brief  加载出厂默认设置
 * @note   所有开关默认关闭，电源策略为平衡模式。
 * @param  settings — 输出：设置结构体指针
 */
static void settings_service_load_defaults(device_settings_t *settings)
{
    if (settings == 0)
    {
        return;
    }

    memset(settings, 0, sizeof(*settings));

    /* 网络功能默认关闭 */
    settings->wifi_enabled                          = 0U;
    settings->ble_enabled                           = 0U;
    settings->mqtt_enabled                          = 0U;

    /* 调试功能默认关闭 */
    settings->debug_mode_enabled                    = 0U;
    settings->esp32_debug_screen_enabled            = 0U;
    settings->esp32_remote_keys_enabled             = 0U;

    /* 电源管理默认值 */
    settings->low_power_enabled                     = 1U;
    settings->standby_enabled                       = DEVICE_SETTINGS_DEFAULT_STANDBY_ENABLED;
    settings->thermal_pause_send_esp_enabled        = 1U;

    /* 快照功能默认关闭 */
    settings->key2_snapshot_enabled                 = 0U;

    /* 电源策略与超时参数 */
    settings->power_policy                          = DEVICE_SETTINGS_DEFAULT_POWER_POLICY;
    settings->screen_off_timeout_ms                 = DEVICE_SETTINGS_DEFAULT_SCREEN_OFF_TIMEOUT_MS;
    settings->rtc_stop_wake_ms                      = DEVICE_SETTINGS_DEFAULT_RTC_STOP_WAKE_MS;
    settings->clock_profile_policy                  =
        settings_clock_policy_from_power_policy(settings->power_policy);
}

/* =========================================================================
 *  7. 内部函数实现 —— 设置数据清洗
 * ======================================================================= */

/**
 * @brief  对设置数据进行合法性清洗
 * @note   将布尔字段归一化为 0/1，校验枚举范围和数值区间。
 * @param  settings — 待清洗的设置结构体指针
 */
static void settings_service_sanitize(device_settings_t *settings)
{
    if (settings == 0)
    {
        return;
    }

    /* 布尔字段归一化：非零值统一为 1 */
    settings->wifi_enabled                      = (settings->wifi_enabled != 0U) ? 1U : 0U;
    settings->ble_enabled                       = (settings->ble_enabled != 0U) ? 1U : 0U;
    settings->mqtt_enabled                      = (settings->mqtt_enabled != 0U) ? 1U : 0U;
    settings->debug_mode_enabled                = (settings->debug_mode_enabled != 0U) ? 1U : 0U;
    settings->esp32_debug_screen_enabled        = (settings->esp32_debug_screen_enabled != 0U) ? 1U : 0U;
    settings->esp32_remote_keys_enabled         = (settings->esp32_remote_keys_enabled != 0U) ? 1U : 0U;
    settings->standby_enabled                   = (settings->standby_enabled != 0U) ? 1U : 0U;
    settings->thermal_pause_send_esp_enabled    = (settings->thermal_pause_send_esp_enabled != 0U) ? 1U : 0U;
    settings->key2_snapshot_enabled             = (settings->key2_snapshot_enabled != 0U) ? 1U : 0U;

    /* 电源策略范围校验 */
    if ((uint32_t)settings->power_policy >= (uint32_t)POWER_POLICY_COUNT)
    {
        settings->power_policy = DEVICE_SETTINGS_DEFAULT_POWER_POLICY;
    }

    /* low_power_enabled 由 power_policy 自动推导 */
    settings->low_power_enabled =
        (settings->power_policy != POWER_POLICY_PERFORMANCE) ? 1U : 0U;

    /* 时钟策略由电源策略自动推导 */
    settings->clock_profile_policy =
        settings_clock_policy_from_power_policy(settings->power_policy);

    /* 灭屏超时范围校验 */
    if (settings->screen_off_timeout_ms < SETTINGS_TIMEOUT_MIN_MS ||
        settings->screen_off_timeout_ms > SETTINGS_TIMEOUT_MAX_MS)
    {
        settings->screen_off_timeout_ms = DEVICE_SETTINGS_DEFAULT_SCREEN_OFF_TIMEOUT_MS;
    }

    /* RTC 唤醒间隔范围校验 */
    if (settings->rtc_stop_wake_ms < SETTINGS_RTC_WAKE_MIN_MS ||
        settings->rtc_stop_wake_ms > SETTINGS_RTC_WAKE_MAX_MS)
    {
        settings->rtc_stop_wake_ms = DEVICE_SETTINGS_DEFAULT_RTC_STOP_WAKE_MS;
    }
}

/* =========================================================================
 *  8. 内部函数实现 —— Blob 序列化与反序列化
 * ======================================================================= */

/**
 * @brief  将设置结构体序列化为 Blob 格式
 * @note   各布尔字段映射为 flags 位域，数值字段直接赋值，
 *         最后计算 CRC32 校验码。
 * @param  blob     — 输出：Blob 结构体指针
 * @param  settings — 输入：设置结构体指针
 */
static void settings_blob_from_settings(device_settings_blob_t *blob,
                                        const device_settings_t *settings)
{
    uint32_t flags = 0U;

    if (blob == 0 || settings == 0)
    {
        return;
    }

    memset(blob, 0, sizeof(*blob));

    /* 构建标志位 */
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
    if (settings->key2_snapshot_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_KEY2_SNAPSHOT_ENABLED;
    }
    if (settings->mqtt_enabled != 0U)
    {
        flags |= DEVICE_SETTINGS_FLAG_MQTT_ENABLED;
    }

    /* 填充 Blob 头部与数值字段 */
    blob->magic                 = DEVICE_SETTINGS_BLOB_MAGIC;
    blob->version               = DEVICE_SETTINGS_BLOB_VERSION;
    blob->size                  = (uint16_t)sizeof(*blob);
    blob->flags                 = flags;
    blob->screen_off_timeout_ms = settings->screen_off_timeout_ms;
    blob->power_policy          = (uint32_t)settings->power_policy;
    blob->rtc_stop_wake_ms      = settings->rtc_stop_wake_ms;
    blob->clock_profile_policy  = (uint32_t)settings->clock_profile_policy;

    /* 计算并写入校验码 */
    blob->crc32 = settings_blob_compute_crc(blob);
}

/**
 * @brief  校验 Blob 数据的合法性
 * @note   检查 magic、版本号、大小和 CRC32 校验码。
 * @param  blob — 待校验的 Blob 指针
 * @retval 1 — 合法；0 — 非法
 */
static uint8_t settings_blob_is_valid(const device_settings_blob_t *blob)
{
    if (blob == 0)
    {
        return 0U;
    }

    /* magic 校验 */
    if (blob->magic != DEVICE_SETTINGS_BLOB_MAGIC)
    {
        return 0U;
    }

    /* 版本号校验（兼容 V1 ~ V6） */
    if (blob->version != DEVICE_SETTINGS_BLOB_VERSION &&
        blob->version != DEVICE_SETTINGS_BLOB_VERSION_V5 &&
        blob->version != DEVICE_SETTINGS_BLOB_VERSION_V4 &&
        blob->version != DEVICE_SETTINGS_BLOB_VERSION_V3 &&
        blob->version != DEVICE_SETTINGS_BLOB_VERSION_V2 &&
        blob->version != DEVICE_SETTINGS_BLOB_VERSION_V1)
    {
        return 0U;
    }

    /* 大小校验 */
    if (blob->size != sizeof(device_settings_blob_t))
    {
        return 0U;
    }

    /* CRC32 校验 */
    return (blob->crc32 == settings_blob_compute_crc(blob)) ? 1U : 0U;
}

/**
 * @brief  将 Blob 反序列化为设置结构体
 * @note   处理不同版本的字段差异，最后执行数据清洗。
 * @param  settings — 输出：设置结构体指针
 * @param  blob     — 输入：Blob 结构体指针
 */
static void settings_from_blob(device_settings_t *settings, const device_settings_blob_t *blob)
{
    if (settings == 0 || blob == 0)
    {
        return;
    }

    /* 从 flags 提取布尔字段 */
    settings->wifi_enabled =
        ((blob->flags & DEVICE_SETTINGS_FLAG_WIFI_ENABLED) != 0U) ? 1U : 0U;
    settings->ble_enabled =
        ((blob->flags & DEVICE_SETTINGS_FLAG_BLE_ENABLED) != 0U) ? 1U : 0U;
    settings->mqtt_enabled =
        ((blob->flags & DEVICE_SETTINGS_FLAG_MQTT_ENABLED) != 0U) ? 1U : 0U;
    settings->debug_mode_enabled =
        ((blob->flags & DEVICE_SETTINGS_FLAG_DEBUG_MODE_ENABLED) != 0U) ? 1U : 0U;
    settings->esp32_debug_screen_enabled =
        ((blob->flags & DEVICE_SETTINGS_FLAG_ESP32_DEBUG_SCREEN) != 0U) ? 1U : 0U;
    settings->esp32_remote_keys_enabled =
        ((blob->flags & DEVICE_SETTINGS_FLAG_ESP32_REMOTE_KEYS) != 0U) ? 1U : 0U;
    settings->thermal_pause_send_esp_enabled =
        ((blob->flags & DEVICE_SETTINGS_FLAG_THERMAL_PAUSE_SEND_ESP) != 0U) ? 1U : 0U;
    settings->key2_snapshot_enabled =
        ((blob->flags & DEVICE_SETTINGS_FLAG_KEY2_SNAPSHOT_ENABLED) != 0U) ? 1U : 0U;
    settings->standby_enabled = DEVICE_SETTINGS_DEFAULT_STANDBY_ENABLED;
    settings->screen_off_timeout_ms = blob->screen_off_timeout_ms;

    /* V2+ 直接读取 power_policy；V1 从 low_power_enabled 标志推导 */
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

    /* V4+ 新增字段 */
    if (blob->version >= DEVICE_SETTINGS_BLOB_VERSION_V4)
    {
        settings->standby_enabled =
            ((blob->flags & DEVICE_SETTINGS_FLAG_STANDBY_ENABLED) != 0U) ? 1U : 0U;
        settings->rtc_stop_wake_ms = blob->rtc_stop_wake_ms;
        settings->clock_profile_policy = (clock_profile_policy_t)blob->clock_profile_policy;
    }
    else
    {
        /* V3 及以下使用默认值 */
        settings->rtc_stop_wake_ms = DEVICE_SETTINGS_DEFAULT_RTC_STOP_WAKE_MS;
        settings->clock_profile_policy = DEVICE_SETTINGS_DEFAULT_CLOCK_POLICY;
    }

    /* 最终清洗 */
    settings_service_sanitize(settings);
}

/* =========================================================================
 *  9. 公共接口实现
 * ======================================================================= */

/**
 * @brief  初始化设置服务
 * @note   从 BootInfo 的 reserved 字段读取持久化的设置 Blob。
 *         读取成功时执行版本迁移；读取失败或校验不通过时加载默认值并保存。
 */
void settings_service_init(void)
{
    BootInfoTypeDef boot_info;
    device_settings_blob_t blob;
    uint8_t should_resave = 0U;

    /* 加载默认设置 */
    settings_service_load_defaults(&s_settings);

    /* 从 BootInfo 读取持久化数据 */
    ota_service_load_boot_info_copy(&boot_info);
    memcpy(&blob, boot_info.reserved, sizeof(blob));

    /* Blob 校验通过：反序列化并执行版本迁移 */
    if (settings_blob_is_valid(&blob) != 0U)
    {
        settings_from_blob(&s_settings, &blob);

        /* V3 → V4 迁移：启用热成像暂停发送（出厂行为统一） */
        if (blob.version < DEVICE_SETTINGS_BLOB_VERSION_V4)
        {
            s_settings.thermal_pause_send_esp_enabled = 1U;
            should_resave = 1U;
        }

        /* V3 → V4 迁移：旧版灭屏超时 15s 更新为新版默认值 */
        if (blob.version < DEVICE_SETTINGS_BLOB_VERSION_V4 &&
            s_settings.screen_off_timeout_ms == SETTINGS_TIMEOUT_LEGACY_DEFAULT_MS)
        {
            s_settings.screen_off_timeout_ms = DEVICE_SETTINGS_DEFAULT_SCREEN_OFF_TIMEOUT_MS;
            should_resave = 1U;
        }

        /* V4 → V5 迁移：BLE/MQTT 改为用户可控，默认关闭 */
        if (blob.version < DEVICE_SETTINGS_BLOB_VERSION_V5)
        {
            s_settings.ble_enabled  = 0U;
            s_settings.mqtt_enabled = 0U;
            should_resave = 1U;
        }

        /* V5 → V6 迁移：新增 KEY2 快照功能，默认关闭 */
        if (blob.version < DEVICE_SETTINGS_BLOB_VERSION)
        {
            s_settings.key2_snapshot_enabled = 0U;
            should_resave = 1U;
        }

        /* 版本号不一致时统一标记需要重存 */
        if (blob.version != DEVICE_SETTINGS_BLOB_VERSION)
        {
            should_resave = 1U;
        }

        /* 有变更时持久化保存 */
        if (should_resave != 0U)
        {
            (void)settings_service_save();
        }

        return;
    }

    /* Blob 无效：使用默认值并持久化 */
    (void)settings_service_save();
}

/**
 * @brief  获取当前设备设置的只读指针
 * @return 指向当前设置的常量指针
 */
const device_settings_t *settings_service_get(void)
{
    return &s_settings;
}

/**
 * @brief  将当前设置持久化到 Flash
 * @note   流程：清洗 → 加载 BootInfo → 序列化为 Blob → 写入 Flash。
 * @retval 1 — 保存成功；0 — 保存失败
 */
uint8_t settings_service_save(void)
{
    BootInfoTypeDef boot_info;
    device_settings_blob_t blob;

    /* 清洗设置数据 */
    settings_service_sanitize(&s_settings);

    /* 序列化并写入 */
    ota_service_load_boot_info_copy(&boot_info);
    settings_blob_from_settings(&blob, &s_settings);
    memcpy(boot_info.reserved, &blob, sizeof(blob));

    return (ota_service_store_boot_info(&boot_info) == 0U) ? 1U : 0U;
}

/**
 * @brief  更新设备设置
 * @note   采用回滚策略：先备份旧设置，更新后若保存失败则恢复旧值。
 * @param  settings — 新设置数据指针
 * @retval 1 — 更新成功；0 — 更新失败或参数为空
 */
uint8_t settings_service_update(const device_settings_t *settings)
{
    device_settings_t previous_settings;

    if (settings == 0)
    {
        return 0U;
    }

    /* 备份当前设置 */
    previous_settings = s_settings;

    /* 应用新设置并清洗 */
    s_settings = *settings;
    settings_service_sanitize(&s_settings);

    /* 尝试持久化 */
    if (settings_service_save() != 0U)
    {
        return 1U;
    }

    /* 保存失败：回滚到旧设置 */
    s_settings = previous_settings;
    return 0U;
}
