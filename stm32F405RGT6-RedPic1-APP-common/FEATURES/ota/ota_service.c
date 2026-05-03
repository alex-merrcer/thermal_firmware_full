/**
 * @file    ota_service.c
 * @brief   OTA 服务模块 —— 启动信息管理与 OTA 升级/回滚控制
 * @note    本模块管理 BootInfo 启动信息的读写、版本号校验与比较、
 *          试运行（trial）确认机制以及 OTA 升级/回滚请求的发起。
 *
 * @par 启动信息存储
 *      BootInfo 采用日志式（journal）存储到内部 Flash：
 *      - 每次写入追加一个新的日志槽位，避免擦除整个扇区
 *      - 扫描时选择序列号最大的有效槽位作为当前 BootInfo
 *      - 无空闲槽位时擦除整个区域并回写最新条目
 *
 * @par 试运行机制
 *      OTA 升级后首次启动标记为 TRIAL_PENDING，运行 2 秒后
 *      通过 TIM4 中断确认试运行成功，将 boot_info 标记为已确认。
 *
 * @par 版本号格式
 *      X.Y.Z（X/Y/Z 为非负整数），通过逐段数值比较判断大小。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "ota_service.h"

#include <string.h>

#include "delay.h"
#include "sys.h"
#include "usart.h"
#include "flash_if.h"
#include "iap.h"
#include "iwdg.h"
#include "stm32f4xx_tim.h"
#include "app_slot_config.h"
#include "ota_ctrl_protocol.h"

/* =========================================================================
 *  2. 宏定义
 * ======================================================================= */

/** @defgroup OTA_SERVICE_CONST  OTA 服务内部常量
 *  @{ */
#define APP_RUNNING_PARTITION   APP_CFG_RUNNING_PARTITION    /**< 当前运行分区              */
#define APP_OTHER_PARTITION     APP_CFG_OTHER_PARTITION      /**< 非运行分区（升级目标）    */
#define APP_DEFAULT_VERSION     "1.0.2"                      /**< 默认版本号                */
#define APP_BOOT_TRIES_MAX      3U                           /**< 最大启动尝试次数          */
#define APP_OTA_UART_BAUD       115200U                      /**< OTA 通信波特率            */
/** @} */

#ifndef APP_FIRMWARE_VERSION
#define APP_FIRMWARE_VERSION    APP_DEFAULT_VERSION          /**< 固件版本号（编译时定义）  */
#endif

/* =========================================================================
 *  3. 版本标记锚点
 * ======================================================================= */

/**
 * @brief 嵌入式版本标记字符串
 * @note  被链接器保留在二进制中，用于 Bootloader 或外部工具识别固件版本。
 *        格式："IAPFWV1|版本槽位标签|固件版本|"
 */
const char g_app_embedded_version_marker[] =
    "IAPFWV1|" APP_CFG_VERSION_SLOT_TAG "|" APP_FIRMWARE_VERSION "|";

/* =========================================================================
 *  4. 模块级静态变量
 * ======================================================================= */

static volatile uint8_t s_trial_timer_count     = 0U;   /**< 试运行定时器计数            */
static volatile uint8_t s_trial_confirm_pending = 0U;   /**< 试运行确认待处理标志        */
static volatile uint8_t s_trial_confirm_due     = 0U;   /**< 试运行确认到期标志          */
static BootInfoTypeDef  s_boot_info;                    /**< 当前启动信息缓存            */

/* =========================================================================
 *  5. 内部函数前向声明
 * ======================================================================= */

static void     ota_service_tim4_init(void);
static uint32_t ota_service_get_apb1_timer_clock_hz(void);
static void     trial_run_complete(void);
static int8_t   app_version_compare(const char *left, const char *right);
static uint32_t BootInfo_Write(const BootInfoTypeDef *boot_info);
static void     BootInfo_Read(BootInfoTypeDef *boot_info);

/* =========================================================================
 *  6. 定时器配置
 * ======================================================================= */

/**
 * @brief  获取 APB1 定时器时钟频率
 * @note   根据 RCC 时钟配置计算 APB1 定时器实际时钟频率。
 *         当 APB1 预分频 > 1 时，定时器时钟为 PCLK1 的 2 倍。
 * @return APB1 定时器时钟频率（Hz）
 */
static uint32_t ota_service_get_apb1_timer_clock_hz(void)
{
    uint32_t ppre1_bits = RCC->CFGR & RCC_CFGR_PPRE1;
    uint32_t hclk_hz    = SystemCoreClock;
    uint32_t pclk1_hz   = hclk_hz;

    switch (ppre1_bits)
    {
    case RCC_CFGR_PPRE1_DIV2:  pclk1_hz = hclk_hz / 2U;  break;
    case RCC_CFGR_PPRE1_DIV4:  pclk1_hz = hclk_hz / 4U;  break;
    case RCC_CFGR_PPRE1_DIV8:  pclk1_hz = hclk_hz / 8U;  break;
    case RCC_CFGR_PPRE1_DIV16: pclk1_hz = hclk_hz / 16U; break;
    default:                    pclk1_hz = hclk_hz;       break;
    }

    return (ppre1_bits == RCC_CFGR_PPRE1_DIV1) ? pclk1_hz : (pclk1_hz * 2U);
}

/**
 * @brief  初始化 TIM4 定时器
 * @note   配置 TIM4 为 1 秒周期的试运行确认心跳定时器。
 *         时钟变更后需要重新配置。
 */
static void ota_service_tim4_init(void)
{
    TIM_TimeBaseInitTypeDef tim_time_base;
    NVIC_InitTypeDef        nvic_init;
    uint32_t timer_clock_hz = ota_service_get_apb1_timer_clock_hz();
    uint32_t prescaler      = timer_clock_hz / 10000UL;

    if (prescaler == 0U)
    {
        prescaler = 1U;
    }

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);
    TIM_Cmd(TIM4, DISABLE);
    TIM_DeInit(TIM4);

    /* 配置 TIM4：10000 次计数 × 预分频 = 1 秒周期 */
    tim_time_base.TIM_Period        = 10000U - 1U;
    tim_time_base.TIM_Prescaler     = (uint16_t)(prescaler - 1U);
    tim_time_base.TIM_ClockDivision = TIM_CKD_DIV1;
    tim_time_base.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM4, &tim_time_base);

    TIM_ITConfig(TIM4, TIM_IT_Update, ENABLE);

    /* 配置 NVIC */
    nvic_init.NVIC_IRQChannel                   = TIM4_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0;
    nvic_init.NVIC_IRQChannelSubPriority        = 0;
    nvic_init.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init);

    TIM_Cmd(TIM4, ENABLE);
}

/**
 * @brief  TIM4 中断处理函数
 * @note   每秒递增试运行定时器计数，达到 2 秒后触发确认标志。
 */
void TIM4_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM4, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM4, TIM_IT_Update);

        if (s_trial_confirm_pending != 0U)
        {
            s_trial_timer_count++;

            if (s_trial_timer_count >= 2U)
            {
                s_trial_confirm_pending = 0U;
                s_trial_confirm_due     = 1U;
            }
        }
    }
}

/**
 * @brief  重新配置时基（时钟变更后调用）
 */
void ota_service_reconfigure_timebase(void)
{
    ota_service_tim4_init();
}

/* =========================================================================
 *  7. CRC32 校验
 * ======================================================================= */

/**
 * @brief  更新 CRC32 校验值（逐字节）
 * @param  crc    — 当前 CRC 值
 * @param  data   — 数据指针
 * @param  length — 数据长度
 * @return 更新后的 CRC32 值
 */
static uint32_t boot_info_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length)
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

/* =========================================================================
 *  8. 版本号工具函数
 * ======================================================================= */

/**
 * @brief  校验版本号字符串是否合法
 * @note   合法格式：X.Y.Z（X/Y/Z 为非负整数，共 2 个点号）
 * @param  version — 版本号字符串
 * @retval 1 — 合法；0 — 非法
 */
static uint8_t boot_info_version_is_valid(const char *version)
{
    uint32_t i         = 0U;
    uint8_t  dot_count = 0U;
    uint8_t  has_digit = 0U;

    if (version == 0 || version[0] == '\0')
    {
        return 0U;
    }

    for (i = 0U; version[i] != '\0'; ++i)
    {
        char ch = version[i];

        if (ch >= '0' && ch <= '9')
        {
            has_digit = 1U;
            continue;
        }

        if (ch == '.')
        {
            if (has_digit == 0U || dot_count >= 2U)
            {
                return 0U;
            }

            dot_count++;
            has_digit = 0U;
            continue;
        }

        return 0U;
    }

    return (has_digit != 0U && dot_count == 2U) ? 1U : 0U;
}

/**
 * @brief  安全拷贝版本号字符串
 * @note   源版本号无效时使用默认版本号。
 * @param  target     — 目标缓冲区
 * @param  target_len — 目标缓冲区长度
 * @param  source     — 源版本号字符串
 */
static void boot_info_version_copy(char *target, uint32_t target_len, const char *source)
{
    uint32_t i       = 0U;
    const char *value = source;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    if (value == 0 || boot_info_version_is_valid(value) == 0U)
    {
        value = APP_DEFAULT_VERSION;
    }

    memset(target, 0, target_len);

    for (i = 0U; i + 1U < target_len && value[i] != '\0'; ++i)
    {
        target[i] = value[i];
    }
}

/**
 * @brief  判断是否应使用编译时固件版本号
 * @retval 1 — 使用；0 — 不使用
 */
static uint8_t boot_info_should_use_app_version(void)
{
    return (boot_info_version_is_valid(APP_FIRMWARE_VERSION) != 0U) ? 1U : 0U;
}

/**
 * @brief  获取指定分区版本号的可写指针
 * @param  boot_info — 启动信息指针
 * @param  partition — 分区编号
 * @return 版本号字符串指针
 */
static char *boot_info_partition_version_ptr(BootInfoTypeDef *boot_info, uint32_t partition)
{
    if (partition == BOOT_INFO_PARTITION_APP2)
    {
        return boot_info->app2_version;
    }

    return boot_info->app1_version;
}

/**
 * @brief  获取指定分区版本号的只读指针
 * @param  boot_info — 启动信息指针
 * @param  partition — 分区编号
 * @return 版本号字符串指针
 */
static const char *boot_info_get_partition_version(const BootInfoTypeDef *boot_info,
                                                   uint32_t partition)
{
    if (partition == BOOT_INFO_PARTITION_APP2)
    {
        return boot_info->app2_version;
    }

    return boot_info->app1_version;
}

/**
 * @brief  获取用于显示的版本号
 * @note   优先级：current_version > 活跃分区版本 > 编译时版本 > 默认版本
 * @param  boot_info — 启动信息指针
 * @return 版本号字符串指针
 */
static const char *boot_info_get_display_version_internal(const BootInfoTypeDef *boot_info)
{
    const char *partition_version = 0;

    if (boot_info != 0 && boot_info_version_is_valid(boot_info->current_version) != 0U)
    {
        return boot_info->current_version;
    }

    if (boot_info != 0)
    {
        partition_version = boot_info_get_partition_version(boot_info, boot_info->active_partition);

        if (boot_info_version_is_valid(partition_version) != 0U)
        {
            return partition_version;
        }
    }

    if (boot_info_version_is_valid(APP_FIRMWARE_VERSION) != 0U)
    {
        return APP_FIRMWARE_VERSION;
    }

    return APP_DEFAULT_VERSION;
}

/**
 * @brief  同步 current_version 到活跃分区版本
 * @param  boot_info — 启动信息指针
 */
static void boot_info_sync_current_version(BootInfoTypeDef *boot_info)
{
    if (boot_info == 0)
    {
        return;
    }

    boot_info_version_copy(boot_info->current_version,
                           sizeof(boot_info->current_version),
                           boot_info_get_partition_version(boot_info, boot_info->active_partition));
}

/**
 * @brief  设置指定分区的版本号
 * @param  boot_info — 启动信息指针
 * @param  partition — 分区编号
 * @param  version   — 版本号字符串
 */
static void boot_info_set_partition_version(BootInfoTypeDef *boot_info,
                                            uint32_t partition,
                                            const char *version)
{
    char *slot_version = 0;

    if (boot_info == 0)
    {
        return;
    }

    slot_version = boot_info_partition_version_ptr(boot_info, partition);
    boot_info_version_copy(slot_version, BOOT_INFO_VERSION_LEN, version);
    boot_info_sync_current_version(boot_info);
}

/* =========================================================================
 *  9. BootInfo CRC 与分区工具
 * ======================================================================= */

/**
 * @brief  计算 BootInfo 的 CRC32
 * @param  boot_info — 启动信息指针
 * @return CRC32 校验值
 */
static uint32_t boot_info_compute_crc(const BootInfoTypeDef *boot_info)
{
    const uint8_t *data_start = 0;
    uint32_t data_len = 0U;

    if (boot_info == 0)
    {
        return 0U;
    }

    data_start = (const uint8_t *)&boot_info->boot_magic;
    data_len   = (uint32_t)(((const uint8_t *)boot_info + sizeof(BootInfoTypeDef)) - data_start);

    return boot_info_crc32_update(0U, data_start, data_len);
}

/**
 * @brief  获取非活跃分区编号
 * @param  partition — 当前活跃分区
 * @return 非活跃分区编号
 */
static uint32_t boot_info_inactive_partition(uint32_t partition)
{
    return (partition == BOOT_INFO_PARTITION_APP2)
               ? BOOT_INFO_PARTITION_APP1
               : BOOT_INFO_PARTITION_APP2;
}

/**
 * @brief  从三个版本号中选择最小值写入目标
 * @param  target — 目标缓冲区
 * @param  left   — 版本号 1
 * @param  right  — 版本号 2
 * @param  third  — 版本号 3
 */
static void boot_info_set_min_version(char *target,
                                      const char *left,
                                      const char *right,
                                      const char *third)
{
    const char *candidate = left;

    if (app_version_compare(right, candidate) > 0)
    {
        candidate = right;
    }

    if (app_version_compare(third, candidate) > 0)
    {
        candidate = third;
    }

    boot_info_version_copy(target, BOOT_INFO_VERSION_LEN, candidate);
}

/* =========================================================================
 *  10. BootInfo 确认与调和
 * ======================================================================= */

/**
 * @brief  标记当前启动信息为已确认
 * @note   试运行成功后调用，将启动信息重置为正常状态：
 *         - 确认活跃分区
 *         - 清除试运行状态
 *         - 设置 boot_magic 为 NORMAL
 *         - 更新 last_good_version 和 min_allowed_ota_version
 */
static void boot_info_mark_confirmed(BootInfoTypeDef *boot_info)
{
    const char *running_version = APP_FIRMWARE_VERSION;

    if (boot_info == 0)
    {
        return;
    }

    if (boot_info_version_is_valid(running_version) == 0U)
    {
        running_version = boot_info_get_partition_version(boot_info, APP_RUNNING_PARTITION);
    }

    boot_info_version_copy(boot_info_partition_version_ptr(boot_info, APP_RUNNING_PARTITION),
                           BOOT_INFO_VERSION_LEN, running_version);

    boot_info->active_partition   = APP_RUNNING_PARTITION;
    boot_info->confirmed_slot     = APP_RUNNING_PARTITION;
    boot_info->target_partition   = APP_OTHER_PARTITION;
    boot_info->trial_state        = BOOT_INFO_TRIAL_NONE;
    boot_info->boot_magic         = MAGIC_NORMAL;
    boot_info->upgrade_flag       = BOOT_UPGRADE_FLAG_NONE;
    boot_info->boot_tries         = APP_BOOT_TRIES_MAX;

    boot_info_sync_current_version(boot_info);

    boot_info_version_copy(boot_info->last_good_version,
                           sizeof(boot_info->last_good_version),
                           boot_info->current_version);

    boot_info_set_min_version(boot_info->min_allowed_ota_version,
                              boot_info->min_allowed_ota_version,
                              boot_info->pending_floor_version,
                              boot_info->current_version);

    boot_info_version_copy(boot_info->pending_floor_version,
                           sizeof(boot_info->pending_floor_version),
                           APP_DEFAULT_VERSION);
}

/**
 * @brief  调和运行镜像与 BootInfo 的一致性
 * @note   手工重新烧录 APP 后，镜像版本可能与 BootInfo 中记录的不一致。
 *         此函数将 BootInfo 中的版本号对齐到编译进镜像的真实版本。
 *         试运行阶段只修正当前槽位和 current_version，不改 confirmed/last_good。
 * @param  boot_info — 启动信息指针
 * @retval 1 — 已修改；0 — 无需修改
 */
static uint8_t boot_info_reconcile_running_image(BootInfoTypeDef *boot_info)
{
    uint8_t modified = 0U;

    if (boot_info == 0 || boot_info_should_use_app_version() == 0U)
    {
        return 0U;
    }

    /* 修正活跃分区 */
    if (boot_info->active_partition != APP_RUNNING_PARTITION)
    {
        boot_info->active_partition = APP_RUNNING_PARTITION;
        modified = 1U;
    }

    /* 修正目标分区 */
    if (boot_info->target_partition != APP_OTHER_PARTITION)
    {
        boot_info->target_partition = APP_OTHER_PARTITION;
        modified = 1U;
    }

    /* 修正当前槽位版本号 */
    if (strcmp(boot_info_get_partition_version(boot_info, APP_RUNNING_PARTITION),
               APP_FIRMWARE_VERSION) != 0)
    {
        boot_info_version_copy(boot_info_partition_version_ptr(boot_info, APP_RUNNING_PARTITION),
                               BOOT_INFO_VERSION_LEN, APP_FIRMWARE_VERSION);
        modified = 1U;
    }

    /* 修正 current_version */
    if (strcmp(boot_info->current_version, APP_FIRMWARE_VERSION) != 0)
    {
        boot_info_sync_current_version(boot_info);
        modified = 1U;
    }

    /* 非试运行状态下修正 confirmed_slot 和 last_good_version */
    if (boot_info->trial_state == BOOT_INFO_TRIAL_NONE)
    {
        if (boot_info->confirmed_slot != APP_RUNNING_PARTITION)
        {
            boot_info->confirmed_slot = APP_RUNNING_PARTITION;
            modified = 1U;
        }

        if (strcmp(boot_info->last_good_version, APP_FIRMWARE_VERSION) != 0)
        {
            boot_info_version_copy(boot_info->last_good_version,
                                   sizeof(boot_info->last_good_version),
                                   APP_FIRMWARE_VERSION);
            modified = 1U;
        }
    }

    if (modified != 0U)
    {
        boot_info->data_crc32 = boot_info_compute_crc(boot_info);
    }

    return modified;
}

/* =========================================================================
 *  11. BootInfo 初始化与校验
 * ======================================================================= */

/**
 * @brief  初始化默认 BootInfo
 * @param  boot_info — 启动信息指针
 */
static void boot_info_init_default(BootInfoTypeDef *boot_info)
{
    if (boot_info == 0)
    {
        return;
    }

    memset(boot_info, 0, sizeof(*boot_info));

    /* 布局信息 */
    boot_info->layout_magic   = BOOT_INFO_LAYOUT_MAGIC;
    boot_info->layout_version = BOOT_INFO_LAYOUT_VERSION;
    boot_info->struct_size    = (uint16_t)sizeof(BootInfoTypeDef);

    /* 启动状态 */
    boot_info->boot_magic      = MAGIC_NORMAL;
    boot_info->upgrade_flag    = BOOT_UPGRADE_FLAG_NONE;
    boot_info->active_partition = APP_RUNNING_PARTITION;
    boot_info->target_partition = APP_OTHER_PARTITION;
    boot_info->confirmed_slot   = APP_RUNNING_PARTITION;
    boot_info->trial_state      = BOOT_INFO_TRIAL_NONE;
    boot_info->boot_tries       = APP_BOOT_TRIES_MAX;
    boot_info->rollback_counter = 0U;

    /* 初始化所有版本号为默认值 */
    boot_info_version_copy(boot_info->app1_version,
                           sizeof(boot_info->app1_version), APP_DEFAULT_VERSION);
    boot_info_version_copy(boot_info->app2_version,
                           sizeof(boot_info->app2_version), APP_DEFAULT_VERSION);
    boot_info_version_copy(boot_info->last_good_version,
                           sizeof(boot_info->last_good_version), APP_DEFAULT_VERSION);
    boot_info_version_copy(boot_info->min_allowed_ota_version,
                           sizeof(boot_info->min_allowed_ota_version), APP_DEFAULT_VERSION);
    boot_info_version_copy(boot_info->pending_floor_version,
                           sizeof(boot_info->pending_floor_version), APP_DEFAULT_VERSION);

    /* 设置当前运行分区版本 */
    if (boot_info_should_use_app_version() != 0U)
    {
        boot_info_set_partition_version(boot_info, APP_RUNNING_PARTITION, APP_FIRMWARE_VERSION);
    }
    else
    {
        boot_info_sync_current_version(boot_info);
    }

    boot_info_version_copy(boot_info->last_good_version,
                           sizeof(boot_info->last_good_version),
                           boot_info->current_version);

    boot_info->data_crc32 = boot_info_compute_crc(boot_info);
}

/**
 * @brief  校验 BootInfo 是否有效
 * @note   校验内容：布局魔数、版本、结构大小、分区编号范围、
 *         启动魔数、版本号合法性、current_version 一致性、
 *         confirmed_slot 一致性和 CRC32。
 * @param  boot_info — 启动信息指针
 * @retval 1 — 有效；0 — 无效
 */
static uint8_t boot_info_is_valid(const BootInfoTypeDef *boot_info)
{
    if (boot_info == 0)
    {
        return 0U;
    }

    /* 布局信息校验 */
    if (boot_info->layout_magic != BOOT_INFO_LAYOUT_MAGIC ||
        boot_info->layout_version != BOOT_INFO_LAYOUT_VERSION ||
        boot_info->struct_size != sizeof(BootInfoTypeDef))
    {
        return 0U;
    }

    /* 枚举值范围校验 */
    if (boot_info->active_partition > BOOT_INFO_PARTITION_APP2 ||
        boot_info->target_partition > BOOT_INFO_PARTITION_APP2 ||
        boot_info->confirmed_slot > BOOT_INFO_PARTITION_APP2 ||
        boot_info->upgrade_flag > BOOT_UPGRADE_FLAG_ROLLBACK ||
        boot_info->boot_tries > APP_BOOT_TRIES_MAX ||
        boot_info->trial_state > BOOT_INFO_TRIAL_PENDING)
    {
        return 0U;
    }

    /* 启动魔数校验 */
    if (boot_info->boot_magic != MAGIC_NORMAL &&
        boot_info->boot_magic != MAGIC_REQUEST &&
        boot_info->boot_magic != MAGIC_NEW_FW)
    {
        return 0U;
    }

    /* 版本号合法性校验 */
    if (boot_info_version_is_valid(boot_info->current_version) == 0U ||
        boot_info_version_is_valid(boot_info->app1_version) == 0U ||
        boot_info_version_is_valid(boot_info->app2_version) == 0U ||
        boot_info_version_is_valid(boot_info->last_good_version) == 0U ||
        boot_info_version_is_valid(boot_info->min_allowed_ota_version) == 0U ||
        boot_info_version_is_valid(boot_info->pending_floor_version) == 0U)
    {
        return 0U;
    }

    /* current_version 与活跃分区版本一致性校验 */
    if (strcmp(boot_info->current_version,
               boot_info_get_partition_version(boot_info, boot_info->active_partition)) != 0)
    {
        return 0U;
    }

    /* 非试运行状态下 confirmed_slot 必须等于 active_partition */
    if (boot_info->trial_state == BOOT_INFO_TRIAL_NONE &&
        boot_info->confirmed_slot != boot_info->active_partition)
    {
        return 0U;
    }

    /* CRC32 校验 */
    return (boot_info->data_crc32 == boot_info_compute_crc(boot_info)) ? 1U : 0U;
}

/* =========================================================================
 *  12. 日志式（Journal）存储系统
 * ======================================================================= */

/** @defgroup JOURNAL_CONST  日志存储常量
 *  @{ */
#define APP_BOOT_JOURNAL_REGION_ADDR    BOOT_INFO_ADDR              /**< Boot 日志区域起始地址 */
#define APP_TXN_JOURNAL_REGION_ADDR     0x0800E000U                 /**< 事务日志区域起始地址  */
#define APP_JOURNAL_REGION_SIZE         0x2000U                     /**< 日志区域大小（8KB）  */
#define APP_BOOTINFO_SECTOR_SIZE        (ADDR_FLASH_SECTOR_4 - BOOT_INFO_ADDR) /**< 扇区大小 */
#define APP_TXN_REGION_OFFSET           (APP_TXN_JOURNAL_REGION_ADDR - APP_BOOT_JOURNAL_REGION_ADDR)
#define APP_JOURNAL_SLOT_SIZE           256U                        /**< 日志槽位大小（字节） */
#define APP_JOURNAL_SLOT_COUNT          (APP_JOURNAL_REGION_SIZE / APP_JOURNAL_SLOT_SIZE) /**< 槽位总数 */
#define APP_JOURNAL_PAYLOAD_SIZE        236U                        /**< 槽位载荷大小        */
#define APP_JOURNAL_SLOT_VERSION        1U                          /**< 槽位格式版本        */
#define APP_JOURNAL_COMMIT_MAGIC        0x434D4954UL                /**< 提交魔数 "CMIT"     */
#define APP_BOOT_JOURNAL_MAGIC          0x424A4E4CUL                /**< Boot 日志魔数 "BJNL" */
#define APP_TXN_JOURNAL_MAGIC           0x544A4E4CUL                /**< 事务日志魔数 "TJNL"  */
/** @} */

/**
 * @brief 扇区备份缓冲区（用于擦除前备份）
 */
static uint32_t s_app_boot_sector_backup[APP_BOOTINFO_SECTOR_SIZE / sizeof(uint32_t)];

/**
 * @brief 日志槽位结构体
 */
typedef struct
{
    uint32_t slot_magic;                        /**< 槽位魔数                      */
    uint16_t slot_version;                      /**< 槽位格式版本                  */
    uint16_t payload_size;                      /**< 载荷大小                      */
    uint32_t slot_seq;                          /**< 槽位序列号                    */
    uint32_t payload_crc32;                     /**< 载荷 CRC32                    */
    uint8_t  payload[APP_JOURNAL_PAYLOAD_SIZE]; /**< 载荷数据                      */
    uint32_t commit_magic;                      /**< 提交魔数（写入完成标志）      */
} app_journal_slot_t;

/**
 * @brief 日志扫描结果结构体
 */
typedef struct
{
    uint8_t  has_valid;                         /**< 是否找到有效槽位              */
    uint8_t  has_empty;                         /**< 是否找到空闲槽位              */
    uint32_t latest_seq;                        /**< 最新有效槽位的序列号          */
    uint32_t latest_addr;                       /**< 最新有效槽位的地址            */
    uint32_t empty_addr;                        /**< 第一个空闲槽位的地址          */
} app_journal_scan_t;

/** @brief 编译期断言：槽位大小必须为 256 字节 */
typedef char app_journal_slot_size_check[(sizeof(app_journal_slot_t) == APP_JOURNAL_SLOT_SIZE) ? 1 : -1];

/**
 * @brief  判断候选序列号是否比当前序列号更新
 * @param  candidate — 候选序列号
 * @param  current   — 当前序列号
 * @retval 1 — 更新；0 — 不更新
 */
static uint8_t app_journal_seq_is_newer(uint32_t candidate, uint32_t current)
{
    if (candidate == 0U || candidate == 0xFFFFFFFFUL)
    {
        return 0U;
    }

    if (current == 0U || current == 0xFFFFFFFFUL)
    {
        return 1U;
    }

    return (((int32_t)(candidate - current)) > 0) ? 1U : 0U;
}

/**
 * @brief  获取下一个序列号
 * @param  current — 当前序列号
 * @return 下一个序列号（跳过 0 和 0xFFFFFFFF）
 */
static uint32_t app_journal_seq_next(uint32_t current)
{
    uint32_t next = current + 1U;

    if (next == 0U || next == 0xFFFFFFFFUL)
    {
        next = 1U;
    }

    return next;
}

/**
 * @brief  判断槽位是否已擦除（全 0xFF）
 * @param  slot — 槽位指针
 * @retval 1 — 已擦除；0 — 未擦除
 */
static uint8_t app_journal_slot_is_erased(const app_journal_slot_t *slot)
{
    const uint32_t *words = (const uint32_t *)slot;
    uint32_t index = 0U;

    if (slot == 0)
    {
        return 0U;
    }

    for (index = 0U; index < (APP_JOURNAL_SLOT_SIZE / sizeof(uint32_t)); ++index)
    {
        if (words[index] != 0xFFFFFFFFUL)
        {
            return 0U;
        }
    }

    return 1U;
}

/**
 * @brief  校验槽位结构是否有效
 * @note   校验魔数、版本、载荷大小、序列号、提交魔数和载荷 CRC32。
 * @param  slot           — 槽位指针
 * @param  expected_magic — 期望的槽位魔数
 * @retval 1 — 有效；0 — 无效
 */
static uint8_t app_journal_slot_is_structurally_valid(const app_journal_slot_t *slot,
                                                      uint32_t expected_magic)
{
    if (slot == 0)
    {
        return 0U;
    }

    if (slot->slot_magic != expected_magic ||
        slot->slot_version != APP_JOURNAL_SLOT_VERSION ||
        slot->payload_size == 0U ||
        slot->payload_size > APP_JOURNAL_PAYLOAD_SIZE ||
        slot->slot_seq == 0U ||
        slot->slot_seq == 0xFFFFFFFFUL ||
        slot->commit_magic != APP_JOURNAL_COMMIT_MAGIC)
    {
        return 0U;
    }

    return (boot_info_crc32_update(0U, slot->payload, slot->payload_size) == slot->payload_crc32)
               ? 1U : 0U;
}

/**
 * @brief  校验槽位是否为有效的 BootInfo 日志
 * @param  slot — 槽位指针
 * @retval 1 — 有效；0 — 无效
 */
static uint8_t app_journal_slot_is_boot_info_valid(const app_journal_slot_t *slot)
{
    if (app_journal_slot_is_structurally_valid(slot, APP_BOOT_JOURNAL_MAGIC) == 0U ||
        slot->payload_size != sizeof(BootInfoTypeDef))
    {
        return 0U;
    }

    return boot_info_is_valid((const BootInfoTypeDef *)slot->payload);
}

/**
 * @brief  扫描 Boot 日志区域，找到最新有效槽位和空闲槽位
 * @param  scan           — [out] 扫描结果
 * @param  latest_boot_info — [out] 最新 BootInfo 副本（可为 NULL）
 */
static void app_journal_scan_boot(app_journal_scan_t *scan, BootInfoTypeDef *latest_boot_info)
{
    uint32_t index = 0U;

    if (scan == 0)
    {
        return;
    }

    memset(scan, 0, sizeof(*scan));

    if (latest_boot_info != 0)
    {
        memset(latest_boot_info, 0, sizeof(*latest_boot_info));
    }

    for (index = 0U; index < APP_JOURNAL_SLOT_COUNT; ++index)
    {
        const app_journal_slot_t *slot =
            (const app_journal_slot_t *)(APP_BOOT_JOURNAL_REGION_ADDR + (index * APP_JOURNAL_SLOT_SIZE));

        /* 跳过已擦除槽位，记录第一个空闲位置 */
        if (app_journal_slot_is_erased(slot) != 0U)
        {
            if (scan->has_empty == 0U)
            {
                scan->has_empty = 1U;
                scan->empty_addr = APP_BOOT_JOURNAL_REGION_ADDR + (index * APP_JOURNAL_SLOT_SIZE);
            }
            continue;
        }

        /* 检查有效槽位，保留序列号最大的 */
        if (app_journal_slot_is_boot_info_valid(slot) != 0U)
        {
            if (scan->has_valid == 0U ||
                app_journal_seq_is_newer(slot->slot_seq, scan->latest_seq) != 0U)
            {
                scan->has_valid  = 1U;
                scan->latest_seq = slot->slot_seq;
                scan->latest_addr = APP_BOOT_JOURNAL_REGION_ADDR + (index * APP_JOURNAL_SLOT_SIZE);

                if (latest_boot_info != 0)
                {
                    memcpy(latest_boot_info, slot->payload, sizeof(*latest_boot_info));
                }
            }
        }
    }
}

/**
 * @brief  构造 BootInfo 日志槽位
 * @param  slot      — [out] 槽位结构体
 * @param  sequence  — 序列号
 * @param  boot_info — 启动信息
 */
static void app_journal_build_boot_slot(app_journal_slot_t *slot,
                                        uint32_t sequence,
                                        const BootInfoTypeDef *boot_info)
{
    memset(slot, 0xFF, sizeof(*slot));

    slot->slot_magic   = APP_BOOT_JOURNAL_MAGIC;
    slot->slot_version = APP_JOURNAL_SLOT_VERSION;
    slot->payload_size = (uint16_t)sizeof(BootInfoTypeDef);
    slot->slot_seq     = sequence;

    memcpy(slot->payload, boot_info, sizeof(BootInfoTypeDef));

    slot->payload_crc32 = boot_info_crc32_update(0U, slot->payload, slot->payload_size);
    slot->commit_magic  = APP_JOURNAL_COMMIT_MAGIC;
}

/**
 * @brief  写入日志槽位到 Flash
 * @param  slot_addr — 槽位地址
 * @param  slot      — 槽位数据
 * @return FLASH_If_Write 返回值（0=成功）
 */
static uint32_t app_journal_write_slot(uint32_t slot_addr, const app_journal_slot_t *slot)
{
    uint32_t flash_addr = slot_addr;

    return FLASH_If_Write(&flash_addr,
                          (uint32_t *)(void *)slot,
                          APP_JOURNAL_SLOT_SIZE / sizeof(uint32_t));
}

/* =========================================================================
 *  13. BootInfo 读写接口
 * ======================================================================= */

/**
 * @brief  从日志区域加载当前 BootInfo
 * @param  boot_info — [out] 启动信息指针
 * @retval 1 — 加载成功；0 — 无有效数据
 */
static uint8_t app_boot_info_load_current(BootInfoTypeDef *boot_info)
{
    app_journal_scan_t scan;

    app_journal_scan_boot(&scan, boot_info);

    if (scan.has_valid != 0U)
    {
        return 1U;
    }

    /* 日志区域无有效数据，尝试直接读取原始地址 */
    memcpy(boot_info, (const void *)BOOT_INFO_ADDR, sizeof(*boot_info));
    return (boot_info_is_valid(boot_info) != 0U) ? 1U : 0U;
}

/**
 * @brief  写入 BootInfo 到日志区域
 * @note   日志式写入流程：
 *         1. 扫描日志区域找到空闲槽位
 *         2. 有空闲槽位：直接追加写入
 *         3. 无空闲槽位：备份整个区域，擦除，回写有效槽位，再写入新槽位
 *         4. 同时备份事务日志区域
 * @param  boot_info — 启动信息指针
 * @return 0 — 成功；非 0 — 失败
 */
static uint32_t BootInfo_Write(const BootInfoTypeDef *boot_info)
{
    BootInfoTypeDef    prepared;
    app_journal_scan_t scan;
    app_journal_slot_t slot;
    uint32_t next_sequence = 1U;
    uint32_t slot_addr     = 0U;

    if (boot_info == 0)
    {
        return 1U;
    }

    /* 准备写入数据 */
    prepared = *boot_info;
    prepared.layout_magic   = BOOT_INFO_LAYOUT_MAGIC;
    prepared.layout_version = BOOT_INFO_LAYOUT_VERSION;
    prepared.struct_size    = (uint16_t)sizeof(BootInfoTypeDef);
    prepared.data_crc32     = boot_info_compute_crc(&prepared);

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                    FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    /* 扫描日志区域 */
    app_journal_scan_boot(&scan, 0);

    if (scan.has_valid != 0U)
    {
        next_sequence = app_journal_seq_next(scan.latest_seq);
    }

    if (scan.has_empty != 0U)
    {
        /* 有空闲槽位：直接追加 */
        slot_addr = scan.empty_addr;
    }
    else
    {
        /* 无空闲槽位：擦除并回写（C3 修复：增加备份完整性校验） */
        uint32_t index           = 0U;
        uint32_t txn_flash_addr  = APP_TXN_JOURNAL_REGION_ADDR;
        uint32_t backup_crc_before  = 0U;
        uint32_t backup_crc_after   = 0U;

        /* 备份整个日志区域 */
        memcpy(s_app_boot_sector_backup,
               (const void *)APP_BOOT_JOURNAL_REGION_ADDR,
               APP_BOOTINFO_SECTOR_SIZE);

        /* C3 修复：计算备份数据 CRC32，擦除前后各校验一次 */
        backup_crc_before = boot_info_crc32_update(
            0U, (const uint8_t *)s_app_boot_sector_backup, APP_BOOTINFO_SECTOR_SIZE);

        if (MY_FLASH_Erase(APP_BOOT_JOURNAL_REGION_ADDR) != 0U)
        {
            FLASH_Lock();
            return 1U;
        }

        /* 回写有效槽位（跳过最新条目，它将被新数据替代） */
        slot_addr = APP_BOOT_JOURNAL_REGION_ADDR;

        for (index = 0U; index < APP_JOURNAL_SLOT_COUNT; ++index)
        {
            app_journal_slot_t *existing_slot =
                (app_journal_slot_t *)((uint8_t *)s_app_boot_sector_backup +
                                       (index * APP_JOURNAL_SLOT_SIZE));

            if (app_journal_slot_is_boot_info_valid(existing_slot) == 0U)
            {
                continue;
            }

            /* 跳过最新条目 */
            if (scan.latest_addr ==
                (APP_BOOT_JOURNAL_REGION_ADDR + (index * APP_JOURNAL_SLOT_SIZE)))
            {
                continue;
            }

            if (app_journal_write_slot(slot_addr, existing_slot) != 0U)
            {
                FLASH_Lock();
                return 1U;
            }

            slot_addr += APP_JOURNAL_SLOT_SIZE;
        }

        /* 备份事务日志区域 */
        if (FLASH_If_Write(&txn_flash_addr,
                           &s_app_boot_sector_backup[APP_TXN_REGION_OFFSET / sizeof(uint32_t)],
                           APP_JOURNAL_REGION_SIZE / sizeof(uint32_t)) != 0U)
        {
            FLASH_Lock();
            return 1U;
        }

        /* C3 修复：回写完成后校验 RAM 备份数据完整性 */
        backup_crc_after = boot_info_crc32_update(
            0U, (const uint8_t *)s_app_boot_sector_backup, APP_BOOTINFO_SECTOR_SIZE);
        if (backup_crc_before != backup_crc_after)
        {
            /* RAM 备份在压缩过程中被篡改（可能被中断或 DMA 踩踏），报告失败 */
            FLASH_Lock();
            return 1U;
        }
    }

    /* 写入新槽位 */
    app_journal_build_boot_slot(&slot, next_sequence, &prepared);

    if (app_journal_write_slot(slot_addr, &slot) != 0U)
    {
        FLASH_Lock();
        return 1U;
    }

    FLASH_Lock();
    return 0U;
}

/**
 * @brief  读取 BootInfo
 * @note   优先从日志区域加载，无效时初始化默认值并写入。
 * @param  boot_info — [out] 启动信息指针
 */
static void BootInfo_Read(BootInfoTypeDef *boot_info)
{
    if (boot_info == 0)
    {
        return;
    }

    if (app_boot_info_load_current(boot_info) != 0U)
    {
        return;
    }

    boot_info_init_default(boot_info);
    BootInfo_Write(boot_info);
}

/* =========================================================================
 *  14. 版本号比较
 * ======================================================================= */

/**
 * @brief  读取版本号的一个分量
 * @param  cursor — [in/out] 字符串游标指针
 * @param  value  — [out] 读取到的数值
 * @retval 1 — 读取成功；0 — 失败
 */
static uint8_t app_version_read_component(const char **cursor, uint32_t *value)
{
    const char *ptr    = 0;
    uint32_t   result  = 0U;
    uint8_t    has_digit = 0U;

    if (cursor == 0 || *cursor == 0 || value == 0)
    {
        return 0U;
    }

    ptr = *cursor;

    while (*ptr >= '0' && *ptr <= '9')
    {
        result = (result * 10U) + (uint32_t)(*ptr - '0');
        ptr++;
        has_digit = 1U;
    }

    if (has_digit == 0U)
    {
        return 0U;
    }

    *cursor = ptr;
    *value  = result;

    return 1U;
}

/**
 * @brief  比较两个版本号
 * @note   逐段比较 X.Y.Z 的每个分量。
 * @param  left  — 版本号 1
 * @param  right — 版本号 2
 * @return >0 — left > right；<0 — left < right；0 — 相等或无效
 */
static int8_t app_version_compare(const char *left, const char *right)
{
    const char *left_ptr   = left;
    const char *right_ptr  = right;
    uint32_t    left_value  = 0U;
    uint32_t    right_value = 0U;
    uint8_t     index       = 0U;

    if (boot_info_version_is_valid(left) == 0U || boot_info_version_is_valid(right) == 0U)
    {
        return 0;
    }

    for (index = 0U; index < 3U; ++index)
    {
        if (app_version_read_component(&left_ptr, &left_value) == 0U ||
            app_version_read_component(&right_ptr, &right_value) == 0U)
        {
            return 0;
        }

        if (left_value > right_value) { return  1; }
        if (left_value < right_value) { return -1; }

        /* 前两段后面必须有分隔符 '.' */
        if (index < 2U)
        {
            if (*left_ptr != '.' || *right_ptr != '.')
            {
                return 0;
            }

            left_ptr++;
            right_ptr++;
        }
    }

    return 0;
}

/* =========================================================================
 *  15. 公共接口实现 —— 错误原因文本
 * ======================================================================= */

/**
 * @brief  获取 OTA 错误原因的可读文本
 * @param  reason — 错误原因码
 * @return 错误原因文本
 */
const char *ota_service_reason_text(uint16_t reason)
{
    switch (reason)
    {
    case OTA_CTRL_ERR_BUSY:          return "ESP32 busy";
    case OTA_CTRL_ERR_NO_WIFI:       return "No WiFi";
    case OTA_CTRL_ERR_FETCH_METADATA: return "Meta failed";
    case OTA_CTRL_ERR_NO_PACKAGE:    return "No package";
    case OTA_CTRL_ERR_PRODUCT:       return "Product err";
    case OTA_CTRL_ERR_HW_REV:        return "HW rev err";
    case OTA_CTRL_ERR_PROTOCOL:      return "Protocol err";
    case OTA_CTRL_ERR_PARTITION:     return "Partition err";
    case OTA_CTRL_ERR_VERSION:       return "Version err";
    case OTA_CTRL_ERR_NO_UPDATE:     return "No update";
    default:                          return "UART timeout";
    }
}

/* =========================================================================
 *  16. 试运行确认
 * ======================================================================= */

/**
 * @brief  完成试运行确认
 * @note   读取 BootInfo，标记为已确认，写回并刷新缓存。
 */
static void trial_run_complete(void)
{
    BootInfoTypeDef boot_info;

    BootInfo_Read(&boot_info);
    boot_info_mark_confirmed(&boot_info);
    BootInfo_Write(&boot_info);
    BootInfo_Read(&s_boot_info);
}

/* =========================================================================
 *  17. 公共接口实现 —— 初始化与轮询
 * ======================================================================= */

/**
 * @brief  重新加载 BootInfo 并调和运行镜像
 */
static void ota_service_reload_boot_info(void)
{
    BootInfo_Read(&s_boot_info);

    if (boot_info_reconcile_running_image(&s_boot_info) != 0U)
    {
        BootInfo_Write(&s_boot_info);
        BootInfo_Read(&s_boot_info);
    }
}

/**
 * @brief  初始化 OTA 服务
 * @note   加载 BootInfo，检查是否处于试运行状态，
 *         初始化 TIM4 定时器用于试运行确认。
 */
void ota_service_init(void)
{
    volatile const void *marker_anchor = g_app_embedded_version_marker;
    (void)marker_anchor;

    ota_service_reload_boot_info();

    /* 检查是否处于试运行状态 */
    if (s_boot_info.boot_magic == MAGIC_NEW_FW &&
        s_boot_info.trial_state == BOOT_INFO_TRIAL_PENDING)
    {
        s_trial_confirm_pending = 1U;
        s_trial_timer_count     = 0U;
        s_trial_confirm_due     = 0U;
    }
    else
    {
        s_trial_confirm_pending = 0U;
        s_trial_timer_count     = 0U;
        s_trial_confirm_due     = 0U;
    }

    ota_service_tim4_init();
    __enable_irq();
}

/**
 * @brief  OTA 服务轮询
 * @note   在主循环中调用，检查试运行确认是否到期。
 */
void ota_service_poll(void)
{
    if (s_trial_confirm_due != 0U)
    {
        s_trial_confirm_due = 0U;
        IWDG_Feed();
        trial_run_complete();
        IWDG_Feed();
    }
}

/* =========================================================================
 *  18. 公共接口实现 —— BootInfo 访问
 * ======================================================================= */

/**
 * @brief  刷新启动信息缓存
 */
void ota_service_refresh_info(void)
{
    ota_service_reload_boot_info();
}

/**
 * @brief  加载 BootInfo 副本
 * @param  boot_info — [out] 输出缓冲区
 */
void ota_service_load_boot_info_copy(BootInfoTypeDef *boot_info)
{
    BootInfo_Read(boot_info);
}

/**
 * @brief  存储 BootInfo
 * @param  boot_info — 启动信息指针
 * @return 0 — 成功；非 0 — 失败
 */
uint32_t ota_service_store_boot_info(const BootInfoTypeDef *boot_info)
{
    uint32_t status = BootInfo_Write(boot_info);

    if (status == 0U)
    {
        BootInfo_Read(&s_boot_info);
    }

    return status;
}

/**
 * @brief  获取当前 BootInfo 只读指针
 * @return BootInfo 指针
 */
const BootInfoTypeDef *ota_service_get_boot_info(void)
{
    return &s_boot_info;
}

/**
 * @brief  获取用于显示的版本号
 * @return 版本号字符串
 */
const char *ota_service_get_display_version(void)
{
    return boot_info_get_display_version_internal(&s_boot_info);
}

/**
 * @brief  获取指定分区的版本号
 * @param  partition — 分区编号
 * @return 版本号字符串
 */
const char *ota_service_get_partition_version(uint32_t partition)
{
    return boot_info_get_partition_version(&s_boot_info, partition);
}

/**
 * @brief  获取指定分区的名称
 * @param  partition — 分区编号
 * @return 分区名称字符串（"APP1" 或 "APP2"）
 */
const char *ota_service_get_partition_name(uint32_t partition)
{
    return (partition == BOOT_INFO_PARTITION_APP2) ? "APP2" : "APP1";
}

/**
 * @brief  获取当前活跃分区编号
 * @return 活跃分区编号
 */
uint32_t ota_service_get_active_partition(void)
{
    return s_boot_info.active_partition;
}

/**
 * @brief  获取非活跃分区编号
 * @return 非活跃分区编号
 */
uint32_t ota_service_get_inactive_partition(void)
{
    return boot_info_inactive_partition(s_boot_info.active_partition);
}

/**
 * @brief  比较两个版本号
 * @param  left  — 版本号 1
 * @param  right — 版本号 2
 * @return >0 — left > right；<0 — left < right；0 — 相等
 */
int8_t ota_service_compare_version(const char *left, const char *right)
{
    return app_version_compare(left, right);
}

/* =========================================================================
 *  19. 公共接口实现 —— OTA 查询与升级/回滚
 * ======================================================================= */

/**
 * @brief  查询最新 OTA 固件版本
 * @param  latest_version      — [out] 最新版本号缓冲区
 * @param  latest_version_len  — 缓冲区长度
 * @param  reject_reason       — [out] 拒绝原因码（可为 NULL）
 * @retval 1 — 查询成功；0 — 失败
 */
uint8_t ota_service_query_latest_version(char *latest_version,
                                         uint16_t latest_version_len,
                                         uint16_t *reject_reason)
{
    if (latest_version != 0 && latest_version_len > 0U)
    {
        memset(latest_version, 0, latest_version_len);
    }

    if (reject_reason != 0)
    {
        *reject_reason = 0U;
    }

    ota_service_refresh_info();

    /* 确保 UART 波特率正确 */
    if (uart_get_current_baud() != APP_OTA_UART_BAUD)
    {
        uart_init(APP_OTA_UART_BAUD);
    }

    return iap_query_latest_version(&s_boot_info,
                                    latest_version,
                                    latest_version_len,
                                    reject_reason);
}

/**
 * @brief  请求 OTA 升级
 * @note   设置 boot_magic 为 MAGIC_REQUEST，upgrade_flag 为 UPGRADE，
 *         写入 BootInfo 后重启系统。
 */
void ota_service_request_upgrade(void)
{
    BootInfoTypeDef boot_info;

    BootInfo_Read(&boot_info);

    boot_info.boot_magic   = MAGIC_REQUEST;
    boot_info.upgrade_flag = BOOT_UPGRADE_FLAG_UPGRADE;

    BootInfo_Write(&boot_info);
    memcpy(&s_boot_info, &boot_info, sizeof(s_boot_info));
    __DSB();                                                    /* C4 修复：确保 Flash 写入完成 */
    __ISB();
    NVIC_SystemReset();
}

/**
 * @brief  请求 OTA 回滚
 * @note   设置 boot_magic 为 MAGIC_REQUEST，upgrade_flag 为 ROLLBACK，
 *         写入 BootInfo 后重启系统。
 */
void ota_service_request_rollback(void)
{
    BootInfoTypeDef boot_info;

    BootInfo_Read(&boot_info);

    boot_info.boot_magic   = MAGIC_REQUEST;
    boot_info.upgrade_flag = BOOT_UPGRADE_FLAG_ROLLBACK;

    BootInfo_Write(&boot_info);
    memcpy(&s_boot_info, &boot_info, sizeof(s_boot_info));
    __DSB();                                                    /* C4 修复：确保 Flash 写入完成 */
    __ISB();
    NVIC_SystemReset();
}
