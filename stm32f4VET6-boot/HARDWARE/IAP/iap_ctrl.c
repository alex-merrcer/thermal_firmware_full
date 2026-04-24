#include "iap_ctrl.h"

static uint8_t s_ota_ctrl_seq = 1U;

static uint16_t ota_ctrl_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0U;
    uint16_t i = 0U;

    while (length-- > 0U)
    {
        crc ^= (uint16_t)(*data++) << 8;
        for (i = 0U; i < 8U; ++i)
        {
            if ((crc & 0x8000U) != 0U)
            {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static void ota_ctrl_write_u16le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void ota_ctrl_write_u32le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t ota_ctrl_read_u16le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static uint32_t ota_ctrl_read_u32le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

static void ota_ctrl_fill_string(uint8_t *target, uint16_t target_len, const char *value)
{
    uint16_t i = 0U;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    for (i = 0U; i < target_len; ++i)
    {
        target[i] = 0U;
    }

    if (value == 0)
    {
        return;
    }

    for (i = 0U; i < target_len && value[i] != '\0'; ++i)
    {
        target[i] = (uint8_t)value[i];
    }
}

static void ota_ctrl_copy_ascii(char *target,
                                uint16_t target_len,
                                const uint8_t *source,
                                uint16_t source_len)
{
    uint16_t i = 0U;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    for (i = 0U; i < target_len; ++i)
    {
        target[i] = '\0';
    }

    if (source == 0)
    {
        return;
    }

    for (i = 0U; i + 1U < target_len && i < source_len && source[i] != '\0'; ++i)
    {
        target[i] = (char)source[i];
    }
}

void ota_ctrl_flush_uart(void)
{
    SerialResetRxState();
}

static uint8_t ota_ctrl_read_byte_timeout(uint8_t *byte, uint32_t timeout_ms)
{
    uint32_t waited_us = 0U;
    uint32_t timeout_us = timeout_ms * 1000U;

    while (waited_us < timeout_us)
    {
        if (SerialKeyPressed(byte) != 0U)
        {
            return 1U;
        }

        delay_us(OTA_CTRL_POLL_STEP_US);
        waited_us += OTA_CTRL_POLL_STEP_US;
    }

    return 0U;
}

static uint8_t ota_ctrl_send_frame(uint8_t msg_type,
                                   uint8_t seq,
                                   const uint8_t *payload,
                                   uint16_t payload_len)
{
    uint8_t frame[OTA_CTRL_MAX_FRAME_LEN];
    uint16_t crc = 0U;
    uint16_t total_len = 0U;
    uint16_t i = 0U;

    if (payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
    {
        return 0U;
    }

    frame[0] = OTA_CTRL_SOF1;
    frame[1] = OTA_CTRL_SOF2;
    frame[2] = OTA_CTRL_PROTOCOL_VERSION;
    frame[3] = msg_type;
    frame[4] = seq;
    ota_ctrl_write_u16le(&frame[5], payload_len);

    for (i = 0U; i < payload_len; ++i)
    {
        frame[OTA_CTRL_HEADER_LEN + i] = payload[i];
    }

    crc = ota_ctrl_crc16(&frame[2], (uint16_t)(5U + payload_len));
    ota_ctrl_write_u16le(&frame[OTA_CTRL_HEADER_LEN + payload_len], crc);

    total_len = (uint16_t)(OTA_CTRL_FRAME_OVERHEAD + payload_len);
    for (i = 0U; i < total_len; ++i)
    {
        SerialPutChar(frame[i]);
    }

    return 1U;
}

static uint8_t ota_ctrl_receive_frame(ota_ctrl_frame_t *frame, uint32_t timeout_ms)
{
    uint8_t ch = 0U;
    uint8_t header[5];
    uint8_t crc_bytes[2];
    uint8_t crc_buffer[5U + OTA_CTRL_MAX_PAYLOAD_LEN];
    uint16_t crc_calc = 0U;
    uint16_t crc_recv = 0U;
    uint16_t i = 0U;
    uint32_t waited = 0U;

    while (waited < timeout_ms)
    {
        if (ota_ctrl_read_byte_timeout(&ch, 1U) == 0U)
        {
            ++waited;
            continue;
        }

        if (ch != OTA_CTRL_SOF1)
        {
            continue;
        }

        if (ota_ctrl_read_byte_timeout(&ch, 20U) == 0U)
        {
            return 0U;
        }

        if (ch != OTA_CTRL_SOF2)
        {
            continue;
        }

        for (i = 0U; i < sizeof(header); ++i)
        {
            if (ota_ctrl_read_byte_timeout(&header[i], 20U) == 0U)
            {
                return 0U;
            }
        }

        if (header[0] != OTA_CTRL_PROTOCOL_VERSION)
        {
            continue;
        }

        frame->msg_type = header[1];
        frame->seq = header[2];
        frame->payload_len = ota_ctrl_read_u16le(&header[3]);

        if (frame->payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
        {
            ota_ctrl_flush_uart();
            return 0U;
        }

        for (i = 0U; i < frame->payload_len; ++i)
        {
            if (ota_ctrl_read_byte_timeout(&frame->payload[i], 20U) == 0U)
            {
                return 0U;
            }
        }

        if (ota_ctrl_read_byte_timeout(&crc_bytes[0], 20U) == 0U ||
            ota_ctrl_read_byte_timeout(&crc_bytes[1], 20U) == 0U)
        {
            return 0U;
        }

        crc_recv = ota_ctrl_read_u16le(crc_bytes);
        for (i = 0U; i < sizeof(header); ++i)
        {
            crc_buffer[i] = header[i];
        }
        for (i = 0U; i < frame->payload_len; ++i)
        {
            crc_buffer[sizeof(header) + i] = frame->payload[i];
        }

        crc_calc = ota_ctrl_crc16(crc_buffer, (uint16_t)(sizeof(header) + frame->payload_len));
        if (crc_recv != crc_calc)
        {
            continue;
        }

        return 1U;
    }

    return 0U;
}

static uint8_t ota_ctrl_next_seq(void)
{
    if (s_ota_ctrl_seq == 0U)
    {
        s_ota_ctrl_seq = 1U;
    }

    return s_ota_ctrl_seq++;
}

static uint8_t ota_ctrl_prepare_request_payload(const BootInfoTypeDef *boot_info,
                                                uint8_t *payload,
                                                uint16_t *payload_len,
                                                uint32_t req_flags)
{
    const uint8_t *uid = (const uint8_t *)STM32_UID_BASE_ADDR;
    uint16_t i = 0U;

    if (boot_info == 0 || payload == 0 || payload_len == 0)
    {
        return 0U;
    }

    for (i = 0U; i < OTA_CTRL_REQ_PAYLOAD_LEN; ++i)
    {
        payload[i] = 0U;
    }

    payload[0] = OTA_CTRL_REQ_TYPE_UPGRADE;
    payload[1] = (uint8_t)boot_info->active_partition;
    payload[2] = (uint8_t)boot_info->target_partition;
    payload[3] = 1U;
    ota_ctrl_fill_string(&payload[4], OTA_CTRL_VERSION_LEN, boot_info->current_version);
    ota_ctrl_fill_string(&payload[20], OTA_CTRL_PRODUCT_ID_LEN, IAP_DEVICE_PRODUCT_ID);
    ota_ctrl_fill_string(&payload[36], OTA_CTRL_HW_REV_LEN, IAP_DEVICE_HW_REV);

    for (i = 0U; i < OTA_CTRL_UID_LEN; ++i)
    {
        payload[44 + i] = uid[i];
    }

    ota_ctrl_write_u32le(&payload[56], req_flags);
    *payload_len = OTA_CTRL_REQ_PAYLOAD_LEN;
    return 1U;
}

uint8_t ota_ctrl_send_go(uint8_t target_partition, uint16_t go_flags, uint32_t resume_offset)
{
    uint8_t payload[OTA_CTRL_GO_PAYLOAD_LEN];
    uint16_t i = 0U;

    for (i = 0U; i < OTA_CTRL_GO_PAYLOAD_LEN; ++i)
    {
        payload[i] = 0U;
    }

    payload[0] = target_partition;
    ota_ctrl_write_u16le(&payload[2], go_flags);
    ota_ctrl_write_u32le(&payload[4], resume_offset);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_GO, ota_ctrl_next_seq(), payload, OTA_CTRL_GO_PAYLOAD_LEN);
}

uint8_t ota_ctrl_send_status(uint8_t stage,
                             uint8_t percent,
                             uint16_t detail_code,
                             uint32_t current_value,
                             uint32_t total_value)
{
    uint8_t payload[OTA_CTRL_STATUS_PAYLOAD_LEN];

    memset(payload, 0, sizeof(payload));
    payload[0] = stage;
    payload[1] = percent;
    ota_ctrl_write_u16le(&payload[2], detail_code);
    ota_ctrl_write_u32le(&payload[4], current_value);
    ota_ctrl_write_u32le(&payload[8], total_value);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_STATUS,
                               ota_ctrl_next_seq(),
                               payload,
                               OTA_CTRL_STATUS_PAYLOAD_LEN);
}

static uint8_t ota_ctrl_extract_ready_info(const ota_ctrl_frame_t *frame,
                                           ota_ctrl_ready_info_t *ready_info)
{
    if (frame == 0 || ready_info == 0 || frame->payload_len < OTA_CTRL_READY_PAYLOAD_LEN)
    {
        return 0U;
    }

    memset(ready_info, 0, sizeof(*ready_info));
    ready_info->target_partition = frame->payload[0];
    ready_info->ready_flags = ota_ctrl_read_u16le(&frame->payload[2]);
    ota_ctrl_copy_ascii(ready_info->version,
                        (uint16_t)sizeof(ready_info->version),
                        &frame->payload[4],
                        OTA_CTRL_VERSION_LEN);
    ready_info->plain_size = ota_ctrl_read_u32le(&frame->payload[20]);
    ready_info->transfer_size = ota_ctrl_read_u32le(&frame->payload[24]);
    ready_info->checkpoint_size = ota_ctrl_read_u32le(&frame->payload[28]);
    memcpy(ready_info->session_fingerprint, &frame->payload[32], OTA_CTRL_FINGERPRINT_LEN);

    if (version_text_is_valid(ready_info->version) == 0U)
    {
        return 0U;
    }

    return 1U;
}

uint8_t ota_ctrl_wait_for_meta_image_header(OtaImageHeaderBinary *header, uint32_t timeout_ms)
{
    ota_ctrl_frame_t frame;
    uint32_t waited_ms = 0U;
    uint16_t expected_offset = 0U;
    uint16_t total_length = 0U;

    if (header == 0)
    {
        return 0U;
    }

    memset(header, 0, sizeof(*header));

    while (waited_ms < timeout_ms)
    {
        uint16_t chunk_offset = 0U;
        uint16_t chunk_len = 0U;
        uint16_t frame_total = 0U;

        if (ota_ctrl_receive_frame(&frame, OTA_CTRL_FRAME_WAIT_MS) == 0U)
        {
            waited_ms += OTA_CTRL_FRAME_WAIT_MS;
            continue;
        }

        if (frame.msg_type != OTA_CTRL_MSG_META ||
            frame.payload_len < OTA_CTRL_META_PAYLOAD_HDR_LEN ||
            frame.payload[0] != OTA_CTRL_META_KIND_IMAGE_HEADER)
        {
            continue;
        }

        chunk_offset = ota_ctrl_read_u16le(&frame.payload[2]);
        chunk_len = ota_ctrl_read_u16le(&frame.payload[4]);
        frame_total = ota_ctrl_read_u16le(&frame.payload[6]);

        if (frame_total != OTA_IMAGE_HEADER_TOTAL_SIZE ||
            chunk_offset != expected_offset ||
            (uint16_t)(chunk_offset + chunk_len) > frame_total ||
            (uint16_t)(OTA_CTRL_META_PAYLOAD_HDR_LEN + chunk_len) != frame.payload_len)
        {
            return 0U;
        }

        memcpy(((uint8_t *)header) + chunk_offset,
               &frame.payload[OTA_CTRL_META_PAYLOAD_HDR_LEN],
               chunk_len);
        expected_offset = (uint16_t)(expected_offset + chunk_len);
        total_length = frame_total;

        if (expected_offset == total_length)
        {
            return 1U;
        }
    }

    return 0U;
}

uint8_t ota_ctrl_send_result(uint8_t outcome, uint8_t stage, uint16_t error_code, uint32_t final_offset)
{
    uint8_t payload[OTA_CTRL_RESULT_PAYLOAD_LEN];

    memset(payload, 0, sizeof(payload));
    payload[0] = outcome;
    payload[1] = stage;
    ota_ctrl_write_u16le(&payload[2], error_code);
    ota_ctrl_write_u32le(&payload[4], final_offset);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_RESULT, ota_ctrl_next_seq(), payload, OTA_CTRL_RESULT_PAYLOAD_LEN);
}

static uint8_t ota_ctrl_is_ready_status_frame(const ota_ctrl_frame_t *frame)
{
    if (frame == 0)
    {
        return 0U;
    }

    if (frame->msg_type != OTA_CTRL_MSG_STATUS ||
        frame->payload_len < OTA_CTRL_STATUS_PAYLOAD_LEN)
    {
        return 0U;
    }

    return (frame->payload[0] == OTA_CTRL_STAGE_READY) ? 1U : 0U;
}

uint8_t ota_ctrl_wait_for_upgrade_ready(const BootInfoTypeDef *boot_info,
                                        ota_ctrl_ready_info_t *ready_info,
                                        uint16_t *reject_reason,
                                        uint32_t req_flags)
{
    ota_ctrl_frame_t frame;
    uint8_t payload[OTA_CTRL_REQ_PAYLOAD_LEN];
    uint16_t payload_len = 0U;
    uint8_t ack_received = 0U;
    uint8_t req_seq = 0U;
    uint8_t retry = 0U;
    uint32_t waited_ms = 0U;

    if (reject_reason != 0)
    {
        *reject_reason = 0U;
    }

    if (ready_info != 0)
    {
        memset(ready_info, 0, sizeof(*ready_info));
    }

    if (ota_ctrl_prepare_request_payload(boot_info, payload, &payload_len, req_flags) == 0U)
    {
        return 0U;
    }

    ota_ctrl_flush_uart();
    req_seq = ota_ctrl_next_seq();
    ota_ctrl_show_status_text("Send request", "To ESP32");

    while (retry < OTA_CTRL_REQ_RETRY_COUNT)
    {
        if (ota_ctrl_send_frame(OTA_CTRL_MSG_REQ, req_seq, payload, payload_len) == 0U)
        {
            return 0U;
        }

        if (ota_ctrl_receive_frame(&frame, OTA_CTRL_ACK_TIMEOUT_MS))
        {
            if (frame.msg_type == OTA_CTRL_MSG_ACK &&
                frame.payload_len >= OTA_CTRL_ACK_PAYLOAD_LEN)
            {
                if (frame.payload[0] == 1U)
                {
                    ack_received = 1U;
                    break;
                }

                if (reject_reason != 0)
                {
                    *reject_reason = ota_ctrl_read_u16le(&frame.payload[4]);
                }
                ota_ctrl_show_ack_reject_reason(ota_ctrl_read_u16le(&frame.payload[4]));
                return 0U;
            }

            if (frame.msg_type == OTA_CTRL_MSG_READY &&
                frame.payload_len >= OTA_CTRL_READY_PAYLOAD_LEN)
            {
                if (frame.payload[0] != (uint8_t)boot_info->target_partition)
                {
                    if (reject_reason != 0)
                    {
                        *reject_reason = OTA_CTRL_ERR_PARTITION;
                    }
                    ota_ctrl_show_error_code(OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PARTITION);
                    return 0U;
                }

                if (ota_ctrl_extract_ready_info(&frame, ready_info) == 0U)
                {
                    if (reject_reason != 0)
                    {
                        *reject_reason = OTA_CTRL_ERR_VERSION;
                    }
                    ota_ctrl_show_error_code(OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_VERSION);
                    return 0U;
                }

                ota_ctrl_show_ready_info(&frame);
                return 1U;
            }

            if (frame.msg_type == OTA_CTRL_MSG_ERROR &&
                frame.payload_len >= OTA_CTRL_ERROR_PAYLOAD_LEN)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = ota_ctrl_read_u16le(&frame.payload[2]);
                }
                ota_ctrl_show_error_code(frame.payload[0], ota_ctrl_read_u16le(&frame.payload[2]));
                return 0U;
            }

            if (frame.msg_type == OTA_CTRL_MSG_STATUS &&
                frame.payload_len >= OTA_CTRL_STATUS_PAYLOAD_LEN)
            {
                ack_received = 1U;
                ota_ctrl_show_stage(frame.payload[0],
                                    frame.payload[1],
                                    ota_ctrl_read_u16le(&frame.payload[2]),
                                    ota_ctrl_read_u32le(&frame.payload[4]),
                                    ota_ctrl_read_u32le(&frame.payload[8]));
                break;
            }
        }

        ++retry;
    }

    if (ack_received == 0U)
    {
        ota_ctrl_show_status_text("ESP32 timeout", "No ACK");
        return 0U;
    }

    ota_ctrl_show_status_text("ESP32 ACK", "Preparing");
    waited_ms = 0U;

    while (waited_ms < OTA_CTRL_READY_TIMEOUT_MS)
    {
        if (ota_ctrl_receive_frame(&frame, OTA_CTRL_FRAME_WAIT_MS) == 0U)
        {
            waited_ms += OTA_CTRL_FRAME_WAIT_MS;
            continue;
        }

        if (frame.msg_type == OTA_CTRL_MSG_STATUS &&
            frame.payload_len >= OTA_CTRL_STATUS_PAYLOAD_LEN)
        {
            if (ota_ctrl_is_ready_status_frame(&frame) == 0U)
            {
                ota_ctrl_show_stage(frame.payload[0],
                                    frame.payload[1],
                                    ota_ctrl_read_u16le(&frame.payload[2]),
                                    ota_ctrl_read_u32le(&frame.payload[4]),
                                    ota_ctrl_read_u32le(&frame.payload[8]));
            }
            continue;
        }

        if (frame.msg_type == OTA_CTRL_MSG_ERROR &&
            frame.payload_len >= OTA_CTRL_ERROR_PAYLOAD_LEN)
        {
            if (reject_reason != 0)
            {
                *reject_reason = ota_ctrl_read_u16le(&frame.payload[2]);
            }
            ota_ctrl_show_error_code(frame.payload[0], ota_ctrl_read_u16le(&frame.payload[2]));
            return 0U;
        }

        if (frame.msg_type == OTA_CTRL_MSG_READY &&
            frame.payload_len >= OTA_CTRL_READY_PAYLOAD_LEN)
        {
            if (frame.payload[0] != (uint8_t)boot_info->target_partition)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = OTA_CTRL_ERR_PARTITION;
                }
                ota_ctrl_show_error_code(OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PARTITION);
                return 0U;
            }

            if (ota_ctrl_extract_ready_info(&frame, ready_info) == 0U)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = OTA_CTRL_ERR_VERSION;
                }
                ota_ctrl_show_error_code(OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_VERSION);
                return 0U;
            }

            ota_ctrl_show_ready_info(&frame);
            return 1U;
        }
    }

    ota_ctrl_show_status_text("ESP32 timeout", "No READY");
    return 0U;
}
