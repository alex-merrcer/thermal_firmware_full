#include "iap_boot_info.h"

#undef active_partition
#undef target_partition
#undef app1_version
#undef app2_version

typedef struct
{
    uint32_t magic;
    uint32_t boot_requested;
    uint32_t active_partition;
    uint32_t target_partition;
    uint32_t boot_tries;
    uint32_t trial_complete;
} legacy_boot_info_t;

#define BOOT_INFO_JOURNAL_REGION_ADDR BOOT_INFO_ADDR
#define TXN_JOURNAL_REGION_ADDR       0x0800E000U
#define IAP_JOURNAL_REGION_SIZE       0x2000U
#define IAP_JOURNAL_SLOT_SIZE         256U
#define IAP_JOURNAL_SLOT_COUNT        (IAP_JOURNAL_REGION_SIZE / IAP_JOURNAL_SLOT_SIZE)
#define IAP_JOURNAL_PAYLOAD_SIZE      236U
#define IAP_JOURNAL_SLOT_VERSION      1U
#define IAP_JOURNAL_COMMIT_MAGIC      0x434D4954UL
#define BOOT_INFO_JOURNAL_MAGIC       0x424A4E4CUL
#define TXN_JOURNAL_MAGIC             0x544A4E4CUL

typedef struct
{
    uint32_t slot_magic;
    uint16_t slot_version;
    uint16_t payload_size;
    uint32_t slot_seq;
    uint32_t payload_crc32;
    uint8_t payload[IAP_JOURNAL_PAYLOAD_SIZE];
    uint32_t commit_magic;
} iap_journal_slot_t;

typedef struct
{
    uint8_t has_valid;
    uint8_t has_empty;
    uint32_t latest_seq;
    uint32_t latest_addr;
    uint32_t empty_addr;
    uint32_t programmed_slots;
    uint32_t invalid_slots;
} iap_journal_scan_t;

typedef enum
{
    BOOT_INFO_LOAD_SOURCE_JOURNAL = 0U,
    BOOT_INFO_LOAD_SOURCE_V3_LEGACY = 1U,
    BOOT_INFO_LOAD_SOURCE_V2_LEGACY = 2U,
    BOOT_INFO_LOAD_SOURCE_V1_LEGACY = 3U,
    BOOT_INFO_LOAD_SOURCE_DEFAULT = 4U
} boot_info_load_source_t;

typedef uint8_t (*iap_payload_validator_t)(const void *payload);

typedef enum
{
    IAP_JOURNAL_SLOT_VALID = 0U,
    IAP_JOURNAL_SLOT_INVALID_HEADER = 1U,
    IAP_JOURNAL_SLOT_INVALID_COMMIT = 2U,
    IAP_JOURNAL_SLOT_INVALID_PAYLOAD_CRC = 3U,
    IAP_JOURNAL_SLOT_INVALID_PAYLOAD_VALIDATE = 4U
} iap_journal_slot_validate_result_t;

typedef enum
{
    IAP_TXN_VALIDATE_OK = 0U,
    IAP_TXN_VALIDATE_LAYOUT = 1U,
    IAP_TXN_VALIDATE_STATE = 2U,
    IAP_TXN_VALIDATE_PARTITION = 3U,
    IAP_TXN_VALIDATE_FIELDS = 4U,
    IAP_TXN_VALIDATE_REQUEST_TYPE = 5U,
    IAP_TXN_VALIDATE_VERSION_TEXT = 6U,
    IAP_TXN_VALIDATE_OFFSET = 7U,
    IAP_TXN_VALIDATE_DATA_CRC = 8U
} iap_txn_validate_reason_t;

static uint8_t txn_payload_is_valid(const void *payload);

typedef char iap_journal_slot_size_check[(sizeof(iap_journal_slot_t) == IAP_JOURNAL_SLOT_SIZE) ? 1 : -1];

static uint32_t iap_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length)
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

static uint8_t boot_version_field_is_valid(const char *field, uint32_t field_len)
{
    uint32_t i = 0U;
    uint8_t dot_count = 0U;
    uint8_t has_digit = 0U;

    if (field == 0 || field_len == 0U)
    {
        return 0U;
    }

    for (i = 0U; i < field_len; ++i)
    {
        char ch = field[i];

        if (ch == '\0')
        {
            return (has_digit != 0U && dot_count == 2U) ? 1U : 0U;
        }

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

            ++dot_count;
            has_digit = 0U;
            continue;
        }

        return 0U;
    }

    return 0U;
}

static uint8_t version_read_component(const char **cursor, uint32_t *value)
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
        ++ptr;
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

static char *boot_info_partition_version_ptr(BootInfoTypeDef *info, uint32_t partition)
{
    if (info == 0)
    {
        return 0;
    }

    if (partition >= BOOT_INFO_SLOT_COUNT)
    {
        return info->slot_versions[BOOT_INFO_SLOT_APP1];
    }

    return info->slot_versions[partition];
}

static uint32_t boot_info_compute_crc(const BootInfoTypeDef *info)
{
    const uint8_t *data_start = 0;
    uint32_t data_len = 0U;

    if (info == 0)
    {
        return 0U;
    }

    data_start = (const uint8_t *)&info->boot_magic;
    data_len = (uint32_t)(((const uint8_t *)info + sizeof(BootInfoTypeDef)) - data_start);
    return iap_crc32_update(0U, data_start, data_len);
}

static uint32_t boot_info_compute_crc_v2(const BootInfoV2Legacy *info)
{
    const uint8_t *data_start = 0;
    uint32_t data_len = 0U;

    if (info == 0)
    {
        return 0U;
    }

    data_start = (const uint8_t *)&info->boot_magic;
    data_len = (uint32_t)(((const uint8_t *)info + sizeof(BootInfoV2Legacy)) - data_start);
    return iap_crc32_update(0U, data_start, data_len);
}

static uint32_t txn_compute_crc(const OtaTxnRecord *txn)
{
    const uint8_t *data_start = 0;
    uint32_t data_len = 0U;

    if (txn == 0)
    {
        return 0U;
    }

    data_start = (const uint8_t *)&txn->txn_counter;
    data_len = (uint32_t)(((const uint8_t *)txn + sizeof(OtaTxnRecord)) - data_start);
    return iap_crc32_update(0U, data_start, data_len);
}

void txn_init_default(OtaTxnRecord *txn)
{
    if (txn == 0)
    {
        return;
    }

    memset(txn, 0, sizeof(*txn));
    txn->layout_magic = IAP_TXN_LAYOUT_MAGIC;
    txn->layout_version = IAP_TXN_LAYOUT_VERSION;
    txn->struct_size = (uint16_t)sizeof(OtaTxnRecord);
    txn->txn_counter = 0U;
    txn->state = IAP_TXN_STATE_IDLE;
    txn->request_type = OTA_CTRL_REQ_TYPE_UPGRADE;
    txn->active_slot = OTA_CTRL_PARTITION_APP1;
    txn->target_slot = OTA_CTRL_PARTITION_APP2;
    version_text_copy(txn->current_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    version_text_copy(txn->target_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    txn->last_error_stage = 0U;
    txn->last_error_code = 0U;
    txn->retryable = 0U;
    txn->transfer_total_size = 0U;
    txn->protocol_version = OTA_CTRL_PROTOCOL_VERSION;
    txn->checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    txn->chunk_size = OTA_DATA_DEFAULT_CHUNK_SIZE;
    txn->plain_size = 0U;
    txn->resume_offset = 0U;
    txn->last_acked_offset = 0U;
    txn->pause_reason = IAP_TXN_PAUSE_NONE;
    txn->data_crc32 = txn_compute_crc(txn);
}

static void txn_prepare_for_store(OtaTxnRecord *txn)
{
    if (txn == 0)
    {
        return;
    }

    txn->layout_magic = IAP_TXN_LAYOUT_MAGIC;
    txn->layout_version = IAP_TXN_LAYOUT_VERSION;
    txn->struct_size = (uint16_t)sizeof(OtaTxnRecord);

    if (txn->state > IAP_TXN_STATE_FAILED_TERMINAL)
    {
        txn->state = IAP_TXN_STATE_IDLE;
    }

    if (txn->request_type != OTA_CTRL_REQ_TYPE_UPGRADE &&
        txn->request_type != OTA_CTRL_REQ_TYPE_ROLLBACK)
    {
        txn->request_type = OTA_CTRL_REQ_TYPE_UPGRADE;
    }

    if (txn->active_slot > OTA_CTRL_PARTITION_APP2)
    {
        txn->active_slot = OTA_CTRL_PARTITION_APP1;
    }

    if (txn->target_slot > OTA_CTRL_PARTITION_APP2 ||
        txn->target_slot == txn->active_slot)
    {
        txn->target_slot = boot_info_inactive_partition(txn->active_slot);
    }

    version_text_copy(txn->current_version, BOOT_INFO_VERSION_LEN, txn->current_version);
    version_text_copy(txn->target_version, BOOT_INFO_VERSION_LEN, txn->target_version);
    txn->retryable = (txn->retryable != 0U) ? 1U : 0U;
    if (txn->protocol_version == 0U)
    {
        txn->protocol_version = OTA_CTRL_PROTOCOL_VERSION;
    }
    if (txn->chunk_size == 0U || txn->chunk_size > OTA_DATA_MAX_PAYLOAD_LEN)
    {
        txn->chunk_size = OTA_DATA_DEFAULT_CHUNK_SIZE;
    }
    if (txn->checkpoint_size == 0U)
    {
        txn->checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    }
    if (txn->last_acked_offset < txn->resume_offset)
    {
        txn->last_acked_offset = txn->resume_offset;
    }
    if (txn->pause_reason > IAP_TXN_PAUSE_TERMINAL)
    {
        txn->pause_reason = IAP_TXN_PAUSE_NONE;
    }

    txn->data_crc32 = txn_compute_crc(txn);
}

static iap_txn_validate_reason_t txn_validate_reason(const OtaTxnRecord *txn)
{
    if (txn == 0)
    {
        return IAP_TXN_VALIDATE_LAYOUT;
    }

    if (txn->layout_magic != IAP_TXN_LAYOUT_MAGIC ||
        txn->layout_version != IAP_TXN_LAYOUT_VERSION ||
        txn->struct_size != sizeof(OtaTxnRecord))
    {
        return IAP_TXN_VALIDATE_LAYOUT;
    }

    if (txn->state > IAP_TXN_STATE_FAILED_TERMINAL)
    {
        return IAP_TXN_VALIDATE_STATE;
    }

    if (txn->active_slot > OTA_CTRL_PARTITION_APP2 ||
        txn->target_slot > OTA_CTRL_PARTITION_APP2 ||
        txn->target_slot == txn->active_slot)
    {
        return IAP_TXN_VALIDATE_PARTITION;
    }

    if (txn->retryable > 1U ||
        txn->pause_reason > IAP_TXN_PAUSE_TERMINAL ||
        txn->chunk_size > OTA_DATA_MAX_PAYLOAD_LEN)
    {
        return IAP_TXN_VALIDATE_FIELDS;
    }

    if (txn->request_type != OTA_CTRL_REQ_TYPE_UPGRADE &&
        txn->request_type != OTA_CTRL_REQ_TYPE_ROLLBACK)
    {
        return IAP_TXN_VALIDATE_REQUEST_TYPE;
    }

    if (boot_version_field_is_valid(txn->current_version, sizeof(txn->current_version)) == 0U ||
        boot_version_field_is_valid(txn->target_version, sizeof(txn->target_version)) == 0U)
    {
        return IAP_TXN_VALIDATE_VERSION_TEXT;
    }

    if (txn->last_acked_offset < txn->resume_offset)
    {
        return IAP_TXN_VALIDATE_OFFSET;
    }

    if (txn->data_crc32 != txn_compute_crc(txn))
    {
        return IAP_TXN_VALIDATE_DATA_CRC;
    }

    return IAP_TXN_VALIDATE_OK;
}

static uint8_t txn_is_valid(const OtaTxnRecord *txn)
{
    return (txn_validate_reason(txn) == IAP_TXN_VALIDATE_OK) ? 1U : 0U;
}

static uint8_t journal_seq_is_newer(uint32_t candidate, uint32_t current)
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

static uint32_t journal_seq_next(uint32_t current)
{
    uint32_t next = current + 1U;

    if (next == 0U || next == 0xFFFFFFFFUL)
    {
        next = 1U;
    }

    return next;
}

static uint8_t journal_slot_is_erased(const iap_journal_slot_t *slot)
{
    const uint32_t *words = (const uint32_t *)slot;
    uint32_t index = 0U;

    if (slot == 0)
    {
        return 0U;
    }

    for (index = 0U; index < (IAP_JOURNAL_SLOT_SIZE / sizeof(uint32_t)); ++index)
    {
        if (words[index] != 0xFFFFFFFFUL)
        {
            return 0U;
        }
    }

    return 1U;
}

static iap_journal_slot_validate_result_t journal_slot_validate_reason(const iap_journal_slot_t *slot,
                                                                       uint32_t expected_magic,
                                                                       uint16_t payload_size,
                                                                       iap_payload_validator_t payload_validator)
{
    if (slot == 0)
    {
        return IAP_JOURNAL_SLOT_INVALID_HEADER;
    }

    if (slot->commit_magic != IAP_JOURNAL_COMMIT_MAGIC)
    {
        return IAP_JOURNAL_SLOT_INVALID_COMMIT;
    }

    if (slot->slot_magic != expected_magic ||
        slot->slot_version != IAP_JOURNAL_SLOT_VERSION ||
        slot->payload_size != payload_size ||
        slot->slot_seq == 0U ||
        slot->slot_seq == 0xFFFFFFFFUL)
    {
        return IAP_JOURNAL_SLOT_INVALID_HEADER;
    }

    if (iap_crc32_update(0U, slot->payload, payload_size) != slot->payload_crc32)
    {
        return IAP_JOURNAL_SLOT_INVALID_PAYLOAD_CRC;
    }

    if (payload_validator != 0 && payload_validator((const void *)slot->payload) == 0U)
    {
        return IAP_JOURNAL_SLOT_INVALID_PAYLOAD_VALIDATE;
    }

    return IAP_JOURNAL_SLOT_VALID;
}

static uint8_t journal_slot_is_valid(const iap_journal_slot_t *slot,
                                     uint32_t expected_magic,
                                     uint16_t payload_size,
                                     iap_payload_validator_t payload_validator)
{
    return (journal_slot_validate_reason(slot,
                                         expected_magic,
                                         payload_size,
                                         payload_validator) == IAP_JOURNAL_SLOT_VALID) ? 1U : 0U;
}

static void txn_diag_record_validate_reason(iap_txn_load_diag_t *diag, const OtaTxnRecord *txn)
{
    if (diag == 0 || txn == 0)
    {
        return;
    }

    switch (txn_validate_reason(txn))
    {
    case IAP_TXN_VALIDATE_LAYOUT:
        diag->invalid_txn_layout++;
        break;
    case IAP_TXN_VALIDATE_STATE:
        diag->invalid_txn_state++;
        break;
    case IAP_TXN_VALIDATE_PARTITION:
        diag->invalid_txn_partition++;
        break;
    case IAP_TXN_VALIDATE_FIELDS:
        diag->invalid_txn_fields++;
        break;
    case IAP_TXN_VALIDATE_REQUEST_TYPE:
        diag->invalid_txn_request_type++;
        break;
    case IAP_TXN_VALIDATE_VERSION_TEXT:
        diag->invalid_txn_version_text++;
        break;
    case IAP_TXN_VALIDATE_OFFSET:
        diag->invalid_txn_offset++;
        break;
    case IAP_TXN_VALIDATE_DATA_CRC:
        diag->invalid_txn_data_crc++;
        break;
    default:
        break;
    }
}

static void txn_diag_record_slot_reason(iap_txn_load_diag_t *diag,
                                        iap_journal_slot_validate_result_t reason,
                                        const iap_journal_slot_t *slot)
{
    if (diag == 0)
    {
        return;
    }

    switch (reason)
    {
    case IAP_JOURNAL_SLOT_INVALID_HEADER:
        diag->invalid_slot_header++;
        break;
    case IAP_JOURNAL_SLOT_INVALID_COMMIT:
        diag->invalid_slot_commit++;
        break;
    case IAP_JOURNAL_SLOT_INVALID_PAYLOAD_CRC:
        diag->invalid_slot_payload_crc++;
        break;
    case IAP_JOURNAL_SLOT_INVALID_PAYLOAD_VALIDATE:
        diag->invalid_slot_payload_validate++;
        if (slot != 0)
        {
            txn_diag_record_validate_reason(diag, (const OtaTxnRecord *)(const void *)slot->payload);
        }
        break;
    default:
        break;
    }
}

static void journal_scan_region(uint32_t region_addr,
                                uint32_t expected_magic,
                                uint16_t payload_size,
                                iap_payload_validator_t payload_validator,
                                void *latest_payload,
                                iap_journal_scan_t *scan)
{
    uint32_t index = 0U;

    if (scan == 0)
    {
        return;
    }

    memset(scan, 0, sizeof(*scan));

    for (index = 0U; index < IAP_JOURNAL_SLOT_COUNT; ++index)
    {
        uint32_t slot_addr = region_addr + (index * IAP_JOURNAL_SLOT_SIZE);
        const iap_journal_slot_t *slot = (const iap_journal_slot_t *)slot_addr;

        if (journal_slot_is_erased(slot) != 0U)
        {
            if (scan->has_empty == 0U)
            {
                scan->has_empty = 1U;
                scan->empty_addr = slot_addr;
            }
            continue;
        }

        scan->programmed_slots++;
        if (journal_slot_is_valid(slot, expected_magic, payload_size, payload_validator) == 0U)
        {
            scan->invalid_slots++;
            continue;
        }

        if (scan->has_valid == 0U || journal_seq_is_newer(slot->slot_seq, scan->latest_seq) != 0U)
        {
            scan->has_valid = 1U;
            scan->latest_seq = slot->slot_seq;
            scan->latest_addr = slot_addr;
            if (latest_payload != 0)
            {
                memcpy(latest_payload, slot->payload, payload_size);
            }
        }
    }
}

static void txn_scan_region(OtaTxnRecord *latest_payload,
                            iap_journal_scan_t *scan,
                            iap_txn_load_diag_t *diag)
{
    uint32_t index = 0U;

    if (scan == 0)
    {
        return;
    }

    memset(scan, 0, sizeof(*scan));

    for (index = 0U; index < IAP_JOURNAL_SLOT_COUNT; ++index)
    {
        uint32_t slot_addr = TXN_JOURNAL_REGION_ADDR + (index * IAP_JOURNAL_SLOT_SIZE);
        const iap_journal_slot_t *slot = (const iap_journal_slot_t *)slot_addr;
        iap_journal_slot_validate_result_t slot_reason = IAP_JOURNAL_SLOT_VALID;

        if (journal_slot_is_erased(slot) != 0U)
        {
            if (scan->has_empty == 0U)
            {
                scan->has_empty = 1U;
                scan->empty_addr = slot_addr;
            }
            continue;
        }

        scan->programmed_slots++;
        slot_reason = journal_slot_validate_reason(slot,
                                                   TXN_JOURNAL_MAGIC,
                                                   (uint16_t)sizeof(OtaTxnRecord),
                                                   txn_payload_is_valid);
        if (slot_reason != IAP_JOURNAL_SLOT_VALID)
        {
            scan->invalid_slots++;
            txn_diag_record_slot_reason(diag, slot_reason, slot);
            continue;
        }

        if (scan->has_valid == 0U || journal_seq_is_newer(slot->slot_seq, scan->latest_seq) != 0U)
        {
            scan->has_valid = 1U;
            scan->latest_seq = slot->slot_seq;
            scan->latest_addr = slot_addr;
            if (latest_payload != 0)
            {
                memcpy(latest_payload, slot->payload, sizeof(OtaTxnRecord));
            }
        }
    }
}

static void journal_build_slot(iap_journal_slot_t *slot,
                               uint32_t slot_magic,
                               uint16_t payload_size,
                               uint32_t slot_seq,
                               const void *payload)
{
    memset(slot, 0xFF, sizeof(*slot));
    slot->slot_magic = slot_magic;
    slot->slot_version = IAP_JOURNAL_SLOT_VERSION;
    slot->payload_size = payload_size;
    slot->slot_seq = slot_seq;
    slot->payload_crc32 = iap_crc32_update(0U, (const uint8_t *)payload, payload_size);
    memcpy(slot->payload, payload, payload_size);
    slot->commit_magic = IAP_JOURNAL_COMMIT_MAGIC;
}

static uint32_t journal_write_slot(uint32_t slot_addr, const iap_journal_slot_t *slot)
{
    uint32_t write_addr = slot_addr;
    uint32_t word_len = (IAP_JOURNAL_SLOT_SIZE - sizeof(uint32_t)) / sizeof(uint32_t);
    uint32_t commit_value = 0U;

    if (slot == 0)
    {
        return 1U;
    }

    if (FLASH_If_Write(&write_addr, (uint32_t *)(void *)slot, word_len) != 0U)
    {
        return 1U;
    }

    commit_value = slot->commit_magic;
    if (FLASH_If_Write(&write_addr, &commit_value, 1U) != 0U)
    {
        return 1U;
    }

    return 0U;
}

static uint32_t journal_rewrite_sector(const BootInfoTypeDef *boot_info,
                                       uint32_t boot_seq,
                                       const OtaTxnRecord *txn,
                                       uint32_t txn_seq)
{
    iap_journal_slot_t boot_slot;
    iap_journal_slot_t txn_slot;

    if (boot_info == 0 || txn == 0)
    {
        return 1U;
    }

    journal_build_slot(&boot_slot,
                       BOOT_INFO_JOURNAL_MAGIC,
                       (uint16_t)sizeof(BootInfoTypeDef),
                       boot_seq,
                       boot_info);
    journal_build_slot(&txn_slot,
                       TXN_JOURNAL_MAGIC,
                       (uint16_t)sizeof(OtaTxnRecord),
                       txn_seq,
                       txn);

    FLASH_Unlock();
    if (MY_FLASH_Erase(BOOT_INFO_JOURNAL_REGION_ADDR) != 0U)
    {
        FLASH_Lock();
        return 1U;
    }
    FLASH_Lock();

    if (journal_write_slot(BOOT_INFO_JOURNAL_REGION_ADDR, &boot_slot) != 0U)
    {
        return 1U;
    }

    if (journal_write_slot(TXN_JOURNAL_REGION_ADDR, &txn_slot) != 0U)
    {
        return 1U;
    }

    return 0U;
}

static void boot_info_set_min_version(char *target,
                                      const char *left,
                                      const char *right,
                                      const char *third)
{
    const char *candidate = left;

    if (version_text_compare(right, candidate) > 0)
    {
        candidate = right;
    }

    if (version_text_compare(third, candidate) > 0)
    {
        candidate = third;
    }

    version_text_copy(target, BOOT_INFO_VERSION_LEN, candidate);
}

static void boot_info_init_default(BootInfoTypeDef *info)
{
    if (info == 0)
    {
        return;
    }

    memset(info, 0, sizeof(*info));
    info->layout_magic = BOOT_INFO_LAYOUT_MAGIC;
    info->layout_version = BOOT_INFO_LAYOUT_VERSION;
    info->struct_size = (uint16_t)sizeof(BootInfoTypeDef);
    info->boot_magic = MAGIC_NORMAL;
    info->upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
    info->active_slot = BOOT_INFO_PARTITION_APP2;
    info->target_slot = BOOT_INFO_PARTITION_APP1;
    info->confirmed_slot = BOOT_INFO_PARTITION_APP2;
    info->trial_state = BOOT_INFO_TRIAL_NONE;
    info->boot_tries = IAP_MAX_BOOT_TRIES;
    info->rollback_counter = 0U;
    version_text_copy(info->slot_versions[BOOT_INFO_SLOT_APP1], BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    version_text_copy(info->slot_versions[BOOT_INFO_SLOT_APP2], BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    version_text_copy(info->last_good_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    version_text_copy(info->min_allowed_ota_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    version_text_copy(info->pending_floor_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    boot_info_sync_current_version(info);
    version_text_copy(info->last_good_version, BOOT_INFO_VERSION_LEN, info->current_version);
    info->data_crc32 = boot_info_compute_crc(info);
}

static uint8_t boot_info_is_valid(const BootInfoTypeDef *info)
{
    if (info == 0)
    {
        return 0U;
    }

    if (info->layout_magic != BOOT_INFO_LAYOUT_MAGIC ||
        info->layout_version != BOOT_INFO_LAYOUT_VERSION ||
        info->struct_size != sizeof(BootInfoTypeDef))
    {
        return 0U;
    }

    if (info->active_slot > OTA_CTRL_PARTITION_APP2 ||
        info->target_slot > OTA_CTRL_PARTITION_APP2 ||
        info->confirmed_slot > OTA_CTRL_PARTITION_APP2 ||
        info->upgrade_flag > BOOT_UPGRADE_FLAG_ROLLBACK ||
        info->boot_tries > IAP_MAX_BOOT_TRIES ||
        info->trial_state > BOOT_INFO_TRIAL_PENDING)
    {
        return 0U;
    }

    if (info->boot_magic != MAGIC_NORMAL &&
        info->boot_magic != MAGIC_REQUEST &&
        info->boot_magic != MAGIC_NEW_FW)
    {
        return 0U;
    }

    if (boot_version_field_is_valid(info->current_version, sizeof(info->current_version)) == 0U ||
        boot_version_field_is_valid(info->slot_versions[BOOT_INFO_SLOT_APP1], BOOT_INFO_VERSION_LEN) == 0U ||
        boot_version_field_is_valid(info->slot_versions[BOOT_INFO_SLOT_APP2], BOOT_INFO_VERSION_LEN) == 0U ||
        boot_version_field_is_valid(info->last_good_version, sizeof(info->last_good_version)) == 0U ||
        boot_version_field_is_valid(info->min_allowed_ota_version, sizeof(info->min_allowed_ota_version)) == 0U ||
        boot_version_field_is_valid(info->pending_floor_version, sizeof(info->pending_floor_version)) == 0U)
    {
        return 0U;
    }

    if (strcmp(info->current_version,
               boot_info_get_partition_version(info, info->active_slot)) != 0)
    {
        return 0U;
    }

    if (info->trial_state == BOOT_INFO_TRIAL_NONE &&
        info->confirmed_slot != info->active_slot)
    {
        return 0U;
    }

    return (info->data_crc32 == boot_info_compute_crc(info)) ? 1U : 0U;
}

static uint8_t boot_info_payload_is_valid(const void *payload)
{
    return boot_info_is_valid((const BootInfoTypeDef *)payload);
}

static uint8_t txn_payload_is_valid(const void *payload)
{
    return txn_is_valid((const OtaTxnRecord *)payload);
}

static uint8_t boot_info_v2_is_valid(const BootInfoV2Legacy *info)
{
    if (info == 0)
    {
        return 0U;
    }

    if (info->layout_magic != BOOT_INFO_LAYOUT_MAGIC_V2 ||
        info->struct_size != sizeof(BootInfoV2Legacy))
    {
        return 0U;
    }

    if (info->active_partition > OTA_CTRL_PARTITION_APP2 ||
        info->target_partition > OTA_CTRL_PARTITION_APP2 ||
        info->upgrade_flag > BOOT_UPGRADE_FLAG_ROLLBACK ||
        info->boot_tries > IAP_MAX_BOOT_TRIES ||
        info->trial_complete > 1U)
    {
        return 0U;
    }

    if (info->boot_magic != MAGIC_NORMAL &&
        info->boot_magic != MAGIC_REQUEST &&
        info->boot_magic != MAGIC_NEW_FW)
    {
        return 0U;
    }

    if (boot_version_field_is_valid(info->current_version, sizeof(info->current_version)) == 0U ||
        boot_version_field_is_valid(info->app1_version, sizeof(info->app1_version)) == 0U ||
        boot_version_field_is_valid(info->app2_version, sizeof(info->app2_version)) == 0U)
    {
        return 0U;
    }

    return (info->data_crc32 == boot_info_compute_crc_v2(info)) ? 1U : 0U;
}

static uint8_t legacy_boot_info_is_plausible(const legacy_boot_info_t *legacy)
{
    if (legacy == 0)
    {
        return 0U;
    }

    if (legacy->active_partition > OTA_CTRL_PARTITION_APP2 ||
        legacy->target_partition > OTA_CTRL_PARTITION_APP2 ||
        legacy->boot_requested > BOOT_UPGRADE_FLAG_ROLLBACK ||
        legacy->boot_tries > IAP_MAX_BOOT_TRIES ||
        legacy->trial_complete > 1U)
    {
        return 0U;
    }

    if (legacy->magic != MAGIC_NORMAL &&
        legacy->magic != MAGIC_REQUEST &&
        legacy->magic != MAGIC_NEW_FW)
    {
        return 0U;
    }

    return 1U;
}

static void boot_info_migrate_v2(const BootInfoV2Legacy *legacy, BootInfoTypeDef *info)
{
    boot_info_init_default(info);

    if (legacy == 0 || info == 0)
    {
        return;
    }

    info->boot_magic = legacy->boot_magic;
    info->upgrade_flag = legacy->upgrade_flag;
    info->active_slot = legacy->active_partition;
    info->target_slot = legacy->target_partition;
    info->boot_tries = legacy->boot_tries;
    version_text_copy(info->slot_versions[BOOT_INFO_SLOT_APP1], BOOT_INFO_VERSION_LEN, legacy->app1_version);
    version_text_copy(info->slot_versions[BOOT_INFO_SLOT_APP2], BOOT_INFO_VERSION_LEN, legacy->app2_version);
    boot_info_sync_current_version(info);

    if (legacy->trial_complete != 0U)
    {
        info->confirmed_slot = info->active_slot;
        info->trial_state = BOOT_INFO_TRIAL_NONE;
        version_text_copy(info->last_good_version, BOOT_INFO_VERSION_LEN, info->current_version);
    }
    else
    {
        info->confirmed_slot = boot_info_inactive_partition(info->active_slot);
        info->trial_state = BOOT_INFO_TRIAL_PENDING;
        version_text_copy(info->last_good_version,
                          BOOT_INFO_VERSION_LEN,
                          boot_info_get_partition_version(info, info->confirmed_slot));
    }

    version_text_copy(info->min_allowed_ota_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    version_text_copy(info->pending_floor_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    info->data_crc32 = boot_info_compute_crc(info);
}

static void boot_info_migrate_v1(const legacy_boot_info_t *legacy, BootInfoTypeDef *info)
{
    boot_info_init_default(info);

    if (legacy == 0 || info == 0)
    {
        return;
    }

    info->boot_magic = legacy->magic;
    info->upgrade_flag = legacy->boot_requested;
    info->active_slot = legacy->active_partition;
    info->target_slot = legacy->target_partition;
    info->boot_tries = legacy->boot_tries;
    info->confirmed_slot = (legacy->trial_complete != 0U) ?
                           info->active_slot :
                           boot_info_inactive_partition(info->active_slot);
    info->trial_state = (legacy->trial_complete != 0U) ?
                        BOOT_INFO_TRIAL_NONE :
                        BOOT_INFO_TRIAL_PENDING;
    boot_info_sync_current_version(info);
    version_text_copy(info->last_good_version,
                      BOOT_INFO_VERSION_LEN,
                      boot_info_get_partition_version(info, info->confirmed_slot));
    info->data_crc32 = boot_info_compute_crc(info);
}

static uint8_t boot_info_normalize(BootInfoTypeDef *info)
{
    uint8_t changed = 0U;

    if (info == 0)
    {
        return 0U;
    }

    if (info->active_slot > OTA_CTRL_PARTITION_APP2)
    {
        info->active_slot = OTA_CTRL_PARTITION_APP2;
        changed = 1U;
    }

    if (info->target_slot > OTA_CTRL_PARTITION_APP2 ||
        info->target_slot == info->active_slot)
    {
        info->target_slot = boot_info_inactive_partition(info->active_slot);
        changed = 1U;
    }

    if (info->confirmed_slot > OTA_CTRL_PARTITION_APP2)
    {
        info->confirmed_slot = (info->trial_state == BOOT_INFO_TRIAL_PENDING) ?
                               boot_info_inactive_partition(info->active_slot) :
                               info->active_slot;
        changed = 1U;
    }

    if (boot_version_field_is_valid(info->slot_versions[BOOT_INFO_SLOT_APP1], BOOT_INFO_VERSION_LEN) == 0U)
    {
        version_text_copy(info->slot_versions[BOOT_INFO_SLOT_APP1], BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
        changed = 1U;
    }

    if (boot_version_field_is_valid(info->slot_versions[BOOT_INFO_SLOT_APP2], BOOT_INFO_VERSION_LEN) == 0U)
    {
        version_text_copy(info->slot_versions[BOOT_INFO_SLOT_APP2], BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
        changed = 1U;
    }

    if (boot_version_field_is_valid(info->last_good_version, BOOT_INFO_VERSION_LEN) == 0U)
    {
        version_text_copy(info->last_good_version,
                          BOOT_INFO_VERSION_LEN,
                          boot_info_get_partition_version(info, info->confirmed_slot));
        changed = 1U;
    }

    if (boot_version_field_is_valid(info->min_allowed_ota_version, BOOT_INFO_VERSION_LEN) == 0U)
    {
        version_text_copy(info->min_allowed_ota_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
        changed = 1U;
    }

    if (boot_version_field_is_valid(info->pending_floor_version, BOOT_INFO_VERSION_LEN) == 0U)
    {
        version_text_copy(info->pending_floor_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
        changed = 1U;
    }

    if (info->boot_tries == 0U || info->boot_tries > IAP_MAX_BOOT_TRIES)
    {
        info->boot_tries = IAP_MAX_BOOT_TRIES;
        changed = 1U;
    }

    if (strcmp(info->current_version,
               boot_info_get_partition_version(info, info->active_slot)) != 0)
    {
        boot_info_sync_current_version(info);
        changed = 1U;
    }

    if (info->trial_state == BOOT_INFO_TRIAL_NONE &&
        info->confirmed_slot != info->active_slot)
    {
        info->confirmed_slot = info->active_slot;
        changed = 1U;
    }

    return changed;
}

uint8_t version_text_is_valid(const char *version)
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

            ++dot_count;
            has_digit = 0U;
            continue;
        }

        return 0U;
    }

    return (has_digit != 0U && dot_count == 2U) ? 1U : 0U;
}

void version_text_copy(char *target, uint32_t target_len, const char *source)
{
    uint32_t i = 0U;
    const char *value = source;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    if (value == target)
    {
        if (version_text_is_valid(target) != 0U)
        {
            return;
        }

        value = IAP_DEFAULT_VERSION;
    }

    if (value == 0 || version_text_is_valid(value) == 0U)
    {
        value = IAP_DEFAULT_VERSION;
    }

    for (i = 0U; i < target_len; ++i)
    {
        target[i] = '\0';
    }

    for (i = 0U; i + 1U < target_len && value[i] != '\0'; ++i)
    {
        target[i] = value[i];
    }
}

int32_t version_text_compare(const char *left, const char *right)
{
    const char *left_ptr = left;
    const char *right_ptr = right;
    uint32_t left_value = 0U;
    uint32_t right_value = 0U;
    uint8_t index = 0U;

    if (version_text_is_valid(left) == 0U || version_text_is_valid(right) == 0U)
    {
        return 0;
    }

    for (index = 0U; index < 3U; ++index)
    {
        if (version_read_component(&left_ptr, &left_value) == 0U ||
            version_read_component(&right_ptr, &right_value) == 0U)
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

            ++left_ptr;
            ++right_ptr;
        }
    }

    return 0;
}

const char *boot_info_get_partition_version(const BootInfoTypeDef *info, uint32_t partition)
{
    if (info == 0 || partition >= BOOT_INFO_SLOT_COUNT)
    {
        return IAP_DEFAULT_VERSION;
    }

    if (boot_version_field_is_valid(info->slot_versions[partition], BOOT_INFO_VERSION_LEN) == 0U)
    {
        return IAP_DEFAULT_VERSION;
    }

    return info->slot_versions[partition];
}

uint32_t boot_info_partition_address(uint32_t partition)
{
    return (partition == OTA_CTRL_PARTITION_APP2) ? FLASH_APP2_ADDR : FLASH_APP1_ADDR;
}

uint32_t boot_info_inactive_partition(uint32_t active_partition)
{
    return (active_partition == OTA_CTRL_PARTITION_APP2) ? OTA_CTRL_PARTITION_APP1 : OTA_CTRL_PARTITION_APP2;
}

void boot_info_sync_current_version(BootInfoTypeDef *info)
{
    if (info == 0)
    {
        return;
    }

    version_text_copy(info->current_version,
                      sizeof(info->current_version),
                      boot_info_get_partition_version(info, info->active_slot));
}

void boot_info_mark_pending_install(BootInfoTypeDef *info, const OtaImageHeaderPayload *payload)
{
    if (info == 0 || payload == 0)
    {
        return;
    }

    version_text_copy(boot_info_partition_version_ptr(info, payload->target_slot),
                      BOOT_INFO_VERSION_LEN,
                      payload->firmware_version);
    info->boot_magic = MAGIC_NEW_FW;
    info->upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
    info->active_slot = payload->target_slot;
    info->target_slot = boot_info_inactive_partition(payload->target_slot);
    info->trial_state = BOOT_INFO_TRIAL_PENDING;
    info->boot_tries = IAP_MAX_BOOT_TRIES;
    version_text_copy(info->pending_floor_version,
                      BOOT_INFO_VERSION_LEN,
                      payload->min_allowed_version);
    boot_info_sync_current_version(info);
}

uint8_t boot_info_switch_to_confirmed_slot(BootInfoTypeDef *info, uint32_t slot)
{
    if (info == 0 || slot > OTA_CTRL_PARTITION_APP2)
    {
        return 0U;
    }

    info->boot_magic = MAGIC_NORMAL;
    info->upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
    info->active_slot = slot;
    info->target_slot = boot_info_inactive_partition(slot);
    info->confirmed_slot = slot;
    info->trial_state = BOOT_INFO_TRIAL_NONE;
    info->boot_tries = IAP_MAX_BOOT_TRIES;
    boot_info_sync_current_version(info);
    version_text_copy(info->last_good_version, BOOT_INFO_VERSION_LEN, info->current_version);
    version_text_copy(info->pending_floor_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    boot_info_set_min_version(info->min_allowed_ota_version,
                              info->min_allowed_ota_version,
                              info->current_version,
                              info->last_good_version);
    return 1U;
}

static void boot_info_prepare_for_store(BootInfoTypeDef *info)
{
    if (info == 0)
    {
        return;
    }

    info->layout_magic = BOOT_INFO_LAYOUT_MAGIC;
    info->layout_version = BOOT_INFO_LAYOUT_VERSION;
    info->struct_size = (uint16_t)sizeof(BootInfoTypeDef);
    boot_info_sync_current_version(info);
    info->data_crc32 = boot_info_compute_crc(info);
}

static boot_info_load_source_t boot_info_read_current(BootInfoTypeDef *info,
                                                      iap_journal_scan_t *scan,
                                                      uint8_t *normalized_changed,
                                                      uint8_t normalize_loaded)
{
    BootInfoTypeDef stored_v3;
    BootInfoV2Legacy stored_v2;
    legacy_boot_info_t legacy;
    iap_journal_scan_t local_scan;

    if (scan == 0)
    {
        scan = &local_scan;
    }

    if (normalized_changed != 0)
    {
        *normalized_changed = 0U;
    }

    if (info == 0)
    {
        return BOOT_INFO_LOAD_SOURCE_DEFAULT;
    }

    journal_scan_region(BOOT_INFO_JOURNAL_REGION_ADDR,
                        BOOT_INFO_JOURNAL_MAGIC,
                        (uint16_t)sizeof(BootInfoTypeDef),
                        boot_info_payload_is_valid,
                        info,
                        scan);
    if (scan->has_valid != 0U)
    {
        if (normalize_loaded != 0U)
        {
            if (normalized_changed != 0)
            {
                *normalized_changed = boot_info_normalize(info);
            }
            else
            {
                (void)boot_info_normalize(info);
            }
            boot_info_prepare_for_store(info);
        }
        return BOOT_INFO_LOAD_SOURCE_JOURNAL;
    }

    memcpy(&stored_v3, (const void *)BOOT_INFO_ADDR, sizeof(stored_v3));
    if (boot_info_is_valid(&stored_v3) != 0U)
    {
        *info = stored_v3;
        if (normalize_loaded != 0U)
        {
            if (normalized_changed != 0)
            {
                *normalized_changed = boot_info_normalize(info);
            }
            else
            {
                (void)boot_info_normalize(info);
            }
            boot_info_prepare_for_store(info);
        }
        return BOOT_INFO_LOAD_SOURCE_V3_LEGACY;
    }

    memcpy(&stored_v2, (const void *)BOOT_INFO_ADDR, sizeof(stored_v2));
    if (boot_info_v2_is_valid(&stored_v2) != 0U)
    {
        boot_info_migrate_v2(&stored_v2, info);
        if (normalize_loaded != 0U)
        {
            if (normalized_changed != 0)
            {
                *normalized_changed = boot_info_normalize(info);
            }
            else
            {
                (void)boot_info_normalize(info);
            }
            boot_info_prepare_for_store(info);
        }
        return BOOT_INFO_LOAD_SOURCE_V2_LEGACY;
    }

    memcpy(&legacy, (const void *)BOOT_INFO_ADDR, sizeof(legacy));
    if (legacy_boot_info_is_plausible(&legacy) != 0U)
    {
        boot_info_migrate_v1(&legacy, info);
        if (normalize_loaded != 0U)
        {
            if (normalized_changed != 0)
            {
                *normalized_changed = boot_info_normalize(info);
            }
            else
            {
                (void)boot_info_normalize(info);
            }
            boot_info_prepare_for_store(info);
        }
        return BOOT_INFO_LOAD_SOURCE_V1_LEGACY;
    }

    boot_info_init_default(info);
    if (normalize_loaded != 0U)
    {
        boot_info_prepare_for_store(info);
    }
    return BOOT_INFO_LOAD_SOURCE_DEFAULT;
}

static uint8_t txn_read_current_with_diag(OtaTxnRecord *txn,
                                          iap_journal_scan_t *scan,
                                          iap_txn_load_diag_t *diag)
{
    iap_journal_scan_t local_scan;

    if (txn == 0)
    {
        return 0U;
    }

    if (scan == 0)
    {
        scan = &local_scan;
    }

    txn_init_default(txn);
    txn_scan_region(txn, scan, diag);
    if (scan->has_valid != 0U)
    {
        txn_prepare_for_store(txn);
        return 1U;
    }

    txn_prepare_for_store(txn);
    return 0U;
}

static uint8_t txn_read_current(OtaTxnRecord *txn, iap_journal_scan_t *scan)
{
    return txn_read_current_with_diag(txn, scan, 0);
}

static uint32_t journal_commit_records(const BootInfoTypeDef *new_boot_info,
                                       const OtaTxnRecord *new_txn_info,
                                       uint8_t update_boot,
                                       uint8_t update_txn)
{
    BootInfoTypeDef current_boot;
    BootInfoTypeDef final_boot;
    OtaTxnRecord current_txn;
    OtaTxnRecord final_txn;
    iap_journal_scan_t boot_scan;
    iap_journal_scan_t txn_scan;
    iap_journal_slot_t slot_image;
    uint32_t boot_seq = 1U;
    uint32_t txn_seq = 1U;
    uint8_t need_rewrite = 0U;
    uint8_t need_txn_bootstrap = 0U;

    (void)boot_info_read_current(&current_boot, &boot_scan, 0, 0U);
    (void)txn_read_current(&current_txn, &txn_scan);

    final_boot = current_boot;
    final_txn = current_txn;

    if (update_boot != 0U && new_boot_info != 0)
    {
        final_boot = *new_boot_info;
    }

    if (update_txn != 0U && new_txn_info != 0)
    {
        final_txn = *new_txn_info;
    }

    (void)boot_info_normalize(&final_boot);
    boot_info_prepare_for_store(&final_boot);
    txn_prepare_for_store(&final_txn);

    if (update_boot != 0U &&
        boot_scan.has_valid != 0U &&
        memcmp(&current_boot, &final_boot, sizeof(final_boot)) == 0)
    {
        update_boot = 0U;
    }

    if (update_txn != 0U &&
        txn_scan.has_valid != 0U &&
        memcmp(&current_txn, &final_txn, sizeof(final_txn)) == 0)
    {
        update_txn = 0U;
    }

    need_txn_bootstrap = (txn_scan.has_valid == 0U) ? 1U : 0U;

    if (boot_scan.has_valid == 0U)
    {
        need_rewrite = 1U;
    }
    else if (update_boot != 0U && boot_scan.has_empty == 0U)
    {
        need_rewrite = 1U;
    }
    else if ((update_txn != 0U || need_txn_bootstrap != 0U) &&
             txn_scan.has_empty == 0U)
    {
        need_rewrite = 1U;
    }

    if (update_boot == 0U && update_txn == 0U && need_txn_bootstrap == 0U)
    {
        return 0U;
    }

    if (need_rewrite != 0U)
    {
        if (update_boot != 0U)
        {
            boot_seq = (boot_scan.has_valid != 0U) ? journal_seq_next(boot_scan.latest_seq) : 1U;
        }
        else
        {
            boot_seq = (boot_scan.has_valid != 0U) ? boot_scan.latest_seq : 1U;
        }

        if (update_txn != 0U)
        {
            txn_seq = (txn_scan.has_valid != 0U) ? journal_seq_next(txn_scan.latest_seq) : 1U;
        }
        else
        {
            txn_seq = (txn_scan.has_valid != 0U) ? txn_scan.latest_seq : 1U;
        }

        if (need_txn_bootstrap != 0U && update_txn == 0U)
        {
            txn_seq = 1U;
        }

        return journal_rewrite_sector(&final_boot, boot_seq, &final_txn, txn_seq);
    }

    if (update_boot != 0U)
    {
        boot_seq = journal_seq_next(boot_scan.latest_seq);
        journal_build_slot(&slot_image,
                           BOOT_INFO_JOURNAL_MAGIC,
                           (uint16_t)sizeof(BootInfoTypeDef),
                           boot_seq,
                           &final_boot);
        if (journal_write_slot(boot_scan.empty_addr, &slot_image) != 0U)
        {
            return 1U;
        }
    }

    if (update_txn != 0U || need_txn_bootstrap != 0U)
    {
        txn_seq = (update_txn != 0U && txn_scan.has_valid != 0U) ?
                  journal_seq_next(txn_scan.latest_seq) :
                  1U;
        if (update_txn != 0U && txn_scan.has_valid == 0U)
        {
            txn_seq = 1U;
        }
        journal_build_slot(&slot_image,
                           TXN_JOURNAL_MAGIC,
                           (uint16_t)sizeof(OtaTxnRecord),
                           txn_seq,
                           &final_txn);
        if (journal_write_slot(txn_scan.empty_addr, &slot_image) != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}

uint32_t boot_info_save(const BootInfoTypeDef *info)
{
    if (info == 0)
    {
        return 1U;
    }

    return journal_commit_records(info, 0, 1U, 0U);
}

void boot_info_load(BootInfoTypeDef *info)
{
    boot_info_load_source_t source = BOOT_INFO_LOAD_SOURCE_DEFAULT;
    uint8_t normalized_changed = 0U;
    iap_journal_scan_t txn_scan;
    OtaTxnRecord txn;

    if (info == 0)
    {
        return;
    }

    source = boot_info_read_current(info, 0, &normalized_changed, 1U);
    (void)txn_read_current(&txn, &txn_scan);

    if (source != BOOT_INFO_LOAD_SOURCE_JOURNAL ||
        normalized_changed != 0U ||
        txn_scan.has_valid == 0U)
    {
        (void)boot_info_save(info);
    }
}

void txn_load(OtaTxnRecord *txn)
{
    (void)txn_read_current(txn, 0);
}

void txn_load_with_diag(OtaTxnRecord *txn, iap_txn_load_diag_t *diag)
{
    iap_journal_scan_t scan;
    uint8_t has_valid = 0U;

    if (diag == 0)
    {
        (void)txn_read_current(txn, &scan);
        return;
    }

    memset(diag, 0, sizeof(*diag));
    has_valid = txn_read_current_with_diag(txn, &scan, diag);
    diag->has_valid = has_valid;
    diag->latest_seq = scan.latest_seq;
    diag->programmed_slots = scan.programmed_slots;
    diag->invalid_slots = scan.invalid_slots;

    if (has_valid != 0U)
    {
        diag->source = OTA_CTRL_TXN_LOAD_SRC_VALID;
    }
    else if (scan.invalid_slots != 0U || scan.programmed_slots != 0U)
    {
        diag->source = OTA_CTRL_TXN_LOAD_SRC_INVALID;
    }
    else
    {
        diag->source = OTA_CTRL_TXN_LOAD_SRC_EMPTY;
    }
}

uint32_t txn_save(const OtaTxnRecord *txn)
{
    if (txn == 0)
    {
        return 1U;
    }

    return journal_commit_records(0, txn, 0U, 1U);
}

uint32_t txn_compact_with_boot_info(const BootInfoTypeDef *boot_info, const OtaTxnRecord *txn)
{
    BootInfoTypeDef current_boot;
    OtaTxnRecord current_txn;
    BootInfoTypeDef prepared_boot;
    OtaTxnRecord prepared_txn;
    iap_journal_scan_t boot_scan;
    iap_journal_scan_t txn_scan;
    uint32_t boot_seq = 1U;
    uint32_t txn_seq = 1U;

    if (boot_info == 0 || txn == 0)
    {
        return 1U;
    }

    prepared_boot = *boot_info;
    prepared_txn = *txn;

    (void)boot_info_normalize(&prepared_boot);
    boot_info_prepare_for_store(&prepared_boot);
    txn_prepare_for_store(&prepared_txn);

    (void)boot_info_read_current(&current_boot, &boot_scan, 0, 0U);
    (void)txn_read_current(&current_txn, &txn_scan);

    if (boot_scan.has_valid != 0U)
    {
        boot_seq = journal_seq_next(boot_scan.latest_seq);
    }

    if (txn_scan.has_valid != 0U)
    {
        txn_seq = journal_seq_next(txn_scan.latest_seq);
    }

    return journal_rewrite_sector(&prepared_boot, boot_seq, &prepared_txn, txn_seq);
}

uint32_t txn_clear(void)
{
    OtaTxnRecord txn;

    txn_init_default(&txn);
    return txn_save(&txn);
}
