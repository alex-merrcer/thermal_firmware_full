#ifndef OTA_STM32_INTERNAL_H
#define OTA_STM32_INTERNAL_H

#include "OTA_STM32.h"
#include "ymodem.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "miniz.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "driver/uart.h"
#include "../../../../protocol/ota_ctrl_protocol.h"
#include "../../../../protocol/ota_data_protocol.h"
#include "../../../../protocol/ota_image_header.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define OTA_STM32_TAG "OTA_STM32"

#define OTA_HTTP_TIMEOUT_MS              (30000)
#define OTA_HTTP_READ_BUFFER_SIZE        (2048)
#define OTA_METADATA_MAX_SIZE            (8192)
#define OTA_STREAM_BUFFER_SIZE           (2048)
#define OTA_AES_BLOCK_SIZE               (16)
#define OTA_AES_KEY_BYTES                (32)
#define OTA_FLASH_SECTOR_SIZE            (4096)
#define OTA_TRANSFER_CHECKPOINT_SIZE     (4096U)
#define OTA_TASK_STACK_SIZE              (32768)
#define OTA_CACHE_PARTITION_LABEL        "iapcache"
#define OTA_MAX_PACKAGES                 (4U)
#define OTA_PACKAGE_URL_MAX_LEN          (256U)
#define OTA_DEFAULT_DEVICE_VERSION       "0.0.0"
#define OTA_SHA256_HEX_LEN               (64U)
#define OTA_IV_HEX_LEN                   (32U)
#define OTA_BASE64_TEXT_MAX_LEN          (512U)
#define OTA_SIGNING_PAYLOAD_MAX_SIZE     (4096U)

#define OTA_CTRL_SERVICE_IDLE_MS         (100U)
#define OTA_CTRL_FRAME_WAIT_MS           (500U)
#define OTA_CTRL_GO_TIMEOUT_MS           (10000U)
#define OTA_CTRL_READY_GUARD_MS          (30U)
#define OTA_BOOT_REPORT_TOTAL_TIMEOUT_MS (12000U)
#define OTA_BOOT_REPORT_IDLE_TIMEOUT_MS  (500U)
#define OTA_BOOT_REPORT_FAIL_TIMEOUT_MS  (1500U)
#define OTA_BOOT_REPORT_FAIL_IDLE_MS     (300U)
#define OTA_BOOT_REPORT_LINE_MAX_LEN     (96U)

#define OTA_PACKAGE_BASE_URL             "https://esp32-bin1.oss-cn-guangzhou.aliyuncs.com/firmware"
#define OTA_LATEST_JSON_NAME             "release-manifest.v2.json"
#define OTA_SUPPORTED_PRODUCT_ID         "LCD"
#define OTA_SUPPORTED_HW_REV             "A1"

#define ZIP_EOCD_SIGNATURE               (0x06054B50UL)
#define ZIP_CDIR_SIGNATURE               (0x02014B50UL)
#define ZIP_LOCAL_SIGNATURE              (0x04034B50UL)

typedef struct
{
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool is_mapped;
    esp_partition_mmap_handle_t map_handle;
} ota_blob_t;

typedef struct
{
    int package_format_version;
    char firmware_file_name[128];
    size_t firmware_size;
    char firmware_crc32[9];
    char firmware_sha256[OTA_SHA256_HEX_LEN + 1U];
    char signature_algorithm[16];
    char hash_algorithm[16];
    bool requires_encryption;
    char encryption_algorithm[32];
    char encryption_key_id[32];
    char encryption_iv_hex[OTA_IV_HEX_LEN + 1U];
    char transfer_encoding[24];
    char signature_base64[OTA_BASE64_TEXT_MAX_LEN];
    uint8_t *signature;
    size_t signature_len;
    uint8_t *manifest_signature;
    size_t manifest_signature_len;
} iap_manifest_t;

typedef struct
{
    uint16_t flags;
    uint16_t compression_method;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
} zip_entry_info_t;

typedef struct
{
    ota_blob_t package_blob;
    ota_blob_t manifest_blob;
    ota_blob_t image_header_blob;
    iap_manifest_t manifest;
    zip_entry_info_t manifest_entry;
    zip_entry_info_t image_header_entry;
    zip_entry_info_t firmware_entry;
    OtaImageHeaderBinary image_header;
} ota_iap_context_t;

typedef struct
{
    const ota_blob_t *package_blob;
    const uint8_t *compressed_data;
    size_t compressed_size;
    size_t uncompressed_size;
    size_t compressed_offset;
    size_t output_total;
    uint16_t compression_method;
    bool finished;
    bool inflate_done;
    uint8_t *dictionary;
    size_t dictionary_write_offset;
    size_t pending_offset;
    size_t pending_count;
    tinfl_decompressor inflator;
} zip_entry_stream_t;

typedef struct
{
    zip_entry_stream_t plain_stream;
    mbedtls_aes_context aes;
    const uint8_t *header_data;
    size_t header_size;
    size_t header_offset;
    uint8_t iv[OTA_AES_BLOCK_SIZE];
    uint8_t plain_block[OTA_AES_BLOCK_SIZE];
    size_t plain_block_len;
    size_t plain_total_read;
    size_t plain_total_expected;
    uint8_t cipher_block[OTA_AES_BLOCK_SIZE];
    size_t cipher_block_offset;
    size_t cipher_block_len;
    bool finalized;
    bool error;
} encrypted_firmware_stream_t;

typedef struct
{
    uint8_t msg_type;
    uint8_t seq;
    uint16_t payload_len;
    uint8_t payload[OTA_CTRL_MAX_PAYLOAD_LEN];
} ota_ctrl_frame_t;

typedef struct
{
    uint8_t request_type;
    uint8_t active_partition;
    uint8_t target_partition;
    bool version_valid;
    char current_version[OTA_CTRL_VERSION_LEN + 1U];
    char product_id[OTA_CTRL_PRODUCT_ID_LEN + 1U];
    char hw_rev[OTA_CTRL_HW_REV_LEN + 1U];
    uint8_t device_uid[OTA_CTRL_UID_LEN];
    uint32_t flags;
} ota_upgrade_request_t;

typedef struct
{
    uint8_t target_partition;
    uint16_t go_flags;
    uint32_t resume_transfer_offset;
} ota_go_request_t;

typedef struct
{
    uint8_t outcome;
    uint8_t stage;
    uint16_t error_code;
    uint32_t final_offset;
} ota_ctrl_result_info_t;

typedef struct
{
    uint8_t stage;
    uint16_t error_code;
} ota_validation_result_t;

typedef struct
{
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    bool valid;
} ota_semver_t;

typedef enum
{
    OTA_DECISION_NO_UPGRADE = 0,
    OTA_DECISION_UPGRADE,
    OTA_DECISION_FORCE_UPGRADE,
    OTA_DECISION_ERROR
} ota_upgrade_decision_t;

typedef struct
{
    char partition[8];
    char version[OTA_CTRL_VERSION_LEN + 1U];
    char url[OTA_PACKAGE_URL_MAX_LEN];
} ota_package_info_t;

typedef struct
{
    char release_id[64];
    char manifest_version[8];
    char product_id[OTA_CTRL_PRODUCT_ID_LEN + 1U];
    char hw_rev[OTA_CTRL_HW_REV_LEN + 1U];
    char latest_version[OTA_CTRL_VERSION_LEN + 1U];
    char min_version[OTA_CTRL_VERSION_LEN + 1U];
    bool force_update;
    char signature_algorithm[16];
    char hash_algorithm[16];
    char signed_at[40];
    ota_package_info_t packages[OTA_MAX_PACKAGES];
    size_t package_count;
    uint8_t *signature;
    size_t signature_len;
} ota_latest_json_t;

typedef struct
{
    ota_upgrade_decision_t decision;
    char selected_version[OTA_CTRL_VERSION_LEN + 1U];
    char package_url[OTA_PACKAGE_URL_MAX_LEN];
} ota_upgrade_plan_t;

typedef enum
{
    OTA_BOOT_REPORT_OUTCOME_NONE = 0,
    OTA_BOOT_REPORT_OUTCOME_SUCCESS = 1,
    OTA_BOOT_REPORT_OUTCOME_RETRYABLE = 2,
    OTA_BOOT_REPORT_OUTCOME_TERMINAL = 3
} ota_boot_report_outcome_t;

typedef struct
{
    bool received_any;
    ota_boot_report_outcome_t outcome;
    char result_code[24];
} ota_stm32_boot_report_t;

extern const uint8_t g_ota_aes_key[OTA_AES_KEY_BYTES];
bool ota_aes_uses_external_key(void);
const char *ota_aes_key_id_text(void);
void ota_log_aes_security_profile(void);

void ota_ctrl_log_status_event(const char *direction,
                               uint8_t stage,
                               uint8_t percent,
                               uint16_t detail_code,
                               uint32_t current_value,
                               uint32_t total_value);
void ota_ctrl_log_error_event(const char *direction,
                              uint8_t stage,
                              uint16_t error_code);
void ota_ctrl_log_ack_event(const char *direction,
                            uint8_t seq,
                            bool accept,
                            uint8_t target_partition,
                            uint16_t reason_code);
void ota_ctrl_log_ready_event(const char *direction,
                              uint8_t target_partition,
                              const char *release_version,
                              uint16_t ready_flags,
                              uint32_t plain_size,
                              uint32_t transfer_size,
                              uint32_t checkpoint_size,
                              const uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN]);
void ota_ctrl_log_request_event(const char *direction,
                                uint8_t seq,
                                const ota_upgrade_request_t *request);
uint16_t ota_ctrl_crc16(const uint8_t *data, uint16_t length);
void ota_ctrl_write_u16le(uint8_t *buffer, uint16_t value);
void ota_ctrl_write_u32le(uint8_t *buffer, uint32_t value);
uint16_t ota_ctrl_read_u16le(const uint8_t *buffer);
uint32_t ota_ctrl_read_u32le(const uint8_t *buffer);
void ota_ctrl_copy_fixed_string(char *target,
                                size_t target_len,
                                const uint8_t *source,
                                size_t source_len);
bool ota_ctrl_read_byte_timeout(uint8_t *byte, uint32_t timeout_ms);
void ota_ctrl_flush_uart(void);
void ota_ctrl_pushback_bytes(const uint8_t *data, size_t data_len);
bool ota_ctrl_send_frame(uint8_t msg_type,
                         uint8_t seq,
                         const uint8_t *payload,
                         uint16_t payload_len);
bool ota_ctrl_receive_frame(ota_ctrl_frame_t *frame, uint32_t timeout_ms);
bool ota_ctrl_parse_request_payload(const uint8_t *payload,
                                    size_t payload_len,
                                    ota_upgrade_request_t *request);
bool ota_ctrl_send_ack(uint8_t seq,
                       const ota_upgrade_request_t *request,
                       bool accept,
                       uint16_t reason_code);
bool ota_ctrl_send_status(uint8_t seq,
                          uint8_t stage,
                          uint8_t percent,
                          uint16_t detail_code,
                          uint32_t current_value,
                          uint32_t total_value);
bool ota_ctrl_send_ready(uint8_t seq,
                         const ota_upgrade_request_t *request,
                         const char *release_version,
                         uint16_t ready_flags,
                         uint32_t plain_size,
                         uint32_t transfer_size,
                         uint32_t checkpoint_size,
                         const uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN]);
bool ota_ctrl_send_error(uint8_t seq, uint8_t stage, uint16_t error_code);
uint16_t ota_ctrl_validate_request(const ota_upgrade_request_t *request);
bool ota_request_is_check_only(const ota_upgrade_request_t *request);
bool ota_ctrl_wait_for_request(ota_upgrade_request_t *request, uint8_t *seq);
bool ota_ctrl_wait_for_go(const ota_upgrade_request_t *request, ota_go_request_t *go_request);
bool ota_ctrl_send_image_header_meta(uint8_t seq, const OtaImageHeaderBinary *header);

void blob_free(ota_blob_t *blob);
bool blob_attach_mmap(ota_blob_t *blob,
                      const uint8_t *mapped_data,
                      size_t data_size,
                      esp_partition_mmap_handle_t map_handle);
bool blob_reserve(ota_blob_t *blob, size_t required_capacity);
bool blob_resize(ota_blob_t *blob, size_t size, bool append_terminator);
void manifest_free(iap_manifest_t *manifest);
void ota_context_free(ota_iap_context_t *context);
bool download_text_blob(const char *url,
                        ota_blob_t *blob,
                        size_t max_size,
                        const char *label);
uint16_t read_le16(const uint8_t *data);
uint32_t read_le32(const uint8_t *data);
bool attach_cached_iap_package(size_t package_size, ota_blob_t *package_blob);
bool download_iap_package(const char *url, ota_blob_t *package_blob);
bool extract_iap_package(ota_iap_context_t *context);
void zip_entry_stream_free(zip_entry_stream_t *stream);
bool zip_entry_stream_init(zip_entry_stream_t *stream,
                           const ota_blob_t *package_blob,
                           const char *entry_name,
                           const zip_entry_info_t *entry);
bool zip_entry_stream_refill(zip_entry_stream_t *stream);
bool zip_entry_stream_read(zip_entry_stream_t *stream,
                           uint8_t *buffer,
                           size_t buffer_len,
                           size_t *bytes_read);

bool ota_parse_semver(const char *text, ota_semver_t *version);
int ota_compare_semver(const ota_semver_t *lhs, const ota_semver_t *rhs);
bool ota_ctrl_build_metadata_url(char *url_buffer, size_t url_buffer_len);
bool copy_json_string_with_context(const char *context_name,
                                   cJSON *root,
                                   const char *field_name,
                                   char *buffer,
                                   size_t buffer_len);
bool copy_json_string(cJSON *root, const char *field_name, char *buffer, size_t buffer_len);
bool normalize_crc32_text(const char *input, char output[9]);
bool normalize_hex_text(const char *input, char *output, size_t expected_chars);
bool append_signing_field(char *buffer,
                          size_t buffer_len,
                          size_t *offset,
                          const char *name,
                          const char *value);
bool build_release_manifest_signing_payload(const ota_latest_json_t *latest,
                                            char *buffer,
                                            size_t buffer_len);
bool decode_base64_signature(const char *base64_text, uint8_t **signature, size_t *signature_len);
bool parse_latest_json(const ota_blob_t *latest_blob, ota_latest_json_t *latest);
ota_upgrade_decision_t ota_decide_upgrade(const ota_upgrade_request_t *request,
                                          const ota_latest_json_t *latest);
const ota_package_info_t *ota_find_package_for_partition(const ota_latest_json_t *latest,
                                                         uint8_t target_partition);
bool ota_prepare_upgrade_plan(const ota_upgrade_request_t *request,
                              ota_upgrade_plan_t *plan,
                              uint16_t *reject_reason);
bool ota_cache_try_prepare_plan(const ota_upgrade_request_t *request,
                                ota_upgrade_plan_t *plan,
                                size_t *package_size);
bool ota_cache_store_valid(const ota_upgrade_request_t *request,
                           const ota_upgrade_plan_t *plan,
                           size_t package_size,
                           uint32_t transfer_size,
                           uint32_t checkpoint_size,
                           const uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN]);
void ota_cache_clear(bool terminal);
bool build_iap_manifest_signing_payload(const iap_manifest_t *manifest,
                                        char *buffer,
                                        size_t buffer_len);
bool parse_manifest(const ota_blob_t *manifest_blob, iap_manifest_t *manifest);

void ota_validation_result_set(ota_validation_result_t *result,
                               uint8_t stage,
                               uint16_t error_code);
bool verify_signature_payload(const uint8_t *payload,
                              size_t payload_len,
                              const char *signature_algorithm,
                              const char *hash_algorithm,
                              const uint8_t *signature,
                              size_t signature_len,
                              const char *context_label);
void crc32_to_hex(uint32_t crc32, char buffer[9]);
void bytes_to_hex_string(const uint8_t *bytes,
                         size_t byte_count,
                         char *buffer,
                         size_t buffer_len);
bool validate_firmware_stream(const ota_iap_context_t *context,
                              ota_validation_result_t *result,
                              uint8_t firmware_sha256[32]);
bool validate_image_header(const ota_iap_context_t *context,
                           uint8_t expected_target_partition,
                           const char *expected_version,
                           ota_validation_result_t *result,
                           const uint8_t firmware_sha256[32]);
bool validate_iap_package(const ota_iap_context_t *context,
                          const ota_upgrade_request_t *request,
                          const ota_upgrade_plan_t *plan,
                          ota_validation_result_t *result);
bool ota_compute_session_fingerprint(const ota_iap_context_t *context,
                                     uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN]);

bool aes_encrypt_block(mbedtls_aes_context *aes,
                       uint8_t iv[OTA_AES_BLOCK_SIZE],
                       uint8_t block[OTA_AES_BLOCK_SIZE]);
bool aes_encrypt_pkcs7(const uint8_t *plain_data,
                       size_t plain_size,
                       const uint8_t iv_bytes[OTA_AES_BLOCK_SIZE],
                       bool prefix_iv,
                       ota_blob_t *encrypted_blob);
int hex_char_to_value(char ch);
bool hex_to_bytes(const char *hex, uint8_t *buffer, size_t buffer_len);
bool aes_self_test(void);
size_t calculate_encrypted_size(size_t plain_size);
bool encrypted_stream_read_plain(encrypted_firmware_stream_t *stream,
                                 uint8_t *buffer,
                                 size_t buffer_len,
                                 size_t *bytes_read);
bool encrypted_stream_init(encrypted_firmware_stream_t *stream,
                           const ota_iap_context_t *context);
void encrypted_stream_free(encrypted_firmware_stream_t *stream);
bool encrypted_stream_fill_next_block(encrypted_firmware_stream_t *stream);
size_t encrypted_stream_read_callback(void *context, uint8_t *buffer, size_t max_len);

void build_transfer_file_name(const char *original_name, char *buffer, size_t buffer_len);
void ota_log_stm32_boot_report_line(const char *line, ota_stm32_boot_report_t *report);
ota_stm32_boot_report_t ota_read_stm32_boot_report(void);
ota_stm32_boot_report_t ota_read_stm32_boot_report_with_timeouts(uint32_t total_timeout_ms,
                                                                 uint32_t idle_timeout_ms);
bool ymodem_send_encrypted_stream(const ota_iap_context_t *context,
                                  const char *transfer_file_name,
                                  size_t start_transfer_offset);

#endif
