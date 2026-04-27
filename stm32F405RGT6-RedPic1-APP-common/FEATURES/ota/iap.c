#include "iap.h"
#include "delay.h"
#include "sys.h"
#include "flash_if.h"
#include "common.h"
#include "ota_ctrl_protocol.h"
#include <string.h>

#define APP_OTA_REQ_FLAG_BASE       0x00000003UL
#define APP_OTA_DEVICE_PRODUCT_ID   "LCD"
#define APP_OTA_DEVICE_HW_REV       "A1"
#define APP_OTA_DEFAULT_VERSION     "0.0.0"
#define APP_OTA_REQ_RETRY_COUNT     3U
#define APP_OTA_ACK_TIMEOUT_MS      5000U
#define APP_OTA_READY_TIMEOUT_MS    30000U
#define APP_OTA_FRAME_WAIT_MS       500U
#define APP_OTA_POLL_STEP_US        50U
#define APP_OTA_STM32_UID_BASE_ADDR 0x1FFF7A10U

typedef struct
{
    uint8_t msg_type;
    uint8_t seq;
    uint16_t payload_len;
    uint8_t payload[OTA_CTRL_MAX_PAYLOAD_LEN];
} app_ota_ctrl_frame_t;

static uint8_t s_app_ota_ctrl_seq = 1U;

static uint8_t app_ota_version_is_valid(const char *version)
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

static uint16_t app_ota_crc16(const uint8_t *data, uint16_t length)
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

static void app_ota_write_u16le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void app_ota_write_u32le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t app_ota_read_u16le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static void app_ota_fill_string(uint8_t *target, uint16_t target_len, const char *value)
{
    uint16_t i = 0U;

    memset(target, 0, target_len);
    if (value == 0)
    {
        return;
    }

    for (i = 0U; i < target_len && value[i] != '\0'; ++i)
    {
        target[i] = (uint8_t)value[i];
    }
}

static void app_ota_copy_ascii(char *target,
                               uint16_t target_len,
                               const uint8_t *source,
                               uint16_t source_len)
{
    uint16_t i = 0U;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    memset(target, 0, target_len);
    if (source == 0)
    {
        return;
    }

    for (i = 0U; i + 1U < target_len && i < source_len && source[i] != '\0'; ++i)
    {
        target[i] = (char)source[i];
    }
}

static uint8_t app_ota_next_seq(void)
{
    if (s_app_ota_ctrl_seq == 0U)
    {
        s_app_ota_ctrl_seq = 1U;
    }

    return s_app_ota_ctrl_seq++;
}

static void app_ota_flush_uart(void)
{
    uint8_t ch = 0U;

    while (SerialKeyPressed(&ch) != 0U)
    {
    }
}

static uint8_t app_ota_read_byte_timeout(uint8_t *byte, uint32_t timeout_ms)
{
    uint32_t waited_us = 0U;
    uint32_t timeout_us = timeout_ms * 1000U;

    while (waited_us < timeout_us)
    {
        if (SerialKeyPressed(byte) != 0U)
        {
            return 1U;
        }

        delay_us(APP_OTA_POLL_STEP_US);
        waited_us += APP_OTA_POLL_STEP_US;
    }

    return 0U;
}

static uint8_t app_ota_send_frame(uint8_t msg_type,
                                  uint8_t seq,
                                  const uint8_t *payload,
                                  uint16_t payload_len)
{
    uint8_t frame[OTA_CTRL_MAX_FRAME_LEN];
    uint16_t total_len = 0U;
    uint16_t crc = 0U;
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
    app_ota_write_u16le(&frame[5], payload_len);

    for (i = 0U; i < payload_len; ++i)
    {
        frame[OTA_CTRL_HEADER_LEN + i] = payload[i];
    }

    crc = app_ota_crc16(&frame[2], (uint16_t)(5U + payload_len));
    app_ota_write_u16le(&frame[OTA_CTRL_HEADER_LEN + payload_len], crc);

    total_len = (uint16_t)(OTA_CTRL_FRAME_OVERHEAD + payload_len);
    for (i = 0U; i < total_len; ++i)
    {
        SerialPutChar(frame[i]);
    }

    return 1U;
}

static uint8_t app_ota_receive_frame(app_ota_ctrl_frame_t *frame, uint32_t timeout_ms)
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
        if (app_ota_read_byte_timeout(&ch, 1U) == 0U)
        {
            ++waited;
            continue;
        }

        if (ch != OTA_CTRL_SOF1)
        {
            continue;
        }

        if (app_ota_read_byte_timeout(&ch, 20U) == 0U)
        {
            return 0U;
        }

        if (ch != OTA_CTRL_SOF2)
        {
            continue;
        }

        for (i = 0U; i < sizeof(header); ++i)
        {
            if (app_ota_read_byte_timeout(&header[i], 20U) == 0U)
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
        frame->payload_len = app_ota_read_u16le(&header[3]);
        if (frame->payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
        {
            app_ota_flush_uart();
            return 0U;
        }

        for (i = 0U; i < frame->payload_len; ++i)
        {
            if (app_ota_read_byte_timeout(&frame->payload[i], 20U) == 0U)
            {
                return 0U;
            }
        }

        if (app_ota_read_byte_timeout(&crc_bytes[0], 20U) == 0U ||
            app_ota_read_byte_timeout(&crc_bytes[1], 20U) == 0U)
        {
            return 0U;
        }

        crc_recv = app_ota_read_u16le(crc_bytes);
        for (i = 0U; i < sizeof(header); ++i)
        {
            crc_buffer[i] = header[i];
        }
        for (i = 0U; i < frame->payload_len; ++i)
        {
            crc_buffer[sizeof(header) + i] = frame->payload[i];
        }

        crc_calc = app_ota_crc16(crc_buffer, (uint16_t)(sizeof(header) + frame->payload_len));
        if (crc_recv != crc_calc)
        {
            continue;
        }

        return 1U;
    }

    return 0U;
}

static uint8_t app_ota_prepare_request_payload(const BootInfoTypeDef *boot_info,
                                               uint8_t *payload,
                                               uint16_t *payload_len,
                                               uint32_t req_flags)
{
    const char *version = APP_OTA_DEFAULT_VERSION;
    const uint8_t *uid = (const uint8_t *)APP_OTA_STM32_UID_BASE_ADDR;
    uint16_t i = 0U;

    if (boot_info == 0 || payload == 0 || payload_len == 0)
    {
        return 0U;
    }

    memset(payload, 0, OTA_CTRL_REQ_PAYLOAD_LEN);
    payload[0] = OTA_CTRL_REQ_TYPE_UPGRADE;
    payload[1] = (uint8_t)boot_info->active_partition;
    payload[2] = (uint8_t)boot_info->target_partition;
    payload[3] = app_ota_version_is_valid(boot_info->current_version) ? 1U : 0U;

    if (payload[3] != 0U)
    {
        version = boot_info->current_version;
    }

    app_ota_fill_string(&payload[4], OTA_CTRL_VERSION_LEN, version);
    app_ota_fill_string(&payload[20], OTA_CTRL_PRODUCT_ID_LEN, APP_OTA_DEVICE_PRODUCT_ID);
    app_ota_fill_string(&payload[36], OTA_CTRL_HW_REV_LEN, APP_OTA_DEVICE_HW_REV);

    for (i = 0U; i < OTA_CTRL_UID_LEN; ++i)
    {
        payload[44 + i] = uid[i];
    }

    app_ota_write_u32le(&payload[56], req_flags);
    *payload_len = OTA_CTRL_REQ_PAYLOAD_LEN;
    return 1U;
}

static uint8_t app_ota_extract_ready_version(const app_ota_ctrl_frame_t *frame,
                                             char *version,
                                             uint16_t version_len)
{
    if (frame == 0 || version == 0 || version_len == 0U ||
        frame->payload_len < OTA_CTRL_READY_PAYLOAD_LEN)
    {
        return 0U;
    }

    app_ota_copy_ascii(version, version_len, &frame->payload[4], OTA_CTRL_VERSION_LEN);
    return app_ota_version_is_valid(version);
}

uint8_t iap_query_latest_version(const BootInfoTypeDef *boot_info,
                                 char *latest_version,
                                 uint16_t latest_version_len,
                                 uint16_t *reject_reason)
{
    app_ota_ctrl_frame_t frame;
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
    if (latest_version != 0 && latest_version_len > 0U)
    {
        latest_version[0] = '\0';
    }

    if (!app_ota_prepare_request_payload(boot_info,
                                         payload,
                                         &payload_len,
                                         APP_OTA_REQ_FLAG_BASE | OTA_CTRL_REQ_FLAG_CHECK_ONLY))
    {
        return 0U;
    }

    app_ota_flush_uart();
    req_seq = app_ota_next_seq();

    while (retry < APP_OTA_REQ_RETRY_COUNT)
    {
        if (!app_ota_send_frame(OTA_CTRL_MSG_REQ, req_seq, payload, payload_len))
        {
            return 0U;
        }

        if (app_ota_receive_frame(&frame, APP_OTA_ACK_TIMEOUT_MS))
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
                    *reject_reason = app_ota_read_u16le(&frame.payload[4]);
                }
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
                    return 0U;
                }

                if (app_ota_extract_ready_version(&frame, latest_version, latest_version_len) == 0U)
                {
                    if (reject_reason != 0)
                    {
                        *reject_reason = OTA_CTRL_ERR_VERSION;
                    }
                    return 0U;
                }

                return 1U;
            }

            if (frame.msg_type == OTA_CTRL_MSG_ERROR &&
                frame.payload_len >= OTA_CTRL_ERROR_PAYLOAD_LEN)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = app_ota_read_u16le(&frame.payload[2]);
                }
                return 0U;
            }

            if (frame.msg_type == OTA_CTRL_MSG_STATUS &&
                frame.payload_len >= OTA_CTRL_STATUS_PAYLOAD_LEN)
            {
                ack_received = 1U;
                break;
            }
        }

        ++retry;
    }

    if (!ack_received)
    {
        return 0U;
    }

    while (waited_ms < APP_OTA_READY_TIMEOUT_MS)
    {
        if (!app_ota_receive_frame(&frame, APP_OTA_FRAME_WAIT_MS))
        {
            waited_ms += APP_OTA_FRAME_WAIT_MS;
            continue;
        }

        if (frame.msg_type == OTA_CTRL_MSG_STATUS &&
            frame.payload_len >= OTA_CTRL_STATUS_PAYLOAD_LEN)
        {
            continue;
        }

        if (frame.msg_type == OTA_CTRL_MSG_ERROR &&
            frame.payload_len >= OTA_CTRL_ERROR_PAYLOAD_LEN)
        {
            if (reject_reason != 0)
            {
                *reject_reason = app_ota_read_u16le(&frame.payload[2]);
            }
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
                return 0U;
            }

            if (app_ota_extract_ready_version(&frame, latest_version, latest_version_len) == 0U)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = OTA_CTRL_ERR_VERSION;
                }
                return 0U;
            }

            return 1U;
        }
    }

    return 0U;
}


