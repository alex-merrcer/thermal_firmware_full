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

#define APP_RUNNING_PARTITION  APP_CFG_RUNNING_PARTITION
#define APP_OTHER_PARTITION    APP_CFG_OTHER_PARTITION
#define APP_DEFAULT_VERSION    "1.0.2"
#define APP_BOOT_TRIES_MAX     3U
#define APP_OTA_UART_BAUD      115200U

#ifndef APP_FIRMWARE_VERSION
#define APP_FIRMWARE_VERSION   APP_DEFAULT_VERSION
#endif

const char g_app_embedded_version_marker[] =
    "IAPFWV1|" APP_CFG_VERSION_SLOT_TAG "|" APP_FIRMWARE_VERSION "|";

static volatile uint8_t s_trial_timer_count = 0U;
static volatile uint8_t s_trial_confirm_pending = 0U;
static volatile uint8_t s_trial_confirm_due = 0U;
static BootInfoTypeDef s_boot_info;

static void ota_service_tim4_init(void);
static uint32_t ota_service_get_apb1_timer_clock_hz(void);
static void trial_run_complete(void);
static int8_t app_version_compare(const char *left, const char *right);
static uint32_t BootInfo_Write(const BootInfoTypeDef *boot_info);
static void BootInfo_Read(BootInfoTypeDef *boot_info);

static uint32_t ota_service_get_apb1_timer_clock_hz(void)
{
    uint32_t ppre1_bits = RCC->CFGR & RCC_CFGR_PPRE1;
    uint32_t hclk_hz = SystemCoreClock;
    uint32_t pclk1_hz = hclk_hz;

    switch (ppre1_bits)
    {
    case RCC_CFGR_PPRE1_DIV2:
        pclk1_hz = hclk_hz / 2U;
        break;
    case RCC_CFGR_PPRE1_DIV4:
        pclk1_hz = hclk_hz / 4U;
        break;
    case RCC_CFGR_PPRE1_DIV8:
        pclk1_hz = hclk_hz / 8U;
        break;
    case RCC_CFGR_PPRE1_DIV16:
        pclk1_hz = hclk_hz / 16U;
        break;
    default:
        pclk1_hz = hclk_hz;
        break;
    }

    return (ppre1_bits == RCC_CFGR_PPRE1_DIV1) ? pclk1_hz : (pclk1_hz * 2U);
}

static void ota_service_tim4_init(void)
{
    TIM_TimeBaseInitTypeDef tim_time_base;
    NVIC_InitTypeDef nvic_init;
    uint32_t timer_clock_hz = ota_service_get_apb1_timer_clock_hz();
    uint32_t prescaler = timer_clock_hz / 10000UL;

    if (prescaler == 0U)
    {
        prescaler = 1U;
    }

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);
    TIM_Cmd(TIM4, DISABLE);
    TIM_DeInit(TIM4);

    /* TIM4 keeps a 1 s trial-confirm heartbeat and must be reconfigured after clock changes. */
    tim_time_base.TIM_Period = 10000U - 1U;
    tim_time_base.TIM_Prescaler = (uint16_t)(prescaler - 1U);
    tim_time_base.TIM_ClockDivision = TIM_CKD_DIV1;
    tim_time_base.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM4, &tim_time_base);

    TIM_ITConfig(TIM4, TIM_IT_Update, ENABLE);

    nvic_init.NVIC_IRQChannel = TIM4_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0;
    nvic_init.NVIC_IRQChannelSubPriority = 0;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init);

    TIM_Cmd(TIM4, ENABLE);
}

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
                s_trial_confirm_due = 1U;
            }
        }
    }
}

void ota_service_reconfigure_timebase(void)
{
    ota_service_tim4_init();
}

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

static uint8_t boot_info_version_is_valid(const char *version)
{
    uint32_t i = 0U;
    uint8_t dot_count = 0U;
    uint8_t has_digit = 0U;

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

static void boot_info_version_copy(char *target, uint32_t target_len, const char *source)
{
    uint32_t i = 0U;
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

static uint8_t boot_info_should_use_app_version(void)
{
    return (boot_info_version_is_valid(APP_FIRMWARE_VERSION) != 0U) ? 1U : 0U;
}

static char *boot_info_partition_version_ptr(BootInfoTypeDef *boot_info, uint32_t partition)
{
    if (partition == BOOT_INFO_PARTITION_APP2)
    {
        return boot_info->app2_version;
    }

    return boot_info->app1_version;
}

static const char *boot_info_get_partition_version(const BootInfoTypeDef *boot_info, uint32_t partition)
{
    if (partition == BOOT_INFO_PARTITION_APP2)
    {
        return boot_info->app2_version;
    }

    return boot_info->app1_version;
}

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

static uint32_t boot_info_compute_crc(const BootInfoTypeDef *boot_info)
{
    const uint8_t *data_start = 0;
    uint32_t data_len = 0U;

    if (boot_info == 0)
    {
        return 0U;
    }

    data_start = (const uint8_t *)&boot_info->boot_magic;
    data_len = (uint32_t)(((const uint8_t *)boot_info + sizeof(BootInfoTypeDef)) - data_start);
    return boot_info_crc32_update(0U, data_start, data_len);
}

static uint32_t boot_info_inactive_partition(uint32_t partition)
{
    return (partition == BOOT_INFO_PARTITION_APP2) ? BOOT_INFO_PARTITION_APP1 : BOOT_INFO_PARTITION_APP2;
}

static void boot_info_set_min_version(char *target, const char *left, const char *right, const char *third)
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
                           BOOT_INFO_VERSION_LEN,
                           running_version);
    boot_info->active_partition = APP_RUNNING_PARTITION;
    boot_info->confirmed_slot = APP_RUNNING_PARTITION;
    boot_info->target_partition = APP_OTHER_PARTITION;
    boot_info->trial_state = BOOT_INFO_TRIAL_NONE;
    boot_info->boot_magic = MAGIC_NORMAL;
    boot_info->upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
    boot_info->boot_tries = APP_BOOT_TRIES_MAX;
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

/* 手工重新烧录 APP 时，镜像本身会变，但 BootInfo 仍可能保留上一次 OTA 运行留下的版本号。
 * 这里在 APP 启动后把“当前正在运行的槽位版本”对齐到编译进镜像的真实版本，
 * 这样菜单显示、发给 ESP32 的 current_version、以及后续 OTA 决策就不会继续拿旧值。
 * 试运行阶段只修正当前槽位和 current_version，不去提前改 confirmed/last_good。 */
static uint8_t boot_info_reconcile_running_image(BootInfoTypeDef *boot_info)
{
    uint8_t modified = 0U;

    if (boot_info == 0 || boot_info_should_use_app_version() == 0U)
    {
        return 0U;
    }

    if (boot_info->active_partition != APP_RUNNING_PARTITION)
    {
        boot_info->active_partition = APP_RUNNING_PARTITION;
        modified = 1U;
    }

    if (boot_info->target_partition != APP_OTHER_PARTITION)
    {
        boot_info->target_partition = APP_OTHER_PARTITION;
        modified = 1U;
    }

    if (strcmp(boot_info_get_partition_version(boot_info, APP_RUNNING_PARTITION),
               APP_FIRMWARE_VERSION) != 0)
    {
        boot_info_version_copy(boot_info_partition_version_ptr(boot_info, APP_RUNNING_PARTITION),
                               BOOT_INFO_VERSION_LEN,
                               APP_FIRMWARE_VERSION);
        modified = 1U;
    }

    if (strcmp(boot_info->current_version, APP_FIRMWARE_VERSION) != 0)
    {
        boot_info_sync_current_version(boot_info);
        modified = 1U;
    }

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

static void boot_info_init_default(BootInfoTypeDef *boot_info)
{
    if (boot_info == 0)
    {
        return;
    }

    memset(boot_info, 0, sizeof(*boot_info));
    boot_info->layout_magic = BOOT_INFO_LAYOUT_MAGIC;
    boot_info->layout_version = BOOT_INFO_LAYOUT_VERSION;
    boot_info->struct_size = (uint16_t)sizeof(BootInfoTypeDef);
    boot_info->boot_magic = MAGIC_NORMAL;
    boot_info->upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
    boot_info->active_partition = APP_RUNNING_PARTITION;
    boot_info->target_partition = APP_OTHER_PARTITION;
    boot_info->confirmed_slot = APP_RUNNING_PARTITION;
    boot_info->trial_state = BOOT_INFO_TRIAL_NONE;
    boot_info->boot_tries = APP_BOOT_TRIES_MAX;
    boot_info->rollback_counter = 0U;
    boot_info_version_copy(boot_info->app1_version, sizeof(boot_info->app1_version), APP_DEFAULT_VERSION);
    boot_info_version_copy(boot_info->app2_version, sizeof(boot_info->app2_version), APP_DEFAULT_VERSION);
    boot_info_version_copy(boot_info->last_good_version, sizeof(boot_info->last_good_version), APP_DEFAULT_VERSION);
    boot_info_version_copy(boot_info->min_allowed_ota_version, sizeof(boot_info->min_allowed_ota_version), APP_DEFAULT_VERSION);
    boot_info_version_copy(boot_info->pending_floor_version, sizeof(boot_info->pending_floor_version), APP_DEFAULT_VERSION);

    if (boot_info_should_use_app_version() != 0U)
    {
        boot_info_set_partition_version(boot_info, APP_RUNNING_PARTITION, APP_FIRMWARE_VERSION);
    }
    else
    {
        boot_info_sync_current_version(boot_info);
    }
    boot_info_version_copy(boot_info->last_good_version, sizeof(boot_info->last_good_version), boot_info->current_version);

    boot_info->data_crc32 = boot_info_compute_crc(boot_info);
}

static uint8_t boot_info_is_valid(const BootInfoTypeDef *boot_info)
{
    if (boot_info == 0)
    {
        return 0U;
    }

    if (boot_info->layout_magic != BOOT_INFO_LAYOUT_MAGIC ||
        boot_info->layout_version != BOOT_INFO_LAYOUT_VERSION ||
        boot_info->struct_size != sizeof(BootInfoTypeDef))
    {
        return 0U;
    }

    if (boot_info->active_partition > BOOT_INFO_PARTITION_APP2 ||
        boot_info->target_partition > BOOT_INFO_PARTITION_APP2 ||
        boot_info->confirmed_slot > BOOT_INFO_PARTITION_APP2 ||
        boot_info->upgrade_flag > BOOT_UPGRADE_FLAG_ROLLBACK ||
        boot_info->boot_tries > APP_BOOT_TRIES_MAX ||
        boot_info->trial_state > BOOT_INFO_TRIAL_PENDING)
    {
        return 0U;
    }

    if (boot_info->boot_magic != MAGIC_NORMAL &&
        boot_info->boot_magic != MAGIC_REQUEST &&
        boot_info->boot_magic != MAGIC_NEW_FW)
    {
        return 0U;
    }

    if (boot_info_version_is_valid(boot_info->current_version) == 0U ||
        boot_info_version_is_valid(boot_info->app1_version) == 0U ||
        boot_info_version_is_valid(boot_info->app2_version) == 0U ||
        boot_info_version_is_valid(boot_info->last_good_version) == 0U ||
        boot_info_version_is_valid(boot_info->min_allowed_ota_version) == 0U ||
        boot_info_version_is_valid(boot_info->pending_floor_version) == 0U)
    {
        return 0U;
    }

    if (strcmp(boot_info->current_version,
               boot_info_get_partition_version(boot_info, boot_info->active_partition)) != 0)
    {
        return 0U;
    }

    if (boot_info->trial_state == BOOT_INFO_TRIAL_NONE &&
        boot_info->confirmed_slot != boot_info->active_partition)
    {
        return 0U;
    }

    return (boot_info->data_crc32 == boot_info_compute_crc(boot_info)) ? 1U : 0U;
}

#define APP_BOOT_JOURNAL_REGION_ADDR  BOOT_INFO_ADDR
#define APP_TXN_JOURNAL_REGION_ADDR   0x0800E000U
#define APP_JOURNAL_REGION_SIZE       0x2000U
#define APP_BOOTINFO_SECTOR_SIZE      (ADDR_FLASH_SECTOR_4 - BOOT_INFO_ADDR)
#define APP_TXN_REGION_OFFSET         (APP_TXN_JOURNAL_REGION_ADDR - APP_BOOT_JOURNAL_REGION_ADDR)
#define APP_JOURNAL_SLOT_SIZE         256U
#define APP_JOURNAL_SLOT_COUNT        (APP_JOURNAL_REGION_SIZE / APP_JOURNAL_SLOT_SIZE)
#define APP_JOURNAL_PAYLOAD_SIZE      236U
#define APP_JOURNAL_SLOT_VERSION      1U
#define APP_JOURNAL_COMMIT_MAGIC      0x434D4954UL
#define APP_BOOT_JOURNAL_MAGIC        0x424A4E4CUL
#define APP_TXN_JOURNAL_MAGIC         0x544A4E4CUL

static uint32_t s_app_boot_sector_backup[APP_BOOTINFO_SECTOR_SIZE / sizeof(uint32_t)];

typedef struct
{
    uint32_t slot_magic;
    uint16_t slot_version;
    uint16_t payload_size;
    uint32_t slot_seq;
    uint32_t payload_crc32;
    uint8_t payload[APP_JOURNAL_PAYLOAD_SIZE];
    uint32_t commit_magic;
} app_journal_slot_t;

typedef struct
{
    uint8_t has_valid;
    uint8_t has_empty;
    uint32_t latest_seq;
    uint32_t latest_addr;
    uint32_t empty_addr;
} app_journal_scan_t;

typedef char app_journal_slot_size_check[(sizeof(app_journal_slot_t) == APP_JOURNAL_SLOT_SIZE) ? 1 : -1];

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

static uint32_t app_journal_seq_next(uint32_t current)
{
    uint32_t next = current + 1U;

    if (next == 0U || next == 0xFFFFFFFFUL)
    {
        next = 1U;
    }

    return next;
}

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

    return (boot_info_crc32_update(0U, slot->payload, slot->payload_size) == slot->payload_crc32) ? 1U : 0U;
}

static uint8_t app_journal_slot_is_boot_info_valid(const app_journal_slot_t *slot)
{
    if (app_journal_slot_is_structurally_valid(slot, APP_BOOT_JOURNAL_MAGIC) == 0U ||
        slot->payload_size != sizeof(BootInfoTypeDef))
    {
        return 0U;
    }

    return boot_info_is_valid((const BootInfoTypeDef *)slot->payload);
}

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

        if (app_journal_slot_is_erased(slot) != 0U)
        {
            if (scan->has_empty == 0U)
            {
                scan->has_empty = 1U;
                scan->empty_addr = APP_BOOT_JOURNAL_REGION_ADDR + (index * APP_JOURNAL_SLOT_SIZE);
            }
            continue;
        }

        if (app_journal_slot_is_boot_info_valid(slot) != 0U)
        {
            if (scan->has_valid == 0U || app_journal_seq_is_newer(slot->slot_seq, scan->latest_seq) != 0U)
            {
                scan->has_valid = 1U;
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

static void app_journal_build_boot_slot(app_journal_slot_t *slot,
                                        uint32_t sequence,
                                        const BootInfoTypeDef *boot_info)
{
    memset(slot, 0xFF, sizeof(*slot));
    slot->slot_magic = APP_BOOT_JOURNAL_MAGIC;
    slot->slot_version = APP_JOURNAL_SLOT_VERSION;
    slot->payload_size = (uint16_t)sizeof(BootInfoTypeDef);
    slot->slot_seq = sequence;
    memcpy(slot->payload, boot_info, sizeof(BootInfoTypeDef));
    slot->payload_crc32 = boot_info_crc32_update(0U, slot->payload, slot->payload_size);
    slot->commit_magic = APP_JOURNAL_COMMIT_MAGIC;
}

static uint32_t app_journal_write_slot(uint32_t slot_addr, const app_journal_slot_t *slot)
{
    uint32_t flash_addr = slot_addr;

    return FLASH_If_Write(&flash_addr,
                          (uint32_t *)(void *)slot,
                          APP_JOURNAL_SLOT_SIZE / sizeof(uint32_t));
}

static uint8_t app_boot_info_load_current(BootInfoTypeDef *boot_info)
{
    app_journal_scan_t scan;

    app_journal_scan_boot(&scan, boot_info);
    if (scan.has_valid != 0U)
    {
        return 1U;
    }

    memcpy(boot_info, (const void *)BOOT_INFO_ADDR, sizeof(*boot_info));
    return (boot_info_is_valid(boot_info) != 0U) ? 1U : 0U;
}

static uint32_t BootInfo_Write(const BootInfoTypeDef *boot_info)
{
    BootInfoTypeDef prepared;
    app_journal_scan_t scan;
    app_journal_slot_t slot;
    uint32_t next_sequence = 1U;
    uint32_t slot_addr = 0U;

    if (boot_info == 0)
    {
        return 1U;
    }

    prepared = *boot_info;
    prepared.layout_magic = BOOT_INFO_LAYOUT_MAGIC;
    prepared.layout_version = BOOT_INFO_LAYOUT_VERSION;
    prepared.struct_size = (uint16_t)sizeof(BootInfoTypeDef);
    prepared.data_crc32 = boot_info_compute_crc(&prepared);

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                    FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    app_journal_scan_boot(&scan, 0);
    if (scan.has_valid != 0U)
    {
        next_sequence = app_journal_seq_next(scan.latest_seq);
    }

    if (scan.has_empty != 0U)
    {
        slot_addr = scan.empty_addr;
    }
    else
    {
        uint32_t index = 0U;
        uint32_t txn_flash_addr = APP_TXN_JOURNAL_REGION_ADDR;

        memcpy(s_app_boot_sector_backup,
               (const void *)APP_BOOT_JOURNAL_REGION_ADDR,
               APP_BOOTINFO_SECTOR_SIZE);

        if (MY_FLASH_Erase(APP_BOOT_JOURNAL_REGION_ADDR) != 0U)
        {
            FLASH_Lock();
            return 1U;
        }

        slot_addr = APP_BOOT_JOURNAL_REGION_ADDR;
        for (index = 0U; index < APP_JOURNAL_SLOT_COUNT; ++index)
        {
            app_journal_slot_t *existing_slot =
                (app_journal_slot_t *)((uint8_t *)s_app_boot_sector_backup + (index * APP_JOURNAL_SLOT_SIZE));

            if (app_journal_slot_is_boot_info_valid(existing_slot) == 0U)
            {
                continue;
            }

            if (scan.latest_addr == (APP_BOOT_JOURNAL_REGION_ADDR + (index * APP_JOURNAL_SLOT_SIZE)))
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

        if (FLASH_If_Write(&txn_flash_addr,
                           &s_app_boot_sector_backup[APP_TXN_REGION_OFFSET / sizeof(uint32_t)],
                           APP_JOURNAL_REGION_SIZE / sizeof(uint32_t)) != 0U)
        {
            FLASH_Lock();
            return 1U;
        }
    }

    app_journal_build_boot_slot(&slot, next_sequence, &prepared);
    if (app_journal_write_slot(slot_addr, &slot) != 0U)
    {
        FLASH_Lock();
        return 1U;
    }

    FLASH_Lock();
    return 0U;
}

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

static uint8_t app_version_read_component(const char **cursor, uint32_t *value)
{
    const char *ptr = 0;
    uint32_t result = 0U;
    uint8_t has_digit = 0U;

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
    *value = result;
    return 1U;
}

static int8_t app_version_compare(const char *left, const char *right)
{
    const char *left_ptr = left;
    const char *right_ptr = right;
    uint32_t left_value = 0U;
    uint32_t right_value = 0U;
    uint8_t index = 0U;

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

        if (left_value > right_value)
        {
            return 1;
        }
        if (left_value < right_value)
        {
            return -1;
        }

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

const char *ota_service_reason_text(uint16_t reason)
{
    switch (reason)
    {
    case OTA_CTRL_ERR_BUSY:
        return "ESP32 busy";
    case OTA_CTRL_ERR_NO_WIFI:
        return "No WiFi";
    case OTA_CTRL_ERR_FETCH_METADATA:
        return "Meta failed";
    case OTA_CTRL_ERR_NO_PACKAGE:
        return "No package";
    case OTA_CTRL_ERR_PRODUCT:
        return "Product err";
    case OTA_CTRL_ERR_HW_REV:
        return "HW rev err";
    case OTA_CTRL_ERR_PROTOCOL:
        return "Protocol err";
    case OTA_CTRL_ERR_PARTITION:
        return "Partition err";
    case OTA_CTRL_ERR_VERSION:
        return "Version err";
    case OTA_CTRL_ERR_NO_UPDATE:
        return "No update";
    default:
        return "UART timeout";
    }
}

static void trial_run_complete(void)
{
    BootInfoTypeDef boot_info;

    BootInfo_Read(&boot_info);
    boot_info_mark_confirmed(&boot_info);
    BootInfo_Write(&boot_info);
    BootInfo_Read(&s_boot_info);
}

static void ota_service_reload_boot_info(void)
{
    BootInfo_Read(&s_boot_info);

    if (boot_info_reconcile_running_image(&s_boot_info) != 0U)
    {
        BootInfo_Write(&s_boot_info);
        BootInfo_Read(&s_boot_info);
    }
}

void ota_service_init(void)
{
    volatile const void *marker_anchor = g_app_embedded_version_marker;

    (void)marker_anchor;
    ota_service_reload_boot_info();

    if (s_boot_info.boot_magic == MAGIC_NEW_FW &&
        s_boot_info.trial_state == BOOT_INFO_TRIAL_PENDING)
    {
        s_trial_confirm_pending = 1U;
        s_trial_timer_count = 0U;
        s_trial_confirm_due = 0U;
    }
    else
    {
        s_trial_confirm_pending = 0U;
        s_trial_timer_count = 0U;
        s_trial_confirm_due = 0U;
    }

    ota_service_tim4_init();
    __enable_irq();
}

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

void ota_service_refresh_info(void)
{
    ota_service_reload_boot_info();
}

void ota_service_load_boot_info_copy(BootInfoTypeDef *boot_info)
{
    BootInfo_Read(boot_info);
}

uint32_t ota_service_store_boot_info(const BootInfoTypeDef *boot_info)
{
    uint32_t status = BootInfo_Write(boot_info);

    if (status == 0U)
    {
        BootInfo_Read(&s_boot_info);
    }

    return status;
}

const BootInfoTypeDef *ota_service_get_boot_info(void)
{
    return &s_boot_info;
}

const char *ota_service_get_display_version(void)
{
    return boot_info_get_display_version_internal(&s_boot_info);
}

const char *ota_service_get_partition_version(uint32_t partition)
{
    return boot_info_get_partition_version(&s_boot_info, partition);
}

const char *ota_service_get_partition_name(uint32_t partition)
{
    return (partition == BOOT_INFO_PARTITION_APP2) ? "APP2" : "APP1";
}

uint32_t ota_service_get_active_partition(void)
{
    return s_boot_info.active_partition;
}

uint32_t ota_service_get_inactive_partition(void)
{
    return boot_info_inactive_partition(s_boot_info.active_partition);
}

int8_t ota_service_compare_version(const char *left, const char *right)
{
    return app_version_compare(left, right);
}

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
    if (uart_get_current_baud() != APP_OTA_UART_BAUD)
    {
        uart_init(APP_OTA_UART_BAUD);
    }

    return iap_query_latest_version(&s_boot_info,
                                    latest_version,
                                    latest_version_len,
                                    reject_reason);
}

void ota_service_request_upgrade(void)
{
    BootInfoTypeDef boot_info;

    BootInfo_Read(&boot_info);
    boot_info.boot_magic = MAGIC_REQUEST;
    boot_info.upgrade_flag = BOOT_UPGRADE_FLAG_UPGRADE;
    BootInfo_Write(&boot_info);
    memcpy(&s_boot_info, &boot_info, sizeof(s_boot_info));
    NVIC_SystemReset();
}

void ota_service_request_rollback(void)
{
    BootInfoTypeDef boot_info;

    BootInfo_Read(&boot_info);
    boot_info.boot_magic = MAGIC_REQUEST;
    boot_info.upgrade_flag = BOOT_UPGRADE_FLAG_ROLLBACK;
    BootInfo_Write(&boot_info);
    memcpy(&s_boot_info, &boot_info, sizeof(s_boot_info));
    NVIC_SystemReset();
}
