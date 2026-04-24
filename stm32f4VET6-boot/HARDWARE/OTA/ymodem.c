/**
  ******************************************************************************
  * @file    ymodem.c
  * @brief   STM32 OTA custom data-plane receiver with resume support.
  ******************************************************************************
  */

#include "flash_if.h"
#include "common.h"
#include "ymodem.h"
#include "iap.h"
#include "string.h"
#include "aes.h"
#include "delay.h"
#include "stm32f4xx_iwdg.h"

uint32_t Send_Byte(uint8_t c);

#define OTA_DATA_PACKET_OK         0
#define OTA_DATA_PACKET_TIMEOUT   -1
#define OTA_DATA_PACKET_CRC       -2
#define OTA_DATA_PACKET_UART      -3

#define OTA_DATA_RX_TIMEOUT        (0x100000UL)
#define OTA_DATA_POLL_STEP_US      50U

typedef struct
{
    uint8_t type;
    uint32_t session_id;
    uint32_t offset;
    uint16_t payload_len;
    uint8_t payload[OTA_DATA_MAX_PAYLOAD_LEN];
} OtaDataFrame;

typedef struct
{
    uint8_t *flash_buffer;
    uint32_t flash_buffer_len;
    uint32_t flash_destination;
    uint32_t firmware_received;
    uint32_t durable_offset;
    uint32_t start_offset;
    uint32_t checkpoint_size;
    uint32_t transfer_size;
    uint32_t last_acked_offset;
    uint16_t chunk_size;
    uint8_t flash_ready;
} YmodemTransferState;

static uint8_t ymodem_error_code = YMODEM_OK;
static uint8_t ymodem_error_stage = 0U;
static uint32_t ymodem_uart_error_flags = 0U;
static uint32_t ymodem_last_acked_offset = 0U;
static YmodemProgressCallback ymodem_progress_callback = 0;
static YmodemHeaderValidator ymodem_header_validator = 0;
static void *ymodem_header_validator_context = 0;
static YmodemCheckpointCallback ymodem_checkpoint_callback = 0;
static void *ymodem_checkpoint_context = 0;
static OtaImageHeaderBinary ymodem_received_header;
static uint8_t ymodem_received_header_valid = 0U;
static uint32_t ymodem_received_firmware_size = 0U;
static YmodemHashDiagnostics ymodem_hash_diagnostics;
static uint8_t ymodem_body_hash_active = 0U;
static OtaSha256Context ymodem_body_hash_context;
static uint8_t ymodem_session_fingerprint[OTA_CTRL_FINGERPRINT_LEN];
static uint8_t ymodem_session_configured = 0U;
static uint32_t ymodem_session_id = 0U;
static uint32_t ymodem_session_start_offset = 0U;
static uint32_t ymodem_session_checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
static const uint32_t s_ymodem_resume_first_persist_bytes = OTA_DATA_DEFAULT_CHUNK_SIZE;
static const uint32_t s_ymodem_resume_persist_stride_bytes = OTA_DATA_DEFAULT_CHECKPOINT_SIZE * 4U;

static const uint32_t s_ota_sha256_k[64] =
{
    0x428A2F98UL, 0x71374491UL, 0xB5C0FBCFUL, 0xE9B5DBA5UL,
    0x3956C25BUL, 0x59F111F1UL, 0x923F82A4UL, 0xAB1C5ED5UL,
    0xD807AA98UL, 0x12835B01UL, 0x243185BEUL, 0x550C7DC3UL,
    0x72BE5D74UL, 0x80DEB1FEUL, 0x9BDC06A7UL, 0xC19BF174UL,
    0xE49B69C1UL, 0xEFBE4786UL, 0x0FC19DC6UL, 0x240CA1CCUL,
    0x2DE92C6FUL, 0x4A7484AAUL, 0x5CB0A9DCUL, 0x76F988DAUL,
    0x983E5152UL, 0xA831C66DUL, 0xB00327C8UL, 0xBF597FC7UL,
    0xC6E00BF3UL, 0xD5A79147UL, 0x06CA6351UL, 0x14292967UL,
    0x27B70A85UL, 0x2E1B2138UL, 0x4D2C6DFCUL, 0x53380D13UL,
    0x650A7354UL, 0x766A0ABBUL, 0x81C2C92EUL, 0x92722C85UL,
    0xA2BFE8A1UL, 0xA81A664BUL, 0xC24B8B70UL, 0xC76C51A3UL,
    0xD192E819UL, 0xD6990624UL, 0xF40E3585UL, 0x106AA070UL,
    0x19A4C116UL, 0x1E376C08UL, 0x2748774CUL, 0x34B0BCB5UL,
    0x391C0CB3UL, 0x4ED8AA4AUL, 0x5B9CCA4FUL, 0x682E6FF3UL,
    0x748F82EEUL, 0x78A5636FUL, 0x84C87814UL, 0x8CC70208UL,
    0x90BEFFFAUL, 0xA4506CEBUL, 0xBEF9A3F7UL, 0xC67178F2UL
};

static const uint32_t s_ota_flash_sector_boundaries[] =
{
    ADDR_FLASH_SECTOR_0,
    ADDR_FLASH_SECTOR_1,
    ADDR_FLASH_SECTOR_2,
    ADDR_FLASH_SECTOR_3,
    ADDR_FLASH_SECTOR_4,
    ADDR_FLASH_SECTOR_5,
    ADDR_FLASH_SECTOR_6,
    ADDR_FLASH_SECTOR_7,
    ADDR_FLASH_SECTOR_8,
    ADDR_FLASH_SECTOR_9,
    ADDR_FLASH_SECTOR_10,
    ADDR_FLASH_SECTOR_11,
    0x08100000U
};

static void Ymodem_ResetHashDiagnostics(void);
static void Ymodem_BodyHashBegin(void);
static uint8_t Ymodem_BodyHashResumeFromFlash(uint32_t address, uint32_t length);
static void Ymodem_BodyHashUpdate(const uint8_t *data, uint32_t data_len);
static void Ymodem_BodyHashFinish(void);
static void Ymodem_ResetSessionState(void);
static uint8_t Ymodem_WriteFlashBuffered(YmodemTransferState *state,
                                         const uint8_t *data,
                                         uint32_t data_len);
static uint8_t Ymodem_FlushFlashBuffered(YmodemTransferState *state);
static int32_t Receive_Byte(uint8_t *c, uint32_t timeout);
static void Ymodem_FeedWatchdog(void);
static void Ymodem_NotifyProgress(uint32_t current, uint32_t total);
static uint16_t Ymodem_Crc16(const uint8_t *data, uint32_t count);
static uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length);
static uint8_t ota_data_send_frame(uint8_t type,
                                   uint32_t session_id,
                                   uint32_t offset,
                                   const uint8_t *payload,
                                   uint16_t payload_len);
static int32_t ota_data_receive_frame(OtaDataFrame *frame, uint32_t timeout);
static void ota_data_send_ack(uint32_t session_id, uint32_t next_offset);
static void ota_data_send_nak(uint32_t session_id,
                              uint32_t retry_offset,
                              uint16_t reason_code,
                              uint16_t detail_code);
static void ota_data_send_abort(uint32_t session_id,
                                uint32_t final_offset,
                                uint8_t stage,
                                uint8_t error_class,
                                uint16_t error_code);
static uint32_t ota_data_session_id_from_fingerprint(const uint8_t fingerprint[OTA_CTRL_FINGERPRINT_LEN]);
static void ota_ctr_build_counter(uint8_t counter[BLOCKSIZE], const uint8_t iv[BLOCKSIZE], uint32_t block_index);
static void ota_ctr_crypt(uint8_t *buffer,
                          uint32_t length,
                          const uint8_t iv[BLOCKSIZE],
                          uint32_t offset);
static uint8_t ota_data_process_start_frame(const OtaDataFrame *frame, YmodemTransferState *state);
static uint8_t ota_data_process_chunk_frame(const OtaDataFrame *frame, YmodemTransferState *state);
static uint8_t ota_data_finalize_transfer(YmodemTransferState *state);
static uint8_t ota_data_maybe_persist_progress(YmodemTransferState *state);
static uint32_t ota_data_slot_end_exclusive(uint32_t address);
static uint32_t ota_data_sector_base(uint32_t address);
static uint32_t ota_data_next_sector_base(uint32_t address);
static uint8_t ota_data_error_is_terminal(uint8_t err_code);
static uint32_t ota_sha256_load_be32(const uint8_t *data);
static void ota_sha256_store_be32(uint32_t value, uint8_t *data);
static void ota_sha256_transform(OtaSha256Context *context, const uint8_t block[64]);

uint8_t Ymodem_GetErrorCode(void)
{
    return ymodem_error_code;
}

uint8_t Ymodem_GetErrorStage(void)
{
    return ymodem_error_stage;
}

uint32_t Ymodem_GetUartErrorFlags(void)
{
    return ymodem_uart_error_flags;
}

uint32_t Ymodem_GetLastAckedOffset(void)
{
    return ymodem_last_acked_offset;
}

void Ymodem_SetError(uint8_t code, uint8_t stage)
{
    ymodem_error_code = code;
    ymodem_error_stage = stage;
}

void Ymodem_ResetError(void)
{
    ymodem_error_code = YMODEM_OK;
    ymodem_error_stage = 0U;
    ymodem_uart_error_flags = 0U;
    ymodem_last_acked_offset = 0U;
}

void Ymodem_SetProgressCallback(YmodemProgressCallback callback)
{
    ymodem_progress_callback = callback;
}

void Ymodem_SetHeaderValidator(YmodemHeaderValidator validator, void *context)
{
    ymodem_header_validator = validator;
    ymodem_header_validator_context = context;
}

const OtaImageHeaderBinary *Ymodem_GetReceivedHeader(void)
{
    return (ymodem_received_header_valid != 0U) ? &ymodem_received_header : 0;
}

uint32_t Ymodem_GetReceivedFirmwareSize(void)
{
    return ymodem_received_firmware_size;
}

const YmodemHashDiagnostics *Ymodem_GetHashDiagnostics(void)
{
    return &ymodem_hash_diagnostics;
}

static void Ymodem_NotifyProgress(uint32_t current, uint32_t total)
{
    if (ymodem_progress_callback != 0)
    {
        ymodem_progress_callback(current, total);
    }
}

static uint32_t ota_sha256_rotr32(uint32_t value, uint32_t shift)
{
    return (value >> shift) | (value << (32U - shift));
}

static uint32_t ota_sha256_load_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static void ota_sha256_store_be32(uint32_t value, uint8_t *data)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static void ota_sha256_transform(OtaSha256Context *context, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];
    uint32_t index = 0U;

    for (index = 0U; index < 16U; ++index)
    {
        w[index] = ota_sha256_load_be32(block + (index * 4U));
    }

    for (index = 16U; index < 64U; ++index)
    {
        uint32_t s0 = ota_sha256_rotr32(w[index - 15U], 7U) ^
                      ota_sha256_rotr32(w[index - 15U], 18U) ^
                      (w[index - 15U] >> 3);
        uint32_t s1 = ota_sha256_rotr32(w[index - 2U], 17U) ^
                      ota_sha256_rotr32(w[index - 2U], 19U) ^
                      (w[index - 2U] >> 10);
        w[index] = w[index - 16U] + s0 + w[index - 7U] + s1;
    }

    for (index = 0U; index < 64U; ++index)
    {
        uint32_t s1 = ota_sha256_rotr32(e, 6U) ^
                      ota_sha256_rotr32(e, 11U) ^
                      ota_sha256_rotr32(e, 25U);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + s_ota_sha256_k[index] + w[index];
        uint32_t s0 = ota_sha256_rotr32(a, 2U) ^
                      ota_sha256_rotr32(a, 13U) ^
                      ota_sha256_rotr32(a, 22U);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void OtaSha256_Init(OtaSha256Context *context)
{
    if (context == 0)
    {
        return;
    }

    memset(context, 0, sizeof(*context));
    context->state[0] = 0x6A09E667UL;
    context->state[1] = 0xBB67AE85UL;
    context->state[2] = 0x3C6EF372UL;
    context->state[3] = 0xA54FF53AUL;
    context->state[4] = 0x510E527FUL;
    context->state[5] = 0x9B05688CUL;
    context->state[6] = 0x1F83D9ABUL;
    context->state[7] = 0x5BE0CD19UL;
}

void OtaSha256_Update(OtaSha256Context *context, const uint8_t *data, uint32_t data_len)
{
    uint32_t offset = 0U;

    if (context == 0 || data == 0 || data_len == 0U)
    {
        return;
    }

    while (offset < data_len)
    {
        uint32_t copy_len = 64U - (uint32_t)context->buffer_len;
        if (copy_len > (data_len - offset))
        {
            copy_len = data_len - offset;
        }

        memcpy(context->buffer + context->buffer_len, data + offset, copy_len);
        context->buffer_len = (uint8_t)(context->buffer_len + copy_len);
        context->total_len += copy_len;
        offset += copy_len;

        if (context->buffer_len == 64U)
        {
            ota_sha256_transform(context, context->buffer);
            context->buffer_len = 0U;
        }
    }
}

void OtaSha256_Final(OtaSha256Context *context, uint8_t output[32])
{
    uint64_t bit_len = 0ULL;
    uint32_t index = 0U;

    if (context == 0 || output == 0)
    {
        return;
    }

    bit_len = ((uint64_t)context->total_len) * 8ULL;

    context->buffer[context->buffer_len++] = 0x80U;
    if (context->buffer_len > 56U)
    {
        while (context->buffer_len < 64U)
        {
            context->buffer[context->buffer_len++] = 0U;
        }
        ota_sha256_transform(context, context->buffer);
        context->buffer_len = 0U;
    }

    while (context->buffer_len < 56U)
    {
        context->buffer[context->buffer_len++] = 0U;
    }

    for (index = 0U; index < 8U; ++index)
    {
        context->buffer[56U + index] = (uint8_t)(bit_len >> ((7U - index) * 8U));
    }

    ota_sha256_transform(context, context->buffer);
    for (index = 0U; index < 8U; ++index)
    {
        ota_sha256_store_be32(context->state[index], output + (index * 4U));
    }
}

uint8_t OtaSha256_Compute(const uint8_t *data, uint32_t length, uint8_t output[32])
{
    OtaSha256Context context;

    if (data == 0 || output == 0)
    {
        return 0U;
    }

    OtaSha256_Init(&context);
    OtaSha256_Update(&context, data, length);
    OtaSha256_Final(&context, output);
    return 1U;
}

static void Ymodem_ResetHashDiagnostics(void)
{
    memset(&ymodem_hash_diagnostics, 0, sizeof(ymodem_hash_diagnostics));
}

static void Ymodem_BodyHashBegin(void)
{
    Ymodem_ResetHashDiagnostics();
    OtaSha256_Init(&ymodem_body_hash_context);
    ymodem_body_hash_active = 1U;
}

static uint8_t Ymodem_BodyHashResumeFromFlash(uint32_t address, uint32_t length)
{
    if (length == 0U)
    {
        return 1U;
    }

    if (address < FLASH_APP1_ADDR || length > FLASH_APP_MAX_SIZE)
    {
        return 0U;
    }

    OtaSha256_Update(&ymodem_body_hash_context, (const uint8_t *)address, length);
    return 1U;
}

static void Ymodem_BodyHashUpdate(const uint8_t *data, uint32_t data_len)
{
    if (ymodem_body_hash_active == 0U || data == 0 || data_len == 0U)
    {
        return;
    }

    OtaSha256_Update(&ymodem_body_hash_context, data, data_len);
}

static void Ymodem_BodyHashFinish(void)
{
    if (ymodem_body_hash_active == 0U)
    {
        return;
    }

    OtaSha256_Final(&ymodem_body_hash_context, ymodem_hash_diagnostics.body_hash);
    ymodem_hash_diagnostics.body_hash_state = YMODEM_BODY_HASH_OK;
    ymodem_body_hash_active = 0U;
}

static uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    uint32_t value = ~crc;
    uint32_t index = 0U;

    for (index = 0U; index < length; ++index)
    {
        uint32_t bit = 0U;

        value ^= data[index];
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((value & 1U) != 0U)
            {
                value = (value >> 1) ^ 0xEDB88320UL;
            }
            else
            {
                value >>= 1;
            }
        }
    }

    return ~value;
}

static uint32_t ota_data_session_id_from_fingerprint(const uint8_t fingerprint[OTA_CTRL_FINGERPRINT_LEN])
{
    return ota_crc32_update(0U, fingerprint, OTA_CTRL_FINGERPRINT_LEN);
}

static void Ymodem_ResetSessionState(void)
{
    memset(&ymodem_received_header, 0, sizeof(ymodem_received_header));
    memset(ymodem_session_fingerprint, 0, sizeof(ymodem_session_fingerprint));
    ymodem_received_header_valid = 0U;
    ymodem_received_firmware_size = 0U;
    ymodem_body_hash_active = 0U;
    ymodem_session_configured = 0U;
    ymodem_session_id = 0U;
    ymodem_session_start_offset = 0U;
    ymodem_session_checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    ymodem_checkpoint_callback = 0;
    ymodem_checkpoint_context = 0;
    memset(&ymodem_body_hash_context, 0, sizeof(ymodem_body_hash_context));
    Ymodem_ResetHashDiagnostics();
}

uint8_t Ymodem_ConfigureTransfer(const OtaImageHeaderBinary *header,
                                 const uint8_t session_fingerprint[32],
                                 uint32_t start_offset,
                                 uint32_t checkpoint_size,
                                 YmodemCheckpointCallback checkpoint_callback,
                                 void *checkpoint_context)
{
    uint8_t computed_fingerprint[OTA_CTRL_FINGERPRINT_LEN];

    Ymodem_ResetSessionState();
    Ymodem_ResetError();

    if (header == 0 || session_fingerprint == 0)
    {
        Ymodem_SetError(YMODEM_ERR_HEADER, 1U);
        return 0U;
    }

    if (header->payload.firmware_size == 0U ||
        header->payload.firmware_size > FLASH_APP_MAX_SIZE ||
        start_offset > header->payload.firmware_size ||
        (start_offset % BLOCKSIZE) != 0U ||
        (start_offset != 0U && checkpoint_size == 0U))
    {
        Ymodem_SetError(YMODEM_ERR_SIZE, 1U);
        return 0U;
    }

    if (checkpoint_size == 0U)
    {
        checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    }

    if (OtaSha256_Compute((const uint8_t *)header,
                          OTA_IMAGE_HEADER_TOTAL_SIZE,
                          computed_fingerprint) == 0U ||
        memcmp(computed_fingerprint, session_fingerprint, OTA_CTRL_FINGERPRINT_LEN) != 0)
    {
        Ymodem_SetError(YMODEM_ERR_HEADER, 1U);
        return 0U;
    }

    memcpy(&ymodem_received_header, header, sizeof(ymodem_received_header));
    memcpy(ymodem_session_fingerprint, session_fingerprint, sizeof(ymodem_session_fingerprint));
    ymodem_received_header_valid = 1U;
    ymodem_received_firmware_size = 0U;
    ymodem_session_id = ota_data_session_id_from_fingerprint(ymodem_session_fingerprint);
    ymodem_session_start_offset = start_offset;
    ymodem_session_checkpoint_size = checkpoint_size;
    ymodem_checkpoint_callback = checkpoint_callback;
    ymodem_checkpoint_context = checkpoint_context;

    if (ymodem_header_validator != 0 &&
        ymodem_header_validator(&ymodem_received_header,
                                ymodem_header_validator_context) == 0U)
    {
        if (Ymodem_GetErrorCode() == YMODEM_OK)
        {
            Ymodem_SetError(YMODEM_ERR_HEADER, 1U);
        }
        return 0U;
    }

    ymodem_last_acked_offset = start_offset;
    ymodem_session_configured = 1U;
    return 1U;
}

static uint8_t Ymodem_WriteFlashBuffered(YmodemTransferState *state,
                                         const uint8_t *data,
                                         uint32_t data_len)
{
    if (state == 0 || state->flash_buffer == 0 || data == 0)
    {
        return 0U;
    }

    while (data_len > 0U)
    {
        uint32_t space = OTA_DATA_MAX_PAYLOAD_LEN - state->flash_buffer_len;
        uint32_t copy_len = (data_len < space) ? data_len : space;

        memcpy(state->flash_buffer + state->flash_buffer_len, data, copy_len);
        state->flash_buffer_len += copy_len;
        data += copy_len;
        data_len -= copy_len;

        if (state->flash_buffer_len == OTA_DATA_MAX_PAYLOAD_LEN)
        {
            if (FLASH_If_Write(&state->flash_destination,
                               (uint32_t *)state->flash_buffer,
                               OTA_DATA_MAX_PAYLOAD_LEN / 4U) != 0U)
            {
                return 0U;
            }
            state->flash_buffer_len = 0U;
        }
    }

    return 1U;
}

static uint8_t Ymodem_FlushFlashBuffered(YmodemTransferState *state)
{
    uint32_t padded_len = 0U;

    if (state == 0 || state->flash_buffer == 0)
    {
        return 0U;
    }

    if (state->flash_buffer_len == 0U)
    {
        return 1U;
    }

    padded_len = (state->flash_buffer_len + 3U) & ~3U;
    if (padded_len > state->flash_buffer_len)
    {
        memset(state->flash_buffer + state->flash_buffer_len,
               0xFF,
               padded_len - state->flash_buffer_len);
    }

    if (FLASH_If_Write(&state->flash_destination,
                       (uint32_t *)state->flash_buffer,
                       padded_len / 4U) != 0U)
    {
        return 0U;
    }

    state->flash_buffer_len = 0U;
    return 1U;
}

static void Ymodem_FeedWatchdog(void)
{
    IWDG_ReloadCounter();
}

static int32_t Receive_Byte(uint8_t *c, uint32_t timeout)
{
    uint32_t waited_us = 0U;
    uint32_t timeout_us = timeout * 1000U;

    if (c == 0)
    {
        return OTA_DATA_PACKET_TIMEOUT;
    }

    while (waited_us < timeout_us)
    {
        if (SerialKeyPressed(c) != 0U)
        {
            return OTA_DATA_PACKET_OK;
        }

        ymodem_uart_error_flags |= SerialTakeRxErrorFlags();
        if (ymodem_uart_error_flags != 0U)
        {
            return OTA_DATA_PACKET_UART;
        }

        Ymodem_FeedWatchdog();
        delay_us(OTA_DATA_POLL_STEP_US);
        waited_us += OTA_DATA_POLL_STEP_US;
    }

    ymodem_uart_error_flags |= SerialTakeRxErrorFlags();
    if (ymodem_uart_error_flags != 0U)
    {
        return OTA_DATA_PACKET_UART;
    }

    return OTA_DATA_PACKET_TIMEOUT;
}

static uint16_t Ymodem_Crc16(const uint8_t *data, uint32_t count)
{
    uint16_t crc = 0U;
    uint32_t index = 0U;
    uint32_t bit = 0U;

    if (data == 0)
    {
        return 0U;
    }

    for (index = 0U; index < count; ++index)
    {
        crc ^= (uint16_t)data[index] << 8;
        for (bit = 0U; bit < 8U; ++bit)
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

static void ota_data_write_u16le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void ota_data_write_u32le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t ota_data_read_u16le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static uint32_t ota_data_read_u32le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

static uint8_t ota_data_send_frame(uint8_t type,
                                   uint32_t session_id,
                                   uint32_t offset,
                                   const uint8_t *payload,
                                   uint16_t payload_len)
{
    uint8_t frame[OTA_DATA_MAX_FRAME_LEN];
    uint16_t header_crc = 0U;
    uint32_t payload_crc = 0U;
    uint16_t index = 0U;
    uint16_t total_len = 0U;

    if (payload_len > OTA_DATA_MAX_PAYLOAD_LEN)
    {
        return 0U;
    }

    frame[0] = OTA_DATA_SOF1;
    frame[1] = OTA_DATA_SOF2;
    frame[2] = OTA_DATA_PROTOCOL_VERSION;
    frame[3] = type;
    ota_data_write_u32le(&frame[4], session_id);
    ota_data_write_u32le(&frame[8], offset);
    ota_data_write_u16le(&frame[12], payload_len);
    header_crc = Ymodem_Crc16(&frame[2], 12U);
    ota_data_write_u16le(&frame[14], header_crc);

    if (payload_len > 0U && payload != 0)
    {
        memcpy(&frame[OTA_DATA_FIXED_HEADER_LEN], payload, payload_len);
    }

    payload_crc = ota_crc32_update(0U, payload, payload_len);
    ota_data_write_u32le(&frame[OTA_DATA_FIXED_HEADER_LEN + payload_len], payload_crc);
    total_len = (uint16_t)(OTA_DATA_FIXED_HEADER_LEN + payload_len + OTA_DATA_TRAILER_LEN);

    for (index = 0U; index < total_len; ++index)
    {
        Send_Byte(frame[index]);
    }

    return 1U;
}

static void ota_data_send_ack(uint32_t session_id, uint32_t next_offset)
{
    (void)ota_data_send_frame(OTA_DATA_TYPE_ACK, session_id, next_offset, 0, 0U);
}

static void ota_data_send_nak(uint32_t session_id,
                              uint32_t retry_offset,
                              uint16_t reason_code,
                              uint16_t detail_code)
{
    uint8_t payload[OTA_DATA_NAK_PAYLOAD_LEN];

    ota_data_write_u16le(&payload[0], reason_code);
    ota_data_write_u16le(&payload[2], detail_code);
    (void)ota_data_send_frame(OTA_DATA_TYPE_NAK,
                              session_id,
                              retry_offset,
                              payload,
                              OTA_DATA_NAK_PAYLOAD_LEN);
}

static void ota_data_send_abort(uint32_t session_id,
                                uint32_t final_offset,
                                uint8_t stage,
                                uint8_t error_class,
                                uint16_t error_code)
{
    uint8_t payload[OTA_DATA_ABORT_PAYLOAD_LEN];

    payload[0] = stage;
    payload[1] = error_class;
    ota_data_write_u16le(&payload[2], error_code);
    (void)ota_data_send_frame(OTA_DATA_TYPE_ABORT,
                              session_id,
                              final_offset,
                              payload,
                              OTA_DATA_ABORT_PAYLOAD_LEN);
}

static int32_t ota_data_receive_frame(OtaDataFrame *frame, uint32_t timeout)
{
    uint8_t ch = 0U;
    uint8_t header[14];
    uint8_t trailer[4];
    uint16_t header_crc_recv = 0U;
    uint16_t header_crc_calc = 0U;
    uint32_t payload_crc_recv = 0U;
    uint32_t payload_crc_calc = 0U;
    uint16_t index = 0U;
    int32_t ret = OTA_DATA_PACKET_OK;

    if (frame == 0)
    {
        return OTA_DATA_PACKET_TIMEOUT;
    }

    memset(frame, 0, sizeof(*frame));

    while (1)
    {
        ret = Receive_Byte(&ch, timeout);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }

        if (ch != OTA_DATA_SOF1)
        {
            continue;
        }

        ret = Receive_Byte(&ch, 100U);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }

        if (ch == OTA_DATA_SOF2)
        {
            break;
        }
    }

    for (index = 0U; index < sizeof(header); ++index)
    {
        ret = Receive_Byte(&header[index], 100U);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }
    }

    if (header[0] != OTA_DATA_PROTOCOL_VERSION)
    {
        return OTA_DATA_PACKET_CRC;
    }

    header_crc_recv = ota_data_read_u16le(&header[12]);
    header_crc_calc = Ymodem_Crc16(header, 12U);
    if (header_crc_recv != header_crc_calc)
    {
        return OTA_DATA_PACKET_CRC;
    }

    frame->type = header[1];
    frame->session_id = ota_data_read_u32le(&header[2]);
    frame->offset = ota_data_read_u32le(&header[6]);
    frame->payload_len = ota_data_read_u16le(&header[10]);
    if (frame->payload_len > OTA_DATA_MAX_PAYLOAD_LEN)
    {
        return OTA_DATA_PACKET_CRC;
    }

    for (index = 0U; index < frame->payload_len; ++index)
    {
        ret = Receive_Byte(&frame->payload[index], 100U);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }
    }

    for (index = 0U; index < sizeof(trailer); ++index)
    {
        ret = Receive_Byte(&trailer[index], 100U);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }
    }

    payload_crc_recv = ota_data_read_u32le(trailer);
    payload_crc_calc = ota_crc32_update(0U, frame->payload, frame->payload_len);
    if (payload_crc_recv != payload_crc_calc)
    {
        return OTA_DATA_PACKET_CRC;
    }

    return OTA_DATA_PACKET_OK;
}

static void ota_ctr_build_counter(uint8_t counter[BLOCKSIZE], const uint8_t iv[BLOCKSIZE], uint32_t block_index)
{
    uint32_t carry = block_index;
    int32_t index = 0;

    memcpy(counter, iv, BLOCKSIZE);
    for (index = BLOCKSIZE - 1; index >= 0; --index)
    {
        carry += (uint32_t)counter[index];
        counter[index] = (uint8_t)(carry & 0xFFU);
        carry >>= 8;
    }
}

static void ota_ctr_crypt(uint8_t *buffer,
                          uint32_t length,
                          const uint8_t iv[BLOCKSIZE],
                          uint32_t offset)
{
    uint8_t counter[BLOCKSIZE];
    uint8_t keystream[BLOCKSIZE];
    uint32_t processed = 0U;
    uint32_t block_index = offset / BLOCKSIZE;
    uint32_t block_offset = offset % BLOCKSIZE;
    uint32_t index = 0U;
    uint32_t chunk = 0U;

    if (buffer == 0 || length == 0U || iv == 0)
    {
        return;
    }

    while (processed < length)
    {
        ota_ctr_build_counter(counter, iv, block_index);
        memcpy(keystream, counter, sizeof(keystream));
        aesEncryptBlock(keystream);

        chunk = BLOCKSIZE - block_offset;
        if (chunk > (length - processed))
        {
            chunk = length - processed;
        }

        for (index = 0U; index < chunk; ++index)
        {
            buffer[processed + index] ^= keystream[block_offset + index];
        }

        processed += chunk;
        ++block_index;
        block_offset = 0U;
    }
}

static uint32_t ota_data_slot_end_exclusive(uint32_t address)
{
    if (address == FLASH_APP1_ADDR)
    {
        return FLASH_APP1_ADDR + FLASH_APP_MAX_SIZE;
    }

    if (address == FLASH_APP2_ADDR)
    {
        return FLASH_APP2_ADDR + FLASH_APP_MAX_SIZE;
    }

    return 0U;
}

static uint32_t ota_data_sector_base(uint32_t address)
{
    uint32_t index = 0U;

    for (index = 0U;
         index + 1U < (sizeof(s_ota_flash_sector_boundaries) / sizeof(s_ota_flash_sector_boundaries[0]));
         ++index)
    {
        if (address >= s_ota_flash_sector_boundaries[index] &&
            address < s_ota_flash_sector_boundaries[index + 1U])
        {
            return s_ota_flash_sector_boundaries[index];
        }
    }

    return 0U;
}

static uint32_t ota_data_next_sector_base(uint32_t address)
{
    uint32_t index = 0U;

    for (index = 0U;
         index + 1U < (sizeof(s_ota_flash_sector_boundaries) / sizeof(s_ota_flash_sector_boundaries[0]));
         ++index)
    {
        if (address >= s_ota_flash_sector_boundaries[index] &&
            address < s_ota_flash_sector_boundaries[index + 1U])
        {
            return s_ota_flash_sector_boundaries[index + 1U];
        }
    }

    return 0U;
}

static uint8_t ota_data_erase_target_area(uint32_t address, uint32_t length)
{
    uint32_t slot_end = 0U;
    uint32_t erase_end = 0U;
    uint32_t sector_addr = 0U;
    uint32_t next_sector = 0U;

    if (length == 0U)
    {
        return 0U;
    }

    slot_end = ota_data_slot_end_exclusive(address);
    if (slot_end == 0U)
    {
        return 0U;
    }

    erase_end = address + length;
    if (erase_end < address || erase_end > slot_end)
    {
        return 0U;
    }

    sector_addr = ota_data_sector_base(address);
    if (sector_addr != address)
    {
        return 0U;
    }

    FLASH_Unlock();

    while (sector_addr < erase_end)
    {
        if (MY_FLASH_Erase(sector_addr) != 0U)
        {
            FLASH_Lock();
            return 0U;
        }

        next_sector = ota_data_next_sector_base(sector_addr);
        if (next_sector <= sector_addr)
        {
            FLASH_Lock();
            return 0U;
        }

        sector_addr = next_sector;
    }

    FLASH_Lock();
    return 1U;
}

static uint8_t ota_data_error_is_terminal(uint8_t err_code)
{
    return (err_code == YMODEM_ERR_HEADER ||
            err_code == YMODEM_ERR_AUTH ||
            err_code == YMODEM_ERR_VERSION ||
            err_code == YMODEM_ERR_SLOT) ? 1U : 0U;
}

static uint8_t ota_data_process_start_frame(const OtaDataFrame *frame, YmodemTransferState *state)
{
    uint16_t flags = 0U;
    uint16_t chunk_size = 0U;
    uint32_t checkpoint_size = 0U;
    uint32_t transfer_size = 0U;
    uint32_t plain_size = 0U;
    uint8_t resume_requested = 0U;

    if (frame == 0 || state == 0)
    {
        Ymodem_SetError(YMODEM_ERR_PROTOCOL, OTA_CTRL_STAGE_READY);
        return 0U;
    }

    if (frame->session_id != ymodem_session_id)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_SESSION,
                          0U);
        return 1U;
    }

    if (frame->payload_len != OTA_DATA_START_PAYLOAD_LEN)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_LENGTH,
                          frame->payload_len);
        return 1U;
    }

    flags = ota_data_read_u16le(&frame->payload[0]);
    chunk_size = ota_data_read_u16le(&frame->payload[2]);
    checkpoint_size = ota_data_read_u32le(&frame->payload[4]);
    transfer_size = ota_data_read_u32le(&frame->payload[8]);
    plain_size = ota_data_read_u32le(&frame->payload[12]);
    resume_requested = ((flags & OTA_DATA_START_FLAG_RESUME) != 0U) ? 1U : 0U;

    if (memcmp(&frame->payload[16], ymodem_session_fingerprint, OTA_CTRL_FINGERPRINT_LEN) != 0)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_SESSION,
                          1U);
        return 1U;
    }

    if (chunk_size == 0U ||
        chunk_size > OTA_DATA_MAX_PAYLOAD_LEN ||
        checkpoint_size == 0U ||
        transfer_size != ymodem_received_header.payload.firmware_size ||
        plain_size != ymodem_received_header.payload.firmware_size ||
        frame->offset != state->start_offset)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_PROTOCOL,
                          0U);
        return 1U;
    }

    if (((state->start_offset != 0U) && (resume_requested == 0U)) ||
        ((state->start_offset == 0U) && (resume_requested != 0U)))
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_STATE,
                          0U);
        return 1U;
    }

    state->transfer_size = transfer_size;
    state->checkpoint_size = checkpoint_size;
    state->chunk_size = chunk_size;
    state->firmware_received = state->start_offset;
    state->durable_offset = state->start_offset;
    state->last_acked_offset = state->start_offset;
    state->flash_destination = APPLICATION_ADDRESS + state->start_offset;

    Ymodem_BodyHashBegin();
    if (state->start_offset == 0U)
    {
        if (ota_data_erase_target_area(APPLICATION_ADDRESS, state->transfer_size) == 0U)
        {
            Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_TRANSFER);
            return 0U;
        }
    }
    else if (Ymodem_BodyHashResumeFromFlash(APPLICATION_ADDRESS, state->start_offset) == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_AUTH, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    state->flash_ready = 1U;
    ymodem_last_acked_offset = state->start_offset;
    ota_data_send_ack(ymodem_session_id, state->start_offset);
    Ymodem_NotifyProgress(state->start_offset, state->transfer_size);
    return 1U;
}

static uint8_t ota_data_process_chunk_frame(const OtaDataFrame *frame, YmodemTransferState *state)
{
    uint8_t plain[OTA_DATA_MAX_PAYLOAD_LEN];

    if (frame == 0 || state == 0)
    {
        Ymodem_SetError(YMODEM_ERR_PROTOCOL, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    if (frame->session_id != ymodem_session_id)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_SESSION,
                          0U);
        return 1U;
    }

    if (state->flash_ready == 0U)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_STATE,
                          0U);
        return 1U;
    }

    if (frame->payload_len == 0U ||
        frame->payload_len > state->chunk_size ||
        (frame->offset + frame->payload_len) > state->transfer_size)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_LENGTH,
                          frame->payload_len);
        return 1U;
    }

    if (frame->offset < state->firmware_received)
    {
        ota_data_send_ack(ymodem_session_id, state->firmware_received);
        return 1U;
    }

    if (frame->offset != state->firmware_received)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_OFFSET,
                          0U);
        return 1U;
    }

    memcpy(plain, frame->payload, frame->payload_len);
    ota_ctr_crypt(plain,
                  frame->payload_len,
                  ymodem_received_header.payload.iv,
                  frame->offset);

    Ymodem_BodyHashUpdate(plain, frame->payload_len);
    if (Ymodem_WriteFlashBuffered(state, plain, frame->payload_len) == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    state->firmware_received += frame->payload_len;
    state->last_acked_offset = state->firmware_received;
    ymodem_last_acked_offset = state->last_acked_offset;

    if (ota_data_maybe_persist_progress(state) == 0U)
    {
        return 0U;
    }

    ota_data_send_ack(ymodem_session_id, state->firmware_received);
    Ymodem_NotifyProgress(state->firmware_received, state->transfer_size);
    return 1U;
}

static uint8_t ota_data_maybe_persist_progress(YmodemTransferState *state)
{
    uint32_t persist_offset = 0U;

    if (state == 0)
    {
        Ymodem_SetError(YMODEM_ERR_PROTOCOL, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    if (state->firmware_received <= state->durable_offset)
    {
        return 1U;
    }

    if (state->durable_offset == state->start_offset)
    {
        uint32_t first_persist_target = state->start_offset + s_ymodem_resume_first_persist_bytes;

        if (state->firmware_received < first_persist_target)
        {
            return 1U;
        }

        persist_offset = first_persist_target;
    }
    else if ((state->firmware_received - state->durable_offset) >= s_ymodem_resume_persist_stride_bytes)
    {
        persist_offset = state->firmware_received;
    }
    else
    {
        return 1U;
    }

    if (persist_offset > state->firmware_received)
    {
        persist_offset = state->firmware_received;
    }

    if (persist_offset <= state->durable_offset)
    {
        return 1U;
    }

    if (Ymodem_FlushFlashBuffered(state) == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    if (ymodem_checkpoint_callback != 0 &&
        ymodem_checkpoint_callback(persist_offset, ymodem_checkpoint_context) == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_DONE);
        return 0U;
    }

    state->durable_offset = persist_offset;
    return 1U;
}

static uint8_t ota_data_finalize_transfer(YmodemTransferState *state)
{
    if (state == 0)
    {
        Ymodem_SetError(YMODEM_ERR_PROTOCOL, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    if (state->firmware_received != state->transfer_size)
    {
        Ymodem_SetError(YMODEM_ERR_SIZE, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    if (Ymodem_FlushFlashBuffered(state) == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    if (state->durable_offset != state->transfer_size)
    {
        if (ymodem_checkpoint_callback != 0 &&
            ymodem_checkpoint_callback(state->transfer_size, ymodem_checkpoint_context) == 0U)
        {
            Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_DONE);
            return 0U;
        }

        state->durable_offset = state->transfer_size;
    }

    Ymodem_BodyHashFinish();
    ymodem_received_firmware_size = state->transfer_size;
    ymodem_last_acked_offset = state->transfer_size;
    Ymodem_NotifyProgress(state->transfer_size, state->transfer_size);
    return 1U;
}

int32_t Ymodem_Receive(uint8_t *buf)
{
    OtaDataFrame frame;
    YmodemTransferState state;
    uint8_t session_started = 0U;
    int32_t ret = OTA_DATA_PACKET_OK;

    if (buf == 0 || ymodem_session_configured == 0U || ymodem_received_header_valid == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_HEADER, OTA_CTRL_STAGE_READY);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.flash_buffer = buf;
    state.start_offset = ymodem_session_start_offset;
    state.checkpoint_size = ymodem_session_checkpoint_size;
    state.transfer_size = ymodem_received_header.payload.firmware_size;
    state.durable_offset = ymodem_session_start_offset;
    state.firmware_received = ymodem_session_start_offset;
    state.last_acked_offset = ymodem_session_start_offset;

    Ymodem_ResetError();
    aesEncInit();

    while (1)
    {
        ret = ota_data_receive_frame(&frame, 5000U);
        if (ret == OTA_DATA_PACKET_TIMEOUT)
        {
            Ymodem_SetError(YMODEM_ERR_TIMEOUT, OTA_CTRL_STAGE_TRANSFER);
            ota_data_send_abort(ymodem_session_id,
                                state.last_acked_offset,
                                OTA_CTRL_STAGE_TRANSFER,
                                OTA_DATA_ABORT_CLASS_RETRYABLE,
                                YMODEM_ERR_TIMEOUT);
            return -1;
        }
        if (ret == OTA_DATA_PACKET_UART)
        {
            Ymodem_SetError(YMODEM_ERR_UART, OTA_CTRL_STAGE_TRANSFER);
            ota_data_send_abort(ymodem_session_id,
                                state.last_acked_offset,
                                OTA_CTRL_STAGE_TRANSFER,
                                OTA_DATA_ABORT_CLASS_RETRYABLE,
                                YMODEM_ERR_UART);
            return -1;
        }
        if (ret == OTA_DATA_PACKET_CRC)
        {
            ota_data_send_nak(ymodem_session_id,
                              state.firmware_received,
                              OTA_DATA_NAK_REASON_PAYLOAD_CRC,
                              0U);
            continue;
        }

        switch (frame.type)
        {
        case OTA_DATA_TYPE_START:
            if (session_started != 0U)
            {
                ota_data_send_ack(ymodem_session_id, state.firmware_received);
                continue;
            }

            if (ota_data_process_start_frame(&frame, &state) == 0U)
            {
                ota_data_send_abort(ymodem_session_id,
                                    state.last_acked_offset,
                                    Ymodem_GetErrorStage(),
                                    OTA_DATA_ABORT_CLASS_TERMINAL,
                                    Ymodem_GetErrorCode());
                return -1;
            }
            session_started = 1U;
            break;

        case OTA_DATA_TYPE_CHUNK:
            if (session_started == 0U)
            {
                ota_data_send_nak(ymodem_session_id,
                                  state.firmware_received,
                                  OTA_DATA_NAK_REASON_STATE,
                                  0U);
                continue;
            }

            if (ota_data_process_chunk_frame(&frame, &state) == 0U)
            {
                ota_data_send_abort(ymodem_session_id,
                                    state.last_acked_offset,
                                    Ymodem_GetErrorStage(),
                                    (ota_data_error_is_terminal(Ymodem_GetErrorCode()) != 0U) ?
                                        OTA_DATA_ABORT_CLASS_TERMINAL :
                                        OTA_DATA_ABORT_CLASS_RETRYABLE,
                                    Ymodem_GetErrorCode());
                return -1;
            }
            break;

        case OTA_DATA_TYPE_FINISH:
            if (session_started == 0U)
            {
                ota_data_send_nak(ymodem_session_id,
                                  state.firmware_received,
                                  OTA_DATA_NAK_REASON_STATE,
                                  0U);
                continue;
            }

            if (frame.session_id != ymodem_session_id ||
                frame.payload_len != 0U ||
                frame.offset != state.transfer_size)
            {
                ota_data_send_nak(ymodem_session_id,
                                  state.firmware_received,
                                  OTA_DATA_NAK_REASON_PROTOCOL,
                                  0U);
                continue;
            }

            if (ota_data_finalize_transfer(&state) == 0U)
            {
                ota_data_send_abort(ymodem_session_id,
                                    state.last_acked_offset,
                                    Ymodem_GetErrorStage(),
                                    OTA_DATA_ABORT_CLASS_RETRYABLE,
                                    Ymodem_GetErrorCode());
                return -1;
            }

            ota_data_send_ack(ymodem_session_id, state.transfer_size);
            return (int32_t)state.transfer_size;

        case OTA_DATA_TYPE_ABORT:
            Ymodem_SetError(YMODEM_ERR_ABORT, OTA_CTRL_STAGE_TRANSFER);
            return -1;

        default:
            ota_data_send_nak(ymodem_session_id,
                              state.firmware_received,
                              OTA_DATA_NAK_REASON_PROTOCOL,
                              frame.type);
            break;
        }
    }
}
