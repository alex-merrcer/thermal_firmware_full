#ifndef IAP_INTERNAL_H
#define IAP_INTERNAL_H

#include "iap.h"
#include "flash_if.h"
#include "ymodem.h"
#include "lcd.h"
#include "delay.h"
#include "misc.h"
#include "common.h"
#include "stm32f4xx_tim.h"
#include "stm32f4xx_usart.h"
#include "../../../protocol/ota_ctrl_protocol.h"
#include "../../../protocol/ota_data_protocol.h"
#include "../../../protocol/ota_ctrl_protocol_text.h"
#include "../../../protocol/ota_image_header.h"
#include "../../../protocol/ota_rsa_public_key.h"
#include <string.h>

#define OTA_CTRL_REQ_FLAG_BASE 0x00000003UL

#define IAP_DEVICE_PRODUCT_ID "LCD"
#define IAP_DEVICE_HW_REV "A1"
#define IAP_DEFAULT_VERSION "0.0.0"
#define IAP_MAX_BOOT_TRIES 3U

#define OTA_CTRL_REQ_RETRY_COUNT 3U
#define OTA_CTRL_ACK_TIMEOUT_MS 5000U
#define OTA_CTRL_READY_TIMEOUT_MS 120000U
#define OTA_CTRL_FRAME_WAIT_MS 500U
#define OTA_CTRL_POLL_STEP_US 50U
#define STM32_UID_BASE_ADDR 0x1FFF7A10U

#define IWDG_PRESCALER IWDG_Prescaler_256
#define IWDG_TIMEOUT_MS 20000U
#define IWDG_RELOAD ((uint16_t)(IWDG_TIMEOUT_MS * 32000U / 1024U))
#define IAP_BOOT_SPLASH_HOLD_MS 2000U

#define IAP_TXN_LAYOUT_MAGIC 0x54584E31UL
#define IAP_TXN_LAYOUT_VERSION 3U
#define IAP_TXN_RECORD_MAX_SIZE 236U

typedef struct
{
    uint8_t msg_type;
    uint8_t seq;
    uint16_t payload_len;
    uint8_t payload[OTA_CTRL_MAX_PAYLOAD_LEN];
} ota_ctrl_frame_t;

typedef struct
{
    uint8_t target_partition;
    uint16_t ready_flags;
    char version[BOOT_INFO_VERSION_LEN];
    uint32_t plain_size;
    uint32_t transfer_size;
    uint32_t checkpoint_size;
    uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN];
} ota_ctrl_ready_info_t;

typedef struct
{
    const BootInfoTypeDef *boot_info;
    uint8_t error_code;
} iap_header_validation_context_t;

typedef struct
{
    uint8_t source;
    uint8_t has_valid;
    uint8_t reserved0;
    uint8_t reserved1;
    uint32_t latest_seq;
    uint32_t programmed_slots;
    uint32_t invalid_slots;
    uint32_t invalid_slot_header;
    uint32_t invalid_slot_commit;
    uint32_t invalid_slot_payload_crc;
    uint32_t invalid_slot_payload_validate;
    uint32_t invalid_txn_layout;
    uint32_t invalid_txn_state;
    uint32_t invalid_txn_partition;
    uint32_t invalid_txn_fields;
    uint32_t invalid_txn_request_type;
    uint32_t invalid_txn_version_text;
    uint32_t invalid_txn_offset;
    uint32_t invalid_txn_data_crc;
} iap_txn_load_diag_t;

typedef enum
{
    IAP_AUTH_RESULT_OK = 0U,
    IAP_AUTH_RESULT_SIZE = 1U,
    IAP_AUTH_RESULT_HASH_ENGINE = 2U,
    IAP_AUTH_RESULT_HASH_MISMATCH = 3U,
    IAP_AUTH_RESULT_RSA = 4U,
    IAP_AUTH_RESULT_INTERNAL = 5U
} iap_auth_result_t;

typedef enum
{
    IAP_HASH_DIAG_NONE = 0U,
    IAP_HASH_DIAG_BODY_CALC = 1U,
    IAP_HASH_DIAG_FLASH_CALC = 2U,
    IAP_HASH_DIAG_BODY_MISMATCH = 3U,
    IAP_HASH_DIAG_FLASH_MISMATCH = 4U,
    IAP_HASH_DIAG_EXPECTED_MISMATCH = 5U,
    IAP_HASH_DIAG_SELF_TEST = 6U
} iap_hash_diag_t;

typedef struct
{
    uint8_t expected_hash[32];
    uint8_t body_hash[32];
    uint8_t flash_hash[32];
    uint8_t has_expected;
    uint8_t has_body;
    uint8_t has_flash;
    iap_hash_diag_t hash_diag;
} iap_auth_diag_t;

typedef enum
{
    IAP_TXN_STATE_IDLE = 0U,
    IAP_TXN_STATE_NEGOTIATING = 1U,
    IAP_TXN_STATE_HEADER_VERIFIED = 2U,
    IAP_TXN_STATE_TRANSFERRING = 3U,
    IAP_TXN_STATE_PAUSED_RESUMABLE = 4U,
    IAP_TXN_STATE_RECEIVED = 5U,
    IAP_TXN_STATE_AUTHORIZED = 6U,
    IAP_TXN_STATE_COMMITTED = 7U,
    IAP_TXN_STATE_FAILED_RETRYABLE = 8U,
    IAP_TXN_STATE_FAILED_TERMINAL = 9U
} iap_txn_state_t;

#define IAP_TXN_PAUSE_NONE              0U
#define IAP_TXN_PAUSE_RETRYABLE         1U
#define IAP_TXN_PAUSE_PROTOCOL_MISMATCH 2U
#define IAP_TXN_PAUSE_TERMINAL          3U

typedef struct
{
    uint32_t layout_magic;
    uint16_t layout_version;
    uint16_t struct_size;
    uint32_t data_crc32;
    uint32_t txn_counter;
    uint32_t state;
    uint32_t request_type;
    uint32_t active_slot;
    uint32_t target_slot;
    char current_version[BOOT_INFO_VERSION_LEN];
    char target_version[BOOT_INFO_VERSION_LEN];
    uint32_t last_error_stage;
    uint32_t last_error_code;
    uint32_t retryable;
    uint32_t transfer_total_size;
    uint32_t protocol_version;
    uint32_t checkpoint_size;
    uint32_t chunk_size;
    uint32_t plain_size;
    uint32_t resume_offset;
    uint32_t last_acked_offset;
    uint32_t pause_reason;
    uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN];
    uint32_t reserved[24];
} OtaTxnRecord;

typedef char iap_txn_record_size_check[(sizeof(OtaTxnRecord) <= IAP_TXN_RECORD_MAX_SIZE) ? 1 : -1];

extern uint8_t tab_1024[1024];

uint8_t version_text_is_valid(const char *version);
void version_text_copy(char *target, uint32_t target_len, const char *source);
int32_t version_text_compare(const char *left, const char *right);
const char *boot_info_get_partition_version(const BootInfoTypeDef *info, uint32_t partition);
uint32_t boot_info_partition_address(uint32_t partition);
uint32_t boot_info_inactive_partition(uint32_t active_partition);
void boot_info_sync_current_version(BootInfoTypeDef *info);
void boot_info_mark_pending_install(BootInfoTypeDef *info, const OtaImageHeaderPayload *payload);
uint8_t boot_info_switch_to_confirmed_slot(BootInfoTypeDef *info, uint32_t slot);
uint32_t boot_info_save(const BootInfoTypeDef *info);
void boot_info_load(BootInfoTypeDef *info);
void txn_init_default(OtaTxnRecord *txn);
void txn_load(OtaTxnRecord *txn);
void txn_load_with_diag(OtaTxnRecord *txn, iap_txn_load_diag_t *diag);
uint32_t txn_save(const OtaTxnRecord *txn);
uint32_t txn_compact_with_boot_info(const BootInfoTypeDef *boot_info, const OtaTxnRecord *txn);
uint32_t txn_clear(void);

uint8_t iap_validate_received_header(const OtaImageHeaderBinary *header, void *context);
uint8_t iap_verify_image_header_signature(const OtaImageHeaderBinary *header);
iap_auth_result_t iap_authorize_received_image(const BootInfoTypeDef *boot_info,
                                               const OtaImageHeaderBinary *header,
                                               uint32_t firmware_address,
                                               iap_auth_diag_t *diag);
const char *iap_auth_result_text(iap_auth_result_t result, const iap_auth_diag_t *diag);
void iap_serial_report_code(const char *code);
void iap_serial_report_auth_failure(iap_auth_result_t result, const iap_auth_diag_t *diag);
void iap_serial_report_txn_load(const OtaTxnRecord *txn, const iap_txn_load_diag_t *diag);
void iap_serial_report_uart_flags(uint32_t flags);

uint8_t ota_ctrl_wait_for_upgrade_ready(const BootInfoTypeDef *boot_info,
                                        ota_ctrl_ready_info_t *ready_info,
                                        uint16_t *reject_reason,
                                        uint32_t req_flags);
uint8_t ota_ctrl_send_status(uint8_t stage,
                             uint8_t percent,
                             uint16_t detail_code,
                             uint32_t current_value,
                             uint32_t total_value);
uint8_t ota_ctrl_send_go(uint8_t target_partition, uint16_t go_flags, uint32_t resume_offset);
uint8_t ota_ctrl_wait_for_meta_image_header(OtaImageHeaderBinary *header, uint32_t timeout_ms);
uint8_t ota_ctrl_send_result(uint8_t outcome, uint8_t stage, uint16_t error_code, uint32_t final_offset);
void ota_ctrl_flush_uart(void);

void ota_ctrl_show_status_lines(u16 color,
                                const char *line1,
                                const char *line2,
                                const char *line3,
                                const char *line4);
void ota_ctrl_show_status_text(const char *line1, const char *line2);
void ota_ctrl_show_stage(uint8_t stage,
                         uint8_t percent,
                         uint16_t detail_code,
                         uint32_t current_value,
                         uint32_t total_value);
void ota_ctrl_show_ready_info(const ota_ctrl_frame_t *frame);
void ota_ctrl_show_error_code(uint8_t stage, uint16_t error_code);
void ota_ctrl_show_ack_reject_reason(uint16_t reason_code);
void iap_show_version_lines(const BootInfoTypeDef *boot_info);
void iap_show_resume_decision(uint8_t accepted,
                              uint16_t reason_code,
                              uint32_t saved_offset,
                              uint32_t total_size);
void iap_ui_show_boot_splash(void);
void iap_ui_boot_prepare(uint8_t warm_handoff);
void iap_ui_show_upgrade_prepare(void);
void iap_ui_show_upgrade_success(void);
void iap_ui_show_upgrade_failure(void);
void iap_reset_ymodem_progress(void);
void iap_show_ymodem_progress(uint32_t current, uint32_t total);

void iap_init_watchdog(void);
void iap_feed_watchdog(void);
uint8_t is_app_valid(uint32_t app_addr);
void iap_cleanup_before_jump(void);
uint8_t get_reset_reason(void);
void clear_reset_flags(void);
uint32_t Send_Byte(uint8_t c);

#endif
