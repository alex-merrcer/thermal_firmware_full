#include "ota_stm32_ctrl.h"
#include "../../../../protocol/ota_ctrl_protocol_text.h"

#define TAG OTA_STM32_TAG
#define OTA_CTRL_PUSHBACK_CAPACITY 8U

static uint8_t s_ctrl_pushback[OTA_CTRL_PUSHBACK_CAPACITY];
static size_t s_ctrl_pushback_len = 0U;

static const char *ota_ctrl_resume_reason_name(uint16_t reason_code)
{
    switch (reason_code) {
    case OTA_CTRL_RESUME_REASON_OK:
        return "OK";
    case OTA_CTRL_RESUME_REASON_STATE:
        return "STATE";
    case OTA_CTRL_RESUME_REASON_REQ_TYPE:
        return "REQ_TYPE";
    case OTA_CTRL_RESUME_REASON_ACTIVE_SLOT:
        return "ACTIVE_SLOT";
    case OTA_CTRL_RESUME_REASON_TARGET_SLOT:
        return "TARGET_SLOT";
    case OTA_CTRL_RESUME_REASON_PROTOCOL:
        return "PROTOCOL";
    case OTA_CTRL_RESUME_REASON_CURRENT_VERSION:
        return "CURRENT_VER";
    case OTA_CTRL_RESUME_REASON_TARGET_VERSION:
        return "TARGET_VER";
    case OTA_CTRL_RESUME_REASON_TRANSFER_SIZE:
        return "TRANSFER_SIZE";
    case OTA_CTRL_RESUME_REASON_PLAIN_SIZE:
        return "PLAIN_SIZE";
    case OTA_CTRL_RESUME_REASON_CHECKPOINT_SIZE:
        return "CHECKPOINT";
    case OTA_CTRL_RESUME_REASON_FINGERPRINT:
        return "FINGERPRINT";
    case OTA_CTRL_RESUME_REASON_OFFSET_ZERO:
        return "OFFSET_ZERO";
    case OTA_CTRL_RESUME_REASON_OFFSET_RANGE:
        return "OFFSET_RANGE";
    case OTA_CTRL_RESUME_REASON_OFFSET_CHECKPOINT:
        return "OFFSET_ALIGN";
    case OTA_CTRL_RESUME_REASON_OFFSET_BLOCK:
        return "OFFSET_AES";
    default:
        return NULL;
    }
}

static const char *ota_ctrl_txn_load_source_name(uint16_t source)
{
    switch (source) {
    case OTA_CTRL_TXN_LOAD_SRC_VALID:
        return "VALID";
    case OTA_CTRL_TXN_LOAD_SRC_EMPTY:
        return "EMPTY";
    case OTA_CTRL_TXN_LOAD_SRC_INVALID:
        return "INVALID";
    default:
        return "NONE";
    }
}

static bool ota_ctrl_pop_pushback(uint8_t *byte)
{
    if (byte == NULL || s_ctrl_pushback_len == 0U) {
        return false;
    }

    *byte = s_ctrl_pushback[0];
    s_ctrl_pushback_len--;
    if (s_ctrl_pushback_len > 0U) {
        memmove(s_ctrl_pushback, &s_ctrl_pushback[1], s_ctrl_pushback_len);
    }

    return true;
}

void ota_ctrl_log_status_event(const char *direction,
                               uint8_t stage,
                               uint8_t percent,
                               uint16_t detail_code,
                               uint32_t current_value,
                               uint32_t total_value)
{
    if (percent == OTA_CTRL_PERCENT_UNKNOWN) {
        ESP_LOGI(TAG,
                 "%s STATUS stg=%u(%s) pct=-- det=%u val=%" PRIu32 "/%" PRIu32,
                 direction,
                 stage,
                 ota_ctrl_stage_name(stage),
                 detail_code,
                 current_value,
                 total_value);
        return;
    }

    ESP_LOGI(TAG,
             "%s STATUS stg=%u(%s) pct=%u det=%u val=%" PRIu32 "/%" PRIu32,
             direction,
             stage,
             ota_ctrl_stage_name(stage),
             percent,
             detail_code,
             current_value,
             total_value);
}

void ota_ctrl_log_error_event(const char *direction,
                              uint8_t stage,
                              uint16_t error_code)
{
    ESP_LOGE(TAG,
             "%s ERROR stg=%u(%s) err=%u(%s)",
             direction,
             stage,
             ota_ctrl_stage_name(stage),
             error_code,
             ota_ctrl_error_name(error_code));
}

void ota_ctrl_log_ack_event(const char *direction,
                            uint8_t seq,
                            bool accept,
                            uint8_t target_partition,
                            uint16_t reason_code)
{
    ESP_LOGI(TAG,
             "%s ACK seq=%u accept=%u target=APP%u reason=%u(%s)",
             direction,
             seq,
             accept ? 1U : 0U,
             (unsigned int)target_partition + 1U,
             reason_code,
             reason_code == 0U ? "OK" : ota_ctrl_error_name(reason_code));
}

void ota_ctrl_log_ready_event(const char *direction,
                              uint8_t target_partition,
                              const char *release_version,
                              uint16_t ready_flags,
                              uint32_t plain_size,
                              uint32_t transfer_size,
                              uint32_t checkpoint_size,
                              const uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN])
{
    const uint8_t *fingerprint = session_fingerprint;

    ESP_LOGI(TAG,
             "%s READY stg=%u(%s) target=APP%u rel=%s flags=0x%04X plain=%" PRIu32 " transfer=%" PRIu32 " checkpoint=%" PRIu32 " fp=%02X%02X%02X%02X",
             direction,
             OTA_CTRL_STAGE_READY,
             ota_ctrl_stage_name(OTA_CTRL_STAGE_READY),
             (unsigned int)target_partition + 1U,
             release_version != NULL ? release_version : "-",
             ready_flags,
             plain_size,
             transfer_size,
             checkpoint_size,
             fingerprint != NULL ? fingerprint[0] : 0U,
             fingerprint != NULL ? fingerprint[1] : 0U,
             fingerprint != NULL ? fingerprint[2] : 0U,
             fingerprint != NULL ? fingerprint[3] : 0U);
}

void ota_ctrl_log_request_event(const char *direction,
                                uint8_t seq,
                                const ota_upgrade_request_t *request)
{
    if (request == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "%s %s seq=%u active=APP%u target=APP%u product=%s hw=%s ver=%s valid=%u flags=0x%08" PRIX32,
             direction,
             ota_ctrl_msg_name(OTA_CTRL_MSG_REQ),
             seq,
             (unsigned int)request->active_partition + 1U,
             (unsigned int)request->target_partition + 1U,
             request->product_id[0] != '\0' ? request->product_id : OTA_SUPPORTED_PRODUCT_ID,
             request->hw_rev[0] != '\0' ? request->hw_rev : OTA_SUPPORTED_HW_REV,
             request->version_valid ? request->current_version : "unknown",
             request->version_valid ? 1U : 0U,
             request->flags);
}

uint16_t ota_ctrl_crc16(const uint8_t *data, uint16_t length)
{
    /* OTA control-frame integrity check.
     * CRC-16/CCITT-FALSE: poly=0x1021, init=0x0000, no xorout. */
    uint16_t crc = 0U;

    while (length-- > 0U) {
        /* Merge current byte into high 8 bits, then update bit by bit. */
        crc ^= (uint16_t)(*data++) << 8;
        for (uint8_t i = 0U; i < 8U; ++i) {
            if ((crc & 0x8000U) != 0U) {
                /* MSB=1: left shift and XOR with generator polynomial. */
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                /* MSB=0: left shift only. */
                crc <<= 1;
            }
        }
    }

    return crc;
}

void ota_ctrl_write_u16le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

void ota_ctrl_write_u32le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

uint16_t ota_ctrl_read_u16le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

uint32_t ota_ctrl_read_u32le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

void ota_ctrl_copy_fixed_string(char *target,
                                size_t target_len,
                                const uint8_t *source,
                                size_t source_len)
{
    size_t copy_len = 0U;

    if (target == NULL || target_len == 0U) {
        return;
    }

    memset(target, 0, target_len);
    while (copy_len < source_len && copy_len + 1U < target_len && source[copy_len] != '\0') {
        target[copy_len] = (char)source[copy_len];
        copy_len++;
    }
}

bool ota_ctrl_read_byte_timeout(uint8_t *byte, uint32_t timeout_ms)
{
    if (byte == NULL) {
        return false;
    }

    if (ota_ctrl_pop_pushback(byte)) {
        return true;
    }

    return uart_read_bytes(EX_UART_NUM, byte, 1, pdMS_TO_TICKS(timeout_ms)) == 1;
}

void ota_ctrl_flush_uart(void)
{
    s_ctrl_pushback_len = 0U;
    uart_flush_input(EX_UART_NUM);
}

void ota_ctrl_pushback_bytes(const uint8_t *data, size_t data_len)
{
    if (data == NULL || data_len == 0U || data_len > OTA_CTRL_PUSHBACK_CAPACITY) {
        return;
    }

    if ((s_ctrl_pushback_len + data_len) > OTA_CTRL_PUSHBACK_CAPACITY) {
        return;
    }

    memmove(&s_ctrl_pushback[data_len], s_ctrl_pushback, s_ctrl_pushback_len);
    memcpy(s_ctrl_pushback, data, data_len);
    s_ctrl_pushback_len += data_len;
}

bool ota_ctrl_send_frame(uint8_t msg_type,
                         uint8_t seq,
                         const uint8_t *payload,
                         uint16_t payload_len)
{
    uint8_t frame[OTA_CTRL_MAX_FRAME_LEN];
    uint16_t crc = 0U;
    size_t total_len = 0U;

    if (payload_len > OTA_CTRL_MAX_PAYLOAD_LEN) {
        return false;
    }

    frame[0] = OTA_CTRL_SOF1;
    frame[1] = OTA_CTRL_SOF2;
    frame[2] = OTA_CTRL_PROTOCOL_VERSION;
    frame[3] = msg_type;
    frame[4] = seq;
    ota_ctrl_write_u16le(&frame[5], payload_len);

    if (payload_len > 0U && payload != NULL) {
        memcpy(&frame[OTA_CTRL_HEADER_LEN], payload, payload_len);
    }

    crc = ota_ctrl_crc16(&frame[2], (uint16_t)(5U + payload_len));
    ota_ctrl_write_u16le(&frame[OTA_CTRL_HEADER_LEN + payload_len], crc);
    total_len = OTA_CTRL_FRAME_OVERHEAD + payload_len;

    return uart_write_bytes(EX_UART_NUM, (const char *)frame, total_len) == (int)total_len;
}

bool ota_ctrl_receive_frame(ota_ctrl_frame_t *frame, uint32_t timeout_ms)
{
    uint8_t ch = 0U;
    uint8_t header[5];
    uint8_t crc_bytes[2];
    uint8_t crc_buffer[5U + OTA_CTRL_MAX_PAYLOAD_LEN];
    uint32_t waited_ms = 0U;

    if (frame == NULL) {
        return false;
    }

    while (waited_ms < timeout_ms) {
        if (!ota_ctrl_read_byte_timeout(&ch, 1U)) {
            waited_ms++;
            continue;
        }

        if (ch != OTA_CTRL_SOF1) {
            continue;
        }

        if (!ota_ctrl_read_byte_timeout(&ch, 20U)) {
            return false;
        }

        if (ch != OTA_CTRL_SOF2) {
            continue;
        }

        for (size_t i = 0U; i < sizeof(header); ++i) {
            if (!ota_ctrl_read_byte_timeout(&header[i], 20U)) {
                return false;
            }
        }

        if (header[0] != OTA_CTRL_PROTOCOL_VERSION) {
            continue;
        }

        frame->msg_type = header[1];
        frame->seq = header[2];
        frame->payload_len = ota_ctrl_read_u16le(&header[3]);
        if (frame->payload_len > OTA_CTRL_MAX_PAYLOAD_LEN) {
            ota_ctrl_flush_uart();
            return false;
        }

        for (uint16_t i = 0U; i < frame->payload_len; ++i) {
            if (!ota_ctrl_read_byte_timeout(&frame->payload[i], 20U)) {
                return false;
            }
        }

        if (!ota_ctrl_read_byte_timeout(&crc_bytes[0], 20U) ||
            !ota_ctrl_read_byte_timeout(&crc_bytes[1], 20U)) {
            return false;
        }

        {
            uint16_t crc_recv = ota_ctrl_read_u16le(crc_bytes);
            uint16_t crc_calc = 0U;

            memcpy(crc_buffer, header, sizeof(header));
            if (frame->payload_len > 0U) {
                memcpy(&crc_buffer[sizeof(header)], frame->payload, frame->payload_len);
            }

            crc_calc = ota_ctrl_crc16(crc_buffer, (uint16_t)(sizeof(header) + frame->payload_len));
            if (crc_recv != crc_calc) {
                ESP_LOGW(TAG, "Control frame CRC mismatch");
                continue;
            }
        }

        return true;
    }

    return false;
}

bool ota_ctrl_parse_request_payload(const uint8_t *payload,
                                    size_t payload_len,
                                    ota_upgrade_request_t *request)
{
    if (payload == NULL || request == NULL || payload_len < OTA_CTRL_REQ_PAYLOAD_LEN) {
        return false;
    }

    memset(request, 0, sizeof(*request));
    request->request_type = payload[0];
    request->active_partition = payload[1];
    request->target_partition = payload[2];
    request->version_valid = payload[3] != 0U;

    ota_ctrl_copy_fixed_string(request->current_version,
                               sizeof(request->current_version),
                               &payload[4],
                               OTA_CTRL_VERSION_LEN);
    ota_ctrl_copy_fixed_string(request->product_id,
                               sizeof(request->product_id),
                               &payload[20],
                               OTA_CTRL_PRODUCT_ID_LEN);
    ota_ctrl_copy_fixed_string(request->hw_rev,
                               sizeof(request->hw_rev),
                               &payload[36],
                               OTA_CTRL_HW_REV_LEN);
    memcpy(request->device_uid, &payload[44], OTA_CTRL_UID_LEN);
    request->flags = ota_ctrl_read_u32le(&payload[56]);
    return true;
}

bool ota_ctrl_send_ack(uint8_t seq,
                       const ota_upgrade_request_t *request,
                       bool accept,
                       uint16_t reason_code)
{
    uint8_t payload[OTA_CTRL_ACK_PAYLOAD_LEN] = {0};
    uint8_t target_partition = 0U;

    payload[0] = accept ? 1U : 0U;
    if (request != NULL) {
        payload[1] = request->request_type;
        payload[2] = request->target_partition;
        target_partition = request->target_partition;
    }

    ota_ctrl_write_u16le(&payload[4], reason_code);
    ota_ctrl_log_ack_event("TX", seq, accept, target_partition, reason_code);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_ACK, seq, payload, OTA_CTRL_ACK_PAYLOAD_LEN);
}

bool ota_ctrl_send_status(uint8_t seq,
                          uint8_t stage,
                          uint8_t percent,
                          uint16_t detail_code,
                          uint32_t current_value,
                          uint32_t total_value)
{
    uint8_t payload[OTA_CTRL_STATUS_PAYLOAD_LEN] = {0};

    ota_ctrl_log_status_event("TX", stage, percent, detail_code, current_value, total_value);
    payload[0] = stage;
    payload[1] = percent;
    ota_ctrl_write_u16le(&payload[2], detail_code);
    ota_ctrl_write_u32le(&payload[4], current_value);
    ota_ctrl_write_u32le(&payload[8], total_value);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_STATUS, seq, payload, OTA_CTRL_STATUS_PAYLOAD_LEN);
}

bool ota_ctrl_send_ready(uint8_t seq,
                         const ota_upgrade_request_t *request,
                         const char *release_version,
                         uint16_t ready_flags,
                         uint32_t plain_size,
                         uint32_t transfer_size,
                         uint32_t checkpoint_size,
                         const uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN])
{
    uint8_t payload[OTA_CTRL_READY_PAYLOAD_LEN] = {0};
    const char *version_text = release_version;

    if (version_text == NULL || version_text[0] == '\0') {
        version_text = OTA_DEFAULT_DEVICE_VERSION;
    }

    payload[0] = request->target_partition;
    ota_ctrl_write_u16le(&payload[2], ready_flags);
    ota_ctrl_copy_fixed_string((char *)&payload[4],
                               OTA_CTRL_VERSION_LEN,
                               (const uint8_t *)version_text,
                               strlen(version_text));
    ota_ctrl_write_u32le(&payload[20], plain_size);
    ota_ctrl_write_u32le(&payload[24], transfer_size);
    ota_ctrl_write_u32le(&payload[28], checkpoint_size);
    if (session_fingerprint != NULL) {
        memcpy(&payload[32], session_fingerprint, OTA_CTRL_FINGERPRINT_LEN);
    }

    ota_ctrl_log_ready_event("TX",
                             request->target_partition,
                             version_text,
                             ready_flags,
                             plain_size,
                             transfer_size,
                             checkpoint_size,
                             session_fingerprint);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_READY, seq, payload, OTA_CTRL_READY_PAYLOAD_LEN);
}

bool ota_ctrl_send_error(uint8_t seq, uint8_t stage, uint16_t error_code)
{
    uint8_t payload[OTA_CTRL_ERROR_PAYLOAD_LEN] = {0};

    ota_ctrl_log_error_event("TX", stage, error_code);
    payload[0] = stage;
    ota_ctrl_write_u16le(&payload[2], error_code);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_ERROR, seq, payload, OTA_CTRL_ERROR_PAYLOAD_LEN);
}

uint16_t ota_ctrl_validate_request(const ota_upgrade_request_t *request)
{
    /* Validate OTA request before any transfer starts.
     * Return 0 on success, otherwise return protocol/business error code. */
    const char *product_id = OTA_SUPPORTED_PRODUCT_ID;

    if (request == NULL) {
        return OTA_CTRL_ERR_PROTOCOL;
    }

    if (request->request_type != OTA_CTRL_REQ_TYPE_UPGRADE) {
        /* Reject unsupported request type to keep protocol deterministic. */
        ESP_LOGE(TAG, "Unsupported request type: %u", request->request_type);
        return OTA_CTRL_ERR_PROTOCOL;
    }

    if (request->target_partition > OTA_CTRL_PARTITION_APP2) {
        /* Partition out of range -> prevent accidental overwrite. */
        ESP_LOGE(TAG, "Invalid target partition: %u", request->target_partition);
        return OTA_CTRL_ERR_PARTITION;
    }

    if (request->product_id[0] != '\0') {
        product_id = request->product_id;
    }

    if (strcasecmp(product_id, OTA_SUPPORTED_PRODUCT_ID) != 0) {
        /* Product mismatch -> block cross-product package flashing. */
        ESP_LOGE(TAG, "Unsupported product id: %s", product_id);
        return OTA_CTRL_ERR_PRODUCT;
    }

    if (request->hw_rev[0] != '\0' && strcasecmp(request->hw_rev, OTA_SUPPORTED_HW_REV) != 0) {
        /* Hardware revision mismatch -> avoid incompatible firmware upgrade. */
        ESP_LOGE(TAG, "Unsupported hardware revision: %s", request->hw_rev);
        return OTA_CTRL_ERR_HW_REV;
    }

    return 0U;
}

bool ota_request_is_check_only(const ota_upgrade_request_t *request)
{
    if (request == NULL) {
        return false;
    }

    return (request->flags & OTA_CTRL_REQ_FLAG_CHECK_ONLY) != 0U;
}

bool ota_ctrl_wait_for_request(ota_upgrade_request_t *request, uint8_t *seq)
{
    ota_ctrl_frame_t frame = {0};

    if (!ota_ctrl_receive_frame(&frame, OTA_CTRL_SERVICE_IDLE_MS)) {
        return false;
    }

    if (frame.msg_type != OTA_CTRL_MSG_REQ) {
        return false;
    }

    if (!ota_ctrl_parse_request_payload(frame.payload, frame.payload_len, request)) {
        ESP_LOGE(TAG, "Invalid request payload");
        return false;
    }

    if (seq != NULL) {
        *seq = frame.seq;
    }

    ota_ctrl_log_request_event("RX", frame.seq, request);
    return true;
}

static bool ota_ctrl_send_meta_chunks(uint8_t seq,
                                      uint8_t kind,
                                      const uint8_t *data,
                                      size_t total_len)
{
    uint8_t payload[OTA_CTRL_MAX_PAYLOAD_LEN];
    size_t offset = 0U;

    if (data == NULL || total_len == 0U || total_len > 0xFFFFU) {
        return false;
    }

    while (offset < total_len) {
        size_t chunk_len = total_len - offset;

        if (chunk_len > OTA_CTRL_META_MAX_CHUNK_LEN) {
            chunk_len = OTA_CTRL_META_MAX_CHUNK_LEN;
        }

        memset(payload, 0, sizeof(payload));
        payload[0] = kind;
        ota_ctrl_write_u16le(&payload[2], (uint16_t)offset);
        ota_ctrl_write_u16le(&payload[4], (uint16_t)chunk_len);
        ota_ctrl_write_u16le(&payload[6], (uint16_t)total_len);
        memcpy(&payload[OTA_CTRL_META_PAYLOAD_HDR_LEN], data + offset, chunk_len);

        if (!ota_ctrl_send_frame(OTA_CTRL_MSG_META,
                                 seq,
                                 payload,
                                 (uint16_t)(OTA_CTRL_META_PAYLOAD_HDR_LEN + chunk_len))) {
            return false;
        }

        offset += chunk_len;
    }

    return true;
}

bool ota_ctrl_send_image_header_meta(uint8_t seq, const OtaImageHeaderBinary *header)
{
    if (header == NULL) {
        return false;
    }

    ESP_LOGI(TAG, "TX META image-header=%u", (unsigned)OTA_IMAGE_HEADER_TOTAL_SIZE);
    if (!ota_ctrl_send_meta_chunks(seq,
                                   OTA_CTRL_META_KIND_IMAGE_HEADER,
                                   (const uint8_t *)header,
                                   OTA_IMAGE_HEADER_TOTAL_SIZE)) {
        return false;
    }

    return true;
}

bool ota_ctrl_wait_for_go(const ota_upgrade_request_t *request, ota_go_request_t *go_request)
{
    ota_ctrl_frame_t frame = {0};
    uint32_t waited_ms = 0U;

    if (go_request != NULL) {
        memset(go_request, 0, sizeof(*go_request));
    }

    while (waited_ms < OTA_CTRL_GO_TIMEOUT_MS) {
        if (!ota_ctrl_receive_frame(&frame, OTA_CTRL_FRAME_WAIT_MS)) {
            waited_ms += OTA_CTRL_FRAME_WAIT_MS;
            continue;
        }

        if (frame.msg_type == OTA_CTRL_MSG_CANCEL) {
            ESP_LOGW(TAG,
                     "RX %s seq=%u target=APP%u",
                     ota_ctrl_msg_name(OTA_CTRL_MSG_CANCEL),
                     frame.seq,
                     (unsigned int)request->target_partition + 1U);
            return false;
        }

        if (frame.msg_type == OTA_CTRL_MSG_STATUS &&
            frame.payload_len >= OTA_CTRL_STATUS_PAYLOAD_LEN) {
            uint8_t stage = frame.payload[0];
            uint8_t percent = frame.payload[1];
            uint16_t detail_code = ota_ctrl_read_u16le(&frame.payload[2]);
            uint32_t current_value = ota_ctrl_read_u32le(&frame.payload[4]);
            uint32_t total_value = ota_ctrl_read_u32le(&frame.payload[8]);
            const char *resume_reason = ota_ctrl_resume_reason_name(detail_code);
            uint16_t detail_group = (uint16_t)(detail_code & 0xFFF0U);
            uint16_t detail_source = (uint16_t)(detail_code & 0x000FU);

            ota_ctrl_log_status_event("RX",
                                      stage,
                                      percent,
                                      detail_code,
                                      current_value,
                                      total_value);

            if (stage == OTA_CTRL_STAGE_READY &&
                (detail_group == OTA_CTRL_DIAG_TXN_LOAD_CORE ||
                 detail_group == OTA_CTRL_DIAG_TXN_LOAD_OFFSETS ||
                 detail_group == OTA_CTRL_DIAG_TXN_LOAD_META ||
                 detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT1 ||
                 detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT2 ||
                 detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_TXN1 ||
                 detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_TXN2 ||
                 detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_TXN3 ||
                 detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_TXN4)) {
                if (detail_group == OTA_CTRL_DIAG_TXN_LOAD_CORE) {
                    ESP_LOGI(TAG,
                             "STM32 TXN LOAD: src=%s state=%" PRIu32 " proto=%" PRIu32,
                             ota_ctrl_txn_load_source_name(detail_source),
                             current_value,
                             total_value);
                } else if (detail_group == OTA_CTRL_DIAG_TXN_LOAD_OFFSETS) {
                    ESP_LOGI(TAG,
                             "STM32 TXN LOAD: src=%s off=%" PRIu32 " ack=%" PRIu32,
                             ota_ctrl_txn_load_source_name(detail_source),
                             current_value,
                             total_value);
                } else if (detail_group == OTA_CTRL_DIAG_TXN_LOAD_META) {
                    ESP_LOGI(TAG,
                             "STM32 TXN LOAD: src=%s ckpt=%" PRIu32 " invalid=%" PRIu32,
                             ota_ctrl_txn_load_source_name(detail_source),
                             current_value,
                             total_value);
                } else if (detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT1) {
                    ESP_LOGI(TAG,
                             "STM32 TXN INVALID: src=%s slot_hdr=%" PRIu32 " slot_commit=%" PRIu32,
                             ota_ctrl_txn_load_source_name(detail_source),
                             current_value,
                             total_value);
                } else if (detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT2) {
                    ESP_LOGI(TAG,
                             "STM32 TXN INVALID: src=%s slot_pcrc=%" PRIu32 " slot_pval=%" PRIu32,
                             ota_ctrl_txn_load_source_name(detail_source),
                             current_value,
                             total_value);
                } else if (detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_TXN1) {
                    ESP_LOGI(TAG,
                             "STM32 TXN INVALID: src=%s pay_layout=%" PRIu32 " pay_state=%" PRIu32,
                             ota_ctrl_txn_load_source_name(detail_source),
                             current_value,
                             total_value);
                } else if (detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_TXN2) {
                    ESP_LOGI(TAG,
                             "STM32 TXN INVALID: src=%s pay_part=%" PRIu32 " pay_fields=%" PRIu32,
                             ota_ctrl_txn_load_source_name(detail_source),
                             current_value,
                             total_value);
                } else if (detail_group == OTA_CTRL_DIAG_TXN_LOAD_INV_TXN3) {
                    ESP_LOGI(TAG,
                             "STM32 TXN INVALID: src=%s pay_req=%" PRIu32 " pay_ver=%" PRIu32,
                             ota_ctrl_txn_load_source_name(detail_source),
                             current_value,
                             total_value);
                } else {
                    ESP_LOGI(TAG,
                             "STM32 TXN INVALID: src=%s pay_off=%" PRIu32 " pay_crc=%" PRIu32,
                             ota_ctrl_txn_load_source_name(detail_source),
                             current_value,
                             total_value);
                }
                continue;
            }

            if (stage == OTA_CTRL_STAGE_READY && resume_reason != NULL) {
                if (detail_code == OTA_CTRL_RESUME_REASON_OK) {
                    ESP_LOGI(TAG,
                             "STM32 resume accepted: offset=%" PRIu32 "/%" PRIu32,
                             current_value,
                             total_value);
                } else {
                    ESP_LOGW(TAG,
                             "STM32 resume rejected: reason=%s saved=%" PRIu32 "/%" PRIu32,
                             resume_reason,
                             current_value,
                             total_value);
                }
            }
            continue;
        }

        if (frame.msg_type == OTA_CTRL_MSG_GO &&
            frame.payload_len >= OTA_CTRL_GO_PAYLOAD_LEN &&
            frame.payload[0] == request->target_partition) {
            uint16_t go_flags = ota_ctrl_read_u16le(&frame.payload[2]);
            uint32_t resume_transfer_offset = ota_ctrl_read_u32le(&frame.payload[4]);

            if ((go_flags & ~OTA_CTRL_GO_FLAG_RESUME_REQUESTED) != 0U) {
                ESP_LOGE(TAG, "GO carries unknown flags: 0x%04X", go_flags);
                return false;
            }

            if (((go_flags & OTA_CTRL_GO_FLAG_RESUME_REQUESTED) != 0U) &&
                (resume_transfer_offset == 0U)) {
                ESP_LOGE(TAG, "GO requests resume with zero offset");
                return false;
            }

            if (go_request != NULL) {
                go_request->target_partition = frame.payload[0];
                go_request->go_flags = go_flags;
                go_request->resume_transfer_offset = resume_transfer_offset;
            }

            ESP_LOGI(TAG,
                     "RX %s seq=%u stg=%u(%s) target=APP%u flags=0x%04X offset=%" PRIu32,
                     ota_ctrl_msg_name(OTA_CTRL_MSG_GO),
                     frame.seq,
                     OTA_CTRL_STAGE_READY,
                     ota_ctrl_stage_name(OTA_CTRL_STAGE_READY),
                     (unsigned int)request->target_partition + 1U,
                     go_flags,
                     resume_transfer_offset);
            return true;
        }
    }

    ESP_LOGE(TAG, "Waiting for GO timed out");
    return false;
}
