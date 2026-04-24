#include "ota_stm32_transfer.h"

#define TAG OTA_STM32_TAG

typedef struct
{
    uint8_t type;
    uint32_t session_id;
    uint32_t offset;
    uint16_t payload_len;
    uint8_t payload[OTA_DATA_MAX_PAYLOAD_LEN];
} ota_data_frame_t;

static bool ota_boot_report_append_char(char line[OTA_BOOT_REPORT_LINE_MAX_LEN],
                                        size_t *line_len,
                                        uint8_t ch,
                                        ota_stm32_boot_report_t *report)
{
    if (line == NULL || line_len == NULL) {
        return false;
    }

    if (ch == '\n') {
        line[*line_len] = '\0';
        ota_log_stm32_boot_report_line(line, report);
        *line_len = 0U;
        return true;
    }

    if (ch == '\r' || !isprint((int)ch)) {
        return false;
    }

    if ((*line_len + 1U) < OTA_BOOT_REPORT_LINE_MAX_LEN) {
        line[(*line_len)++] = (char)ch;
    }

    return true;
}

static void ota_boot_report_set(ota_stm32_boot_report_t *report,
                                ota_boot_report_outcome_t outcome,
                                const char *code)
{
    if (report == NULL) {
        return;
    }

    report->received_any = true;
    report->outcome = outcome;
    if (code == NULL) {
        report->result_code[0] = '\0';
    } else {
        snprintf(report->result_code, sizeof(report->result_code), "%s", code);
    }
}

static uint32_t ota_data_session_id_from_fingerprint(const uint8_t fingerprint[OTA_CTRL_FINGERPRINT_LEN])
{
    return esp_rom_crc32_le(0U, fingerprint, OTA_CTRL_FINGERPRINT_LEN);
}

static uint16_t ota_data_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0U;

    while (length-- > 0U) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint8_t i = 0U; i < 8U; ++i) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static bool ota_data_send_frame(uint8_t type,
                                uint32_t session_id,
                                uint32_t offset,
                                const uint8_t *payload,
                                uint16_t payload_len)
{
    uint8_t frame[OTA_DATA_MAX_FRAME_LEN];
    uint16_t header_crc = 0U;
    uint32_t payload_crc = 0U;
    size_t total_len = 0U;

    if (payload_len > OTA_DATA_MAX_PAYLOAD_LEN) {
        return false;
    }

    frame[0] = OTA_DATA_SOF1;
    frame[1] = OTA_DATA_SOF2;
    frame[2] = OTA_DATA_PROTOCOL_VERSION;
    frame[3] = type;
    ota_ctrl_write_u32le(&frame[4], session_id);
    ota_ctrl_write_u32le(&frame[8], offset);
    ota_ctrl_write_u16le(&frame[12], payload_len);
    header_crc = ota_data_crc16(&frame[2], 12U);
    ota_ctrl_write_u16le(&frame[14], header_crc);

    if (payload_len > 0U && payload != NULL) {
        memcpy(&frame[OTA_DATA_FIXED_HEADER_LEN], payload, payload_len);
    }

    payload_crc = (payload_len == 0U) ? 0U : esp_rom_crc32_le(0U, payload, payload_len);
    ota_ctrl_write_u32le(&frame[OTA_DATA_FIXED_HEADER_LEN + payload_len], payload_crc);
    total_len = OTA_DATA_FIXED_HEADER_LEN + payload_len + OTA_DATA_TRAILER_LEN;
    return uart_write_bytes(EX_UART_NUM, (const char *)frame, total_len) == (int)total_len;
}

static bool ota_data_receive_frame(ota_data_frame_t *frame, uint32_t timeout_ms)
{
    uint8_t ch = 0U;
    uint8_t header[14];
    uint8_t trailer[4];
    uint32_t waited_ms = 0U;

    if (frame == NULL) {
        return false;
    }

    memset(frame, 0, sizeof(*frame));

    while (waited_ms < timeout_ms) {
        if (!ota_ctrl_read_byte_timeout(&ch, 1U)) {
            waited_ms++;
            continue;
        }

        if (ch != OTA_DATA_SOF1) {
            continue;
        }

        if (!ota_ctrl_read_byte_timeout(&ch, 20U)) {
            return false;
        }

        if (ch != OTA_DATA_SOF2) {
            continue;
        }

        for (size_t i = 0U; i < sizeof(header); ++i) {
            if (!ota_ctrl_read_byte_timeout(&header[i], 20U)) {
                return false;
            }
        }

        if (header[0] != OTA_DATA_PROTOCOL_VERSION) {
            continue;
        }

        if (ota_ctrl_read_u16le(&header[12]) != ota_data_crc16(header, 12U)) {
            ESP_LOGW(TAG, "Data frame header CRC mismatch");
            continue;
        }

        frame->type = header[1];
        frame->session_id = ota_ctrl_read_u32le(&header[2]);
        frame->offset = ota_ctrl_read_u32le(&header[6]);
        frame->payload_len = ota_ctrl_read_u16le(&header[10]);
        if (frame->payload_len > OTA_DATA_MAX_PAYLOAD_LEN) {
            ESP_LOGW(TAG, "Data frame payload too large: %u", frame->payload_len);
            continue;
        }

        for (uint16_t i = 0U; i < frame->payload_len; ++i) {
            if (!ota_ctrl_read_byte_timeout(&frame->payload[i], 20U)) {
                return false;
            }
        }

        for (size_t i = 0U; i < sizeof(trailer); ++i) {
            if (!ota_ctrl_read_byte_timeout(&trailer[i], 20U)) {
                return false;
            }
        }

        if (ota_ctrl_read_u32le(trailer) != esp_rom_crc32_le(0U, frame->payload, frame->payload_len)) {
            ESP_LOGW(TAG, "Data frame payload CRC mismatch");
            continue;
        }

        return true;
    }

    return false;
}

static void ota_ctr_build_counter(uint8_t counter[OTA_AES_BLOCK_SIZE],
                                  const uint8_t iv[OTA_AES_BLOCK_SIZE],
                                  uint32_t block_index)
{
    uint32_t carry = block_index;

    memcpy(counter, iv, OTA_AES_BLOCK_SIZE);
    for (int index = OTA_AES_BLOCK_SIZE - 1; index >= 0; --index) {
        carry += (uint32_t)counter[index];
        counter[index] = (uint8_t)(carry & 0xFFU);
        carry >>= 8;
    }
}

static bool ota_ctr_crypt_buffer(mbedtls_aes_context *aes,
                                 const uint8_t iv[OTA_AES_BLOCK_SIZE],
                                 uint32_t offset,
                                 uint8_t *buffer,
                                 size_t length)
{
    uint8_t counter[OTA_AES_BLOCK_SIZE];
    uint8_t keystream[OTA_AES_BLOCK_SIZE];
    uint32_t block_index = offset / OTA_AES_BLOCK_SIZE;
    uint32_t block_offset = offset % OTA_AES_BLOCK_SIZE;
    size_t processed = 0U;

    if (aes == NULL || iv == NULL || buffer == NULL) {
        return false;
    }

    while (processed < length) {
        size_t chunk = OTA_AES_BLOCK_SIZE - block_offset;
        int ret = 0;

        if (chunk > (length - processed)) {
            chunk = length - processed;
        }

        ota_ctr_build_counter(counter, iv, block_index);
        ret = mbedtls_aes_crypt_ecb(aes, MBEDTLS_AES_ENCRYPT, counter, keystream);
        if (ret != 0) {
            ESP_LOGE(TAG, "AES-CTR keystream generation failed: %d", ret);
            return false;
        }

        for (size_t i = 0U; i < chunk; ++i) {
            buffer[processed + i] ^= keystream[block_offset + i];
        }

        processed += chunk;
        block_index++;
        block_offset = 0U;
    }

    return true;
}

static bool ota_discard_plain_bytes(zip_entry_stream_t *stream, size_t discard_len)
{
    uint8_t buffer[OTA_STREAM_BUFFER_SIZE];
    size_t remaining = discard_len;

    while (remaining > 0U) {
        size_t chunk_len = remaining;
        size_t bytes_read = 0U;

        if (chunk_len > sizeof(buffer)) {
            chunk_len = sizeof(buffer);
        }

        if (!zip_entry_stream_read(stream, buffer, chunk_len, &bytes_read) || bytes_read != chunk_len) {
            ESP_LOGE(TAG, "Failed to discard firmware prefix: need=%u got=%u",
                     (unsigned)chunk_len,
                     (unsigned)bytes_read);
            return false;
        }

        remaining -= bytes_read;
    }

    return true;
}

static bool ota_wait_for_response(uint32_t session_id, ota_data_frame_t *response, uint32_t timeout_ms)
{
    uint32_t waited_ms = 0U;

    while (waited_ms < timeout_ms) {
        if (!ota_data_receive_frame(response, 200U)) {
            waited_ms += 200U;
            continue;
        }

        if (response->session_id != session_id) {
            ESP_LOGW(TAG, "Ignore data response for foreign session: 0x%08" PRIX32, response->session_id);
            continue;
        }

        return true;
    }

    return false;
}

static uint32_t ota_data_response_timeout_ms(uint8_t type)
{
    if (type == OTA_DATA_TYPE_CHUNK) {
        return 1000U;
    }

    return 5000U;
}

static uint32_t ota_data_retry_limit(uint8_t type)
{
    if (type == OTA_DATA_TYPE_CHUNK) {
        return 2U;
    }

    return OTA_DATA_MAX_RETRIES;
}

static bool ota_send_with_retry(uint8_t type,
                                uint32_t session_id,
                                uint32_t frame_offset,
                                const uint8_t *payload,
                                uint16_t payload_len,
                                uint32_t expect_ack_offset)
{
    ota_data_frame_t response;
    uint32_t response_timeout_ms = ota_data_response_timeout_ms(type);
    uint32_t retry_limit = ota_data_retry_limit(type);

    for (uint32_t retry = 0U; retry < retry_limit; ++retry) {
        if (!ota_data_send_frame(type, session_id, frame_offset, payload, payload_len)) {
            return false;
        }

        if (!ota_wait_for_response(session_id, &response, response_timeout_ms)) {
            ESP_LOGW(TAG,
                     "Data response timeout type=0x%02X retry=%u/%u timeout=%ums",
                     type,
                     (unsigned)(retry + 1U),
                     (unsigned)retry_limit,
                     (unsigned)response_timeout_ms);
            continue;
        }

        if (response.type == OTA_DATA_TYPE_ACK) {
            if (response.offset == expect_ack_offset) {
                return true;
            }

            ESP_LOGW(TAG,
                     "Unexpected ACK offset=%" PRIu32 " expected=%" PRIu32,
                     response.offset,
                     expect_ack_offset);
            continue;
        }

        if (response.type == OTA_DATA_TYPE_NAK) {
            uint16_t reason = (response.payload_len >= 2U) ? ota_ctrl_read_u16le(&response.payload[0]) : 0U;
            uint16_t detail = (response.payload_len >= 4U) ? ota_ctrl_read_u16le(&response.payload[2]) : 0U;
            ESP_LOGW(TAG,
                     "RX NAK type=0x%02X retry=%u offset=%" PRIu32 " reason=%u detail=%u",
                     type,
                     (unsigned)(retry + 1U),
                     response.offset,
                     reason,
                     detail);
            continue;
        }

        if (response.type == OTA_DATA_TYPE_ABORT) {
            uint8_t stage = (response.payload_len >= 1U) ? response.payload[0] : 0U;
            uint8_t error_class = (response.payload_len >= 2U) ? response.payload[1] : 0U;
            uint16_t error_code = (response.payload_len >= 4U) ? ota_ctrl_read_u16le(&response.payload[2]) : 0U;
            ESP_LOGE(TAG,
                     "RX ABORT type=0x%02X stage=%u class=%u code=%u offset=%" PRIu32,
                     type,
                     stage,
                     error_class,
                     error_code,
                     response.offset);
            return false;
        }
    }

    return false;
}

static void ota_log_tx_progress(uint32_t transferred,
                                uint32_t total,
                                int *last_percent_bucket)
{
    uint32_t percent = 0U;
    int current_bucket = 0;

    if (last_percent_bucket == NULL || total == 0U) {
        return;
    }

    percent = (transferred >= total) ? 100U : ((transferred * 100U) / total);
    current_bucket = (int)(percent / 5U);
    if (current_bucket != *last_percent_bucket || percent == 100U) {
        ESP_LOGI(TAG, "OTA DATA TX %u%% (%u/%u)", (unsigned)percent, (unsigned)transferred, (unsigned)total);
        *last_percent_bucket = current_bucket;
    }
}

static bool ota_try_parse_result_frame(ota_stm32_boot_report_t *report)
{
    ota_ctrl_frame_t frame = {0};
    ota_ctrl_result_info_t result = {0};

    if (!ota_ctrl_receive_frame(&frame, 100U)) {
        return false;
    }

    if (frame.msg_type != OTA_CTRL_MSG_RESULT || frame.payload_len < OTA_CTRL_RESULT_PAYLOAD_LEN) {
        return false;
    }

    result.outcome = frame.payload[0];
    result.stage = frame.payload[1];
    result.error_code = ota_ctrl_read_u16le(&frame.payload[2]);
    result.final_offset = ota_ctrl_read_u32le(&frame.payload[4]);

    switch (result.outcome) {
    case OTA_CTRL_RESULT_OUTCOME_SUCCESS:
        ota_boot_report_set(report, OTA_BOOT_REPORT_OUTCOME_SUCCESS, "RESULT OK");
        ESP_LOGI(TAG,
                 "STM32 result: success stage=%u offset=%" PRIu32,
                 result.stage,
                 result.final_offset);
        break;
    case OTA_CTRL_RESULT_OUTCOME_TERMINAL:
        ota_boot_report_set(report, OTA_BOOT_REPORT_OUTCOME_TERMINAL, "RESULT TERM");
        ESP_LOGE(TAG,
                 "STM32 result: terminal stage=%u err=%u offset=%" PRIu32,
                 result.stage,
                 result.error_code,
                 result.final_offset);
        break;
    case OTA_CTRL_RESULT_OUTCOME_RETRYABLE:
        ota_boot_report_set(report, OTA_BOOT_REPORT_OUTCOME_RETRYABLE, "RESULT RETRY");
        ESP_LOGW(TAG,
                 "STM32 result: retryable stage=%u err=%u offset=%" PRIu32,
                 result.stage,
                 result.error_code,
                 result.final_offset);
        break;
    default:
        ota_boot_report_set(report, OTA_BOOT_REPORT_OUTCOME_RETRYABLE, "RESULT ?");
        ESP_LOGW(TAG,
                 "STM32 result: unknown outcome=%u stage=%u err=%u offset=%" PRIu32,
                 result.outcome,
                 result.stage,
                 result.error_code,
                 result.final_offset);
        break;
    }

    return true;
}

void build_transfer_file_name(const char *original_name, char *buffer, size_t buffer_len)
{
    const char *file_name = original_name;
    const char *slash = strrchr(original_name, '/');
    const char *backslash = strrchr(original_name, '\\');
    const char *separator = slash;

    if (separator == NULL || (backslash != NULL && backslash > separator)) {
        separator = backslash;
    }

    if (separator != NULL && separator[1] != '\0') {
        file_name = separator + 1;
    }

    {
        const char *dot = strrchr(file_name, '.');
        if (dot == NULL || dot == file_name) {
            snprintf(buffer, buffer_len, "%s_ctr.bin", file_name);
            return;
        }

        snprintf(buffer, buffer_len, "%.*s_ctr%s", (int)(dot - file_name), file_name, dot);
    }
}

void ota_log_stm32_boot_report_line(const char *line, ota_stm32_boot_report_t *report)
{
    const char *code = NULL;

    if (line == NULL || line[0] == '\0') {
        return;
    }

    if (strncmp(line, "[BOOT] RES ", 11) == 0) {
        code = line + 11;
        if (strcmp(code, "OK") == 0) {
            ota_boot_report_set(report, OTA_BOOT_REPORT_OUTCOME_SUCCESS, code);
            ESP_LOGI(TAG, "STM32 boot result: upgrade authorized, jumping to new APP");
            return;
        }
        if (strncmp(code, "AUTH ", 5) == 0 || strcmp(code, "HDR") == 0 || strcmp(code, "APPBAD") == 0) {
            ota_boot_report_set(report, OTA_BOOT_REPORT_OUTCOME_TERMINAL, code);
            ESP_LOGE(TAG, "STM32 boot result: %s", code);
            return;
        }

        ota_boot_report_set(report, OTA_BOOT_REPORT_OUTCOME_RETRYABLE, code);
        ESP_LOGW(TAG, "STM32 boot result: %s", code);
        return;
    }

    if (strncmp(line, "[BOOT] SHA ", 11) == 0) {
        ESP_LOGI(TAG, "STM32 SHA summary: %s", line + 11);
        return;
    }

    if (strncmp(line, "[BOOT] UART ", 12) == 0) {
        ESP_LOGW(TAG, "STM32 UART RX diagnostics:%s", line + 11);
        return;
    }

    ESP_LOGI(TAG, "STM32 post-transfer: %s", line);
}

ota_stm32_boot_report_t ota_read_stm32_boot_report(void)
{
    return ota_read_stm32_boot_report_with_timeouts(OTA_BOOT_REPORT_TOTAL_TIMEOUT_MS,
                                                    OTA_BOOT_REPORT_IDLE_TIMEOUT_MS);
}

ota_stm32_boot_report_t ota_read_stm32_boot_report_with_timeouts(uint32_t total_timeout_ms,
                                                                 uint32_t idle_timeout_ms)
{
    ota_stm32_boot_report_t report = {0};
    char line[OTA_BOOT_REPORT_LINE_MAX_LEN];
    size_t line_len = 0U;
    uint32_t waited_ms = 0U;
    uint32_t idle_ms = 0U;
    bool got_any = false;

    memset(line, 0, sizeof(line));

    while (waited_ms < total_timeout_ms) {
        uint8_t ch = 0U;
        waited_ms += 50U;
        if (!ota_ctrl_read_byte_timeout(&ch, 50U)) {
            if (got_any) {
                idle_ms += 50U;
                if (idle_ms >= idle_timeout_ms) {
                    break;
                }
            }
            continue;
        }

        got_any = true;
        idle_ms = 0U;

        if (ch == OTA_CTRL_SOF1) {
            uint8_t next = 0U;
            if (ota_ctrl_read_byte_timeout(&next, 5U) && next == OTA_CTRL_SOF2) {
                uint8_t sof[2];

                sof[0] = OTA_CTRL_SOF1;
                sof[1] = OTA_CTRL_SOF2;
                ota_ctrl_pushback_bytes(sof, sizeof(sof));
                if (ota_try_parse_result_frame(&report)) {
                    break;
                }
                continue;
            }

            (void)ota_boot_report_append_char(line, &line_len, ch, &report);
            if (next != 0U) {
                (void)ota_boot_report_append_char(line, &line_len, next, &report);
            }
            continue;
        }

        (void)ota_boot_report_append_char(line, &line_len, ch, &report);
    }

    if (line_len > 0U) {
        line[line_len] = '\0';
        ota_log_stm32_boot_report_line(line, &report);
    }

    if (!got_any) {
        ESP_LOGW(TAG, "No STM32 post-transfer report received before timeout");
    }

    return report;
}

bool ymodem_send_encrypted_stream(const ota_iap_context_t *context,
                                  const char *transfer_file_name,
                                  size_t start_transfer_offset)
{
    zip_entry_stream_t stream;
    mbedtls_aes_context aes;
    uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN];
    uint8_t iv[OTA_AES_BLOCK_SIZE];
    uint8_t start_payload[OTA_DATA_START_PAYLOAD_LEN];
    uint8_t plain[OTA_DATA_DEFAULT_CHUNK_SIZE];
    uint8_t cipher[OTA_DATA_DEFAULT_CHUNK_SIZE];
    uint32_t session_id = 0U;
    uint32_t transfer_size = 0U;
    uint32_t offset = 0U;
    int last_percent_bucket = -1;
    bool ok = false;

    if (context == NULL || transfer_file_name == NULL) {
        return false;
    }

    transfer_size = (uint32_t)context->manifest.firmware_size;
    if (start_transfer_offset > transfer_size ||
        (start_transfer_offset != 0U &&
         ((start_transfer_offset % OTA_DATA_DEFAULT_CHUNK_SIZE) != 0U ||
          (start_transfer_offset % OTA_AES_BLOCK_SIZE) != 0U))) {
        ESP_LOGE(TAG,
                 "Invalid transfer start offset=%u size=%u chunk=%u",
                 (unsigned)start_transfer_offset,
                 (unsigned)transfer_size,
                 (unsigned)OTA_DATA_DEFAULT_CHUNK_SIZE);
        return false;
    }

    if (!ota_compute_session_fingerprint(context, session_fingerprint) ||
        !hex_to_bytes(context->manifest.encryption_iv_hex, iv, sizeof(iv))) {
        return false;
    }

    session_id = ota_data_session_id_from_fingerprint(session_fingerprint);
    offset = (uint32_t)start_transfer_offset;

    memset(&stream, 0, sizeof(stream));
    if (!zip_entry_stream_init(&stream,
                               &context->package_blob,
                               "firmware.bin",
                               &context->firmware_entry)) {
        return false;
    }

    if (start_transfer_offset > 0U && !ota_discard_plain_bytes(&stream, start_transfer_offset)) {
        zip_entry_stream_free(&stream);
        return false;
    }

    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, g_ota_aes_key, OTA_AES_KEY_BYTES * 8U) != 0) {
        ESP_LOGE(TAG, "AES setkey failed");
        goto cleanup;
    }

    memset(start_payload, 0, sizeof(start_payload));
    ota_ctrl_write_u16le(&start_payload[0], (start_transfer_offset != 0U) ? OTA_DATA_START_FLAG_RESUME : 0U);
    ota_ctrl_write_u16le(&start_payload[2], OTA_DATA_DEFAULT_CHUNK_SIZE);
    ota_ctrl_write_u32le(&start_payload[4], OTA_TRANSFER_CHECKPOINT_SIZE);
    ota_ctrl_write_u32le(&start_payload[8], transfer_size);
    ota_ctrl_write_u32le(&start_payload[12], transfer_size);
    memcpy(&start_payload[16], session_fingerprint, OTA_CTRL_FINGERPRINT_LEN);

    ESP_LOGI(TAG,
             "%s OTA data transmit: %s, size=%u bytes, start=%u",
             (start_transfer_offset == 0U) ? "Start" : "Resume",
             transfer_file_name,
             (unsigned)transfer_size,
             (unsigned)start_transfer_offset);

    if (!ota_send_with_retry(OTA_DATA_TYPE_START,
                             session_id,
                             offset,
                             start_payload,
                             sizeof(start_payload),
                             offset)) {
        ESP_LOGE(TAG, "START frame exchange failed");
        goto cleanup;
    }

    while (offset < transfer_size) {
        size_t chunk_len = transfer_size - offset;
        size_t bytes_read = 0U;

        if (chunk_len > sizeof(plain)) {
            chunk_len = sizeof(plain);
        }

        if (!zip_entry_stream_read(&stream, plain, chunk_len, &bytes_read) || bytes_read != chunk_len) {
            ESP_LOGE(TAG, "Firmware stream read failed at offset=%u", (unsigned)offset);
            goto cleanup;
        }

        memcpy(cipher, plain, chunk_len);
        if (!ota_ctr_crypt_buffer(&aes, iv, offset, cipher, chunk_len)) {
            goto cleanup;
        }

        if (!ota_send_with_retry(OTA_DATA_TYPE_CHUNK,
                                 session_id,
                                 offset,
                                 cipher,
                                 (uint16_t)chunk_len,
                                 offset + (uint32_t)chunk_len)) {
            ESP_LOGE(TAG, "CHUNK exchange failed at offset=%u", (unsigned)offset);
            goto cleanup;
        }

        offset += (uint32_t)chunk_len;
        ota_log_tx_progress(offset, transfer_size, &last_percent_bucket);
    }

    if (!ota_send_with_retry(OTA_DATA_TYPE_FINISH, session_id, transfer_size, NULL, 0U, transfer_size)) {
        ESP_LOGE(TAG, "FINISH exchange failed");
        goto cleanup;
    }

    ok = true;

cleanup:
    zip_entry_stream_free(&stream);
    mbedtls_aes_free(&aes);
    return ok;
}
