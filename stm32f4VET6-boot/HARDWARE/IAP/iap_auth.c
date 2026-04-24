#include "iap_auth.h"
#include "uart_rx_ring.h"

#define OTA_RSA_WORD_COUNT OTA_RSA_PUBLIC_KEY_WORD_COUNT
#define OTA_RSA_MODULUS_BYTES OTA_RSA_PUBLIC_KEY_MODULUS_BYTES

static const uint8_t s_sha256_self_test_input[3] = { 'a', 'b', 'c' };
static const uint8_t s_sha256_self_test_output[32] =
{
    0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
    0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
    0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
    0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD
};
static const uint8_t s_sha256_digestinfo_prefix[19] =
{
    0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

static uint8_t iap_set_header_error(iap_header_validation_context_t *context, uint8_t code)
{
    if (context != 0)
    {
        context->error_code = code;
    }

    Ymodem_SetError(code, 2U);
    return 0U;
}

static void iap_auth_diag_reset(iap_auth_diag_t *diag)
{
    if (diag != 0)
    {
        memset(diag, 0, sizeof(*diag));
    }
}

static uint8_t ota_sha256_self_test(void)
{
    uint8_t hash[32];

    if (OtaSha256_Compute(s_sha256_self_test_input,
                          sizeof(s_sha256_self_test_input),
                          hash) == 0U)
    {
        return 0U;
    }

    return (memcmp(hash, s_sha256_self_test_output, sizeof(hash)) == 0) ? 1U : 0U;
}

static uint8_t iap_compute_flash_hash(uint32_t firmware_address,
                                      uint32_t firmware_size,
                                      uint8_t output[32])
{
    if (firmware_address < FLASH_APP1_ADDR ||
        firmware_size == 0U ||
        firmware_size > FLASH_APP_MAX_SIZE ||
        output == 0)
    {
        return 0U;
    }

    return OtaSha256_Compute((const uint8_t *)firmware_address, firmware_size, output);
}

static void ota_rsa_words_zero(uint32_t *words)
{
    memset(words, 0, OTA_RSA_WORD_COUNT * sizeof(uint32_t));
}

static void ota_rsa_words_copy(uint32_t *target, const uint32_t *source)
{
    memcpy(target, source, OTA_RSA_WORD_COUNT * sizeof(uint32_t));
}

static int32_t ota_rsa_words_compare(const uint32_t *left, const uint32_t *right)
{
    int32_t index = 0;

    for (index = (int32_t)OTA_RSA_WORD_COUNT - 1; index >= 0; --index)
    {
        if (left[index] > right[index])
        {
            return 1;
        }
        if (left[index] < right[index])
        {
            return -1;
        }
    }

    return 0;
}

static uint32_t ota_rsa_words_add(uint32_t *target, const uint32_t *source)
{
    uint64_t carry = 0ULL;
    uint32_t index = 0U;

    for (index = 0U; index < OTA_RSA_WORD_COUNT; ++index)
    {
        uint64_t value = (uint64_t)target[index] + (uint64_t)source[index] + carry;
        target[index] = (uint32_t)value;
        carry = value >> 32;
    }

    return (uint32_t)carry;
}

static uint32_t ota_rsa_words_sub_inplace(uint32_t *target, const uint32_t *source)
{
    uint64_t borrow = 0ULL;
    uint32_t index = 0U;

    for (index = 0U; index < OTA_RSA_WORD_COUNT; ++index)
    {
        uint64_t left = (uint64_t)target[index];
        uint64_t right = (uint64_t)source[index] + borrow;

        if (left >= right)
        {
            target[index] = (uint32_t)(left - right);
            borrow = 0ULL;
        }
        else
        {
            target[index] = (uint32_t)(((uint64_t)1ULL << 32) + left - right);
            borrow = 1ULL;
        }
    }

    return (uint32_t)borrow;
}

static void ota_rsa_mod_add_inplace(uint32_t *accumulator,
                                    const uint32_t *addend,
                                    const uint32_t *modulus)
{
    uint32_t threshold[OTA_RSA_WORD_COUNT];

    ota_rsa_words_copy(threshold, modulus);
    ota_rsa_words_sub_inplace(threshold, addend);

    if (ota_rsa_words_compare(accumulator, threshold) >= 0)
    {
        ota_rsa_words_sub_inplace(accumulator, threshold);
    }
    else
    {
        ota_rsa_words_add(accumulator, addend);
    }
}

static void ota_rsa_mod_double_inplace(uint32_t *value, const uint32_t *modulus)
{
    uint32_t threshold[OTA_RSA_WORD_COUNT];

    ota_rsa_words_copy(threshold, modulus);
    ota_rsa_words_sub_inplace(threshold, value);

    if (ota_rsa_words_compare(value, threshold) >= 0)
    {
        ota_rsa_words_sub_inplace(value, threshold);
    }
    else
    {
        ota_rsa_words_add(value, value);
    }
}

static void ota_rsa_mod_mul(uint32_t *result,
                            const uint32_t *left,
                            const uint32_t *right,
                            const uint32_t *modulus)
{
    uint32_t base[OTA_RSA_WORD_COUNT];
    uint32_t output[OTA_RSA_WORD_COUNT];
    uint32_t word_index = 0U;

    ota_rsa_words_zero(output);
    ota_rsa_words_copy(base, left);

    for (word_index = 0U; word_index < OTA_RSA_WORD_COUNT; ++word_index)
    {
        uint32_t bit_value = right[word_index];
        uint32_t bit_index = 0U;

        for (bit_index = 0U; bit_index < 32U; ++bit_index)
        {
            if ((bit_value & 1U) != 0U)
            {
                ota_rsa_mod_add_inplace(output, base, modulus);
            }

            bit_value >>= 1U;
            ota_rsa_mod_double_inplace(base, modulus);
        }
    }

    ota_rsa_words_copy(result, output);
}

static void ota_rsa_words_from_bytes_be(uint32_t *words, const uint8_t *bytes, uint32_t byte_len)
{
    uint32_t offset = 0U;

    ota_rsa_words_zero(words);
    if (bytes == 0)
    {
        return;
    }

    for (offset = 0U; offset < byte_len && offset < OTA_RSA_MODULUS_BYTES; ++offset)
    {
        uint32_t source_index = byte_len - 1U - offset;
        uint32_t word_index = offset / 4U;
        uint32_t byte_index = offset % 4U;
        words[word_index] |= ((uint32_t)bytes[source_index]) << (byte_index * 8U);
    }
}

static void ota_rsa_words_to_bytes_be(const uint32_t *words, uint8_t *bytes, uint32_t byte_len)
{
    uint32_t offset = 0U;

    if (words == 0 || bytes == 0)
    {
        return;
    }

    for (offset = 0U; offset < byte_len; ++offset)
    {
        uint32_t source_index = byte_len - 1U - offset;
        uint32_t word_index = offset / 4U;
        uint32_t byte_index = offset % 4U;
        bytes[source_index] = (uint8_t)(words[word_index] >> (byte_index * 8U));
    }
}

static uint8_t ota_rsa_verify_signature(const uint8_t *signature,
                                        uint32_t signature_len,
                                        const uint8_t hash[32])
{
    uint32_t signature_words[OTA_RSA_WORD_COUNT];
    uint32_t result_words[OTA_RSA_WORD_COUNT];
    uint32_t base_words[OTA_RSA_WORD_COUNT];
    uint32_t temp_words[OTA_RSA_WORD_COUNT];
    uint8_t encoded[OTA_RSA_MODULUS_BYTES];
    uint32_t exponent = OTA_RSA_PUBLIC_KEY_PUBLIC_EXPONENT;
    uint32_t offset = 0U;

    if (signature == 0 || hash == 0 || signature_len != OTA_RSA_MODULUS_BYTES)
    {
        return 0U;
    }

    ota_rsa_words_from_bytes_be(signature_words, signature, signature_len);
    if (ota_rsa_words_compare(signature_words, s_ota_rsa_public_key_modulus_words) >= 0)
    {
        return 0U;
    }

    ota_rsa_words_zero(result_words);
    result_words[0] = 1U;
    ota_rsa_words_copy(base_words, signature_words);

    while (exponent != 0U)
    {
        if ((exponent & 1U) != 0U)
        {
            ota_rsa_mod_mul(temp_words, result_words, base_words, s_ota_rsa_public_key_modulus_words);
            ota_rsa_words_copy(result_words, temp_words);
        }

        exponent >>= 1U;
        if (exponent != 0U)
        {
            ota_rsa_mod_mul(temp_words, base_words, base_words, s_ota_rsa_public_key_modulus_words);
            ota_rsa_words_copy(base_words, temp_words);
        }
    }

    ota_rsa_words_to_bytes_be(result_words, encoded, sizeof(encoded));

    if (encoded[0] != 0x00U || encoded[1] != 0x01U)
    {
        return 0U;
    }

    offset = 2U;
    while (offset < sizeof(encoded) && encoded[offset] == 0xFFU)
    {
        ++offset;
    }

    if (offset < 10U || offset >= sizeof(encoded) || encoded[offset] != 0x00U)
    {
        return 0U;
    }

    ++offset;
    if (offset + sizeof(s_sha256_digestinfo_prefix) + 32U != sizeof(encoded))
    {
        return 0U;
    }

    if (memcmp(&encoded[offset], s_sha256_digestinfo_prefix, sizeof(s_sha256_digestinfo_prefix)) != 0)
    {
        return 0U;
    }

    offset += sizeof(s_sha256_digestinfo_prefix);
    return (memcmp(&encoded[offset], hash, 32U) == 0) ? 1U : 0U;
}

static void iap_serial_put_hex_byte(uint8_t value)
{
    static const char s_hex[] = "0123456789ABCDEF";

    Send_Byte((uint8_t)s_hex[(value >> 4) & 0x0FU]);
    Send_Byte((uint8_t)s_hex[value & 0x0FU]);
}

static void iap_serial_put_hash_prefix(const char *label,
                                       const uint8_t hash[32],
                                       uint8_t present)
{
    uint32_t index = 0U;

    Serial_PutString((uint8_t *)label);
    if (present == 0U)
    {
        Serial_PutString((uint8_t *)"--------");
        return;
    }

    for (index = 0U; index < 4U; ++index)
    {
        iap_serial_put_hex_byte(hash[index]);
    }
}

static void iap_serial_put_u32(uint32_t value)
{
    uint8_t digits[10];
    uint32_t count = 0U;

    do
    {
        digits[count++] = (uint8_t)('0' + (value % 10U));
        value /= 10U;
    } while (value > 0U && count < sizeof(digits));

    while (count > 0U)
    {
        Send_Byte(digits[--count]);
    }
}

static const char *iap_txn_load_source_text(uint8_t source)
{
    switch (source)
    {
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

uint8_t iap_validate_received_header(const OtaImageHeaderBinary *header, void *context)
{
    iap_header_validation_context_t *validation = (iap_header_validation_context_t *)context;
    const BootInfoTypeDef *boot_info = (validation != 0) ? validation->boot_info : 0;
    uint32_t inactive_slot = OTA_CTRL_PARTITION_APP2;

    if (header == 0 || boot_info == 0)
    {
        return iap_set_header_error(validation, YMODEM_ERR_HEADER);
    }

    if (header->envelope.magic != OTA_IMAGE_HEADER_MAGIC ||
        header->envelope.header_version != OTA_IMAGE_HEADER_VERSION ||
        header->envelope.header_size != OTA_IMAGE_HEADER_TOTAL_SIZE ||
        header->envelope.signature_len != OTA_IMAGE_SIGNATURE_LEN)
    {
        return iap_set_header_error(validation, YMODEM_ERR_HEADER);
    }

    if (header->payload.format_version != OTA_IMAGE_FORMAT_VERSION ||
        header->payload.signature_algorithm != OTA_IMAGE_SIG_ALG_RSA2048_SHA256_PKCS1V15)
    {
        return iap_set_header_error(validation, YMODEM_ERR_HEADER);
    }

    if (header->payload.firmware_size == 0U ||
        header->payload.firmware_size > FLASH_APP_MAX_SIZE)
    {
        return iap_set_header_error(validation, YMODEM_ERR_SIZE);
    }

    if (version_text_is_valid(header->payload.firmware_version) == 0U ||
        version_text_is_valid(header->payload.min_allowed_version) == 0U)
    {
        return iap_set_header_error(validation, YMODEM_ERR_VERSION);
    }

    inactive_slot = boot_info_inactive_partition(boot_info->active_partition);
    if (header->payload.target_slot != boot_info->target_partition ||
        header->payload.target_slot != inactive_slot)
    {
        return iap_set_header_error(validation, YMODEM_ERR_SLOT);
    }

    if (version_text_compare(header->payload.firmware_version,
                             boot_info->min_allowed_ota_version) < 0)
    {
        return iap_set_header_error(validation, YMODEM_ERR_VERSION);
    }

    return 1U;
}

uint8_t iap_verify_image_header_signature(const OtaImageHeaderBinary *header)
{
    uint8_t payload_hash[32];

    if (header == 0)
    {
        return 0U;
    }

    if (header->envelope.magic != OTA_IMAGE_HEADER_MAGIC ||
        header->envelope.header_version != OTA_IMAGE_HEADER_VERSION ||
        header->envelope.header_size != OTA_IMAGE_HEADER_TOTAL_SIZE ||
        header->envelope.signature_len != OTA_IMAGE_SIGNATURE_LEN)
    {
        return 0U;
    }

    if (OtaSha256_Compute((const uint8_t *)&header->payload,
                          OTA_IMAGE_HEADER_PAYLOAD_SIZE,
                          payload_hash) == 0U)
    {
        return 0U;
    }

    return ota_rsa_verify_signature(header->signature,
                                    header->envelope.signature_len,
                                    payload_hash);
}

iap_auth_result_t iap_authorize_received_image(const BootInfoTypeDef *boot_info,
                                               const OtaImageHeaderBinary *header,
                                               uint32_t firmware_address,
                                               iap_auth_diag_t *diag)
{
    const YmodemHashDiagnostics *hash_diag = 0;
    uint8_t payload_hash[32];

    iap_auth_diag_reset(diag);

    if (boot_info == 0 || header == 0 || diag == 0)
    {
        return IAP_AUTH_RESULT_INTERNAL;
    }

    memcpy(diag->expected_hash, header->payload.firmware_sha256, sizeof(diag->expected_hash));
    diag->has_expected = 1U;

    if (header->payload.firmware_size == 0U ||
        header->payload.firmware_size > FLASH_APP_MAX_SIZE ||
        Ymodem_GetReceivedFirmwareSize() != header->payload.firmware_size)
    {
        return IAP_AUTH_RESULT_SIZE;
    }

    if (ota_sha256_self_test() == 0U)
    {
        diag->hash_diag = IAP_HASH_DIAG_SELF_TEST;
        return IAP_AUTH_RESULT_HASH_ENGINE;
    }

    hash_diag = Ymodem_GetHashDiagnostics();
    if (hash_diag == 0 || hash_diag->body_hash_state != YMODEM_BODY_HASH_OK)
    {
        diag->hash_diag = IAP_HASH_DIAG_BODY_CALC;
        return IAP_AUTH_RESULT_HASH_ENGINE;
    }

    memcpy(diag->body_hash, hash_diag->body_hash, sizeof(diag->body_hash));
    diag->has_body = 1U;

    if (iap_compute_flash_hash(firmware_address,
                               header->payload.firmware_size,
                               diag->flash_hash) == 0U)
    {
        diag->hash_diag = IAP_HASH_DIAG_FLASH_CALC;
        return IAP_AUTH_RESULT_HASH_ENGINE;
    }
    diag->has_flash = 1U;

    if (memcmp(diag->body_hash, diag->expected_hash, sizeof(diag->body_hash)) != 0)
    {
        if (memcmp(diag->body_hash, diag->flash_hash, sizeof(diag->body_hash)) == 0)
        {
            diag->hash_diag = IAP_HASH_DIAG_EXPECTED_MISMATCH;
        }
        else
        {
            diag->hash_diag = IAP_HASH_DIAG_BODY_MISMATCH;
        }
        return IAP_AUTH_RESULT_HASH_MISMATCH;
    }

    if (memcmp(diag->flash_hash, diag->expected_hash, sizeof(diag->flash_hash)) != 0)
    {
        diag->hash_diag = IAP_HASH_DIAG_FLASH_MISMATCH;
        return IAP_AUTH_RESULT_HASH_MISMATCH;
    }

    if (OtaSha256_Compute((const uint8_t *)&header->payload,
                          OTA_IMAGE_HEADER_PAYLOAD_SIZE,
                          payload_hash) == 0U)
    {
        diag->hash_diag = IAP_HASH_DIAG_SELF_TEST;
        return IAP_AUTH_RESULT_HASH_ENGINE;
    }

    if (ota_rsa_verify_signature(header->signature,
                                 header->envelope.signature_len,
                                 payload_hash) == 0U)
    {
        return IAP_AUTH_RESULT_RSA;
    }

    (void)boot_info;
    return IAP_AUTH_RESULT_OK;
}

const char *iap_auth_result_text(iap_auth_result_t result, const iap_auth_diag_t *diag)
{
    switch (result)
    {
    case IAP_AUTH_RESULT_SIZE:
        return "AUTH SIZE";
    case IAP_AUTH_RESULT_HASH_ENGINE:
        if (diag != 0)
        {
            if (diag->hash_diag == IAP_HASH_DIAG_BODY_CALC)
            {
                return "AUTH HBDC";
            }
            if (diag->hash_diag == IAP_HASH_DIAG_FLASH_CALC)
            {
                return "AUTH HFLC";
            }
            if (diag->hash_diag == IAP_HASH_DIAG_SELF_TEST)
            {
                return "AUTH HSTF";
            }
        }
        return "AUTH HMIS";
    case IAP_AUTH_RESULT_HASH_MISMATCH:
        if (diag != 0)
        {
            if (diag->hash_diag == IAP_HASH_DIAG_BODY_MISMATCH)
            {
                return "AUTH HBDY";
            }
            if (diag->hash_diag == IAP_HASH_DIAG_FLASH_MISMATCH)
            {
                return "AUTH HFLS";
            }
            if (diag->hash_diag == IAP_HASH_DIAG_EXPECTED_MISMATCH)
            {
                return "AUTH HEXP";
            }
        }
        return "AUTH HMIS";
    case IAP_AUTH_RESULT_RSA:
        return "AUTH RSA";
    default:
        return "AUTH INT";
    }
}

void iap_serial_report_code(const char *code)
{
    if (code == 0)
    {
        return;
    }

    Serial_PutString((uint8_t *)"[BOOT] RES ");
    Serial_PutString((uint8_t *)code);
    Serial_PutString((uint8_t *)"\r\n");
}

void iap_serial_report_auth_failure(iap_auth_result_t result, const iap_auth_diag_t *diag)
{
    iap_serial_report_code(iap_auth_result_text(result, diag));

    if (diag == 0)
    {
        return;
    }

    Serial_PutString((uint8_t *)"[BOOT] SHA ");
    iap_serial_put_hash_prefix("E=", diag->expected_hash, diag->has_expected);
    Send_Byte(' ');
    iap_serial_put_hash_prefix("B=", diag->body_hash, diag->has_body);
    Send_Byte(' ');
    iap_serial_put_hash_prefix("F=", diag->flash_hash, diag->has_flash);
    Serial_PutString((uint8_t *)"\r\n");
}

void iap_serial_report_txn_load(const OtaTxnRecord *txn, const iap_txn_load_diag_t *diag)
{
    if (txn == 0 || diag == 0)
    {
        return;
    }

    Serial_PutString((uint8_t *)"[BOOT] TXN src=");
    Serial_PutString((uint8_t *)iap_txn_load_source_text(diag->source));

    Serial_PutString((uint8_t *)" st=");
    iap_serial_put_u32(txn->state);

    Serial_PutString((uint8_t *)" off=");
    iap_serial_put_u32(txn->resume_offset);

    Serial_PutString((uint8_t *)" ack=");
    iap_serial_put_u32(txn->last_acked_offset);

    Serial_PutString((uint8_t *)" ck=");
    iap_serial_put_u32(txn->checkpoint_size);

    Serial_PutString((uint8_t *)" proto=");
    iap_serial_put_u32(txn->protocol_version);

    Serial_PutString((uint8_t *)" inv=");
    iap_serial_put_u32(diag->invalid_slots);

    Serial_PutString((uint8_t *)" seq=");
    iap_serial_put_u32(diag->latest_seq);
    Serial_PutString((uint8_t *)"\r\n");

    if (diag->invalid_slots == 0U)
    {
        return;
    }

    Serial_PutString((uint8_t *)"[BOOT] TXNINV slot hdr=");
    iap_serial_put_u32(diag->invalid_slot_header);
    Serial_PutString((uint8_t *)" commit=");
    iap_serial_put_u32(diag->invalid_slot_commit);
    Serial_PutString((uint8_t *)" pcrc=");
    iap_serial_put_u32(diag->invalid_slot_payload_crc);
    Serial_PutString((uint8_t *)" pval=");
    iap_serial_put_u32(diag->invalid_slot_payload_validate);
    Serial_PutString((uint8_t *)"\r\n");

    Serial_PutString((uint8_t *)"[BOOT] TXNINV pay lay=");
    iap_serial_put_u32(diag->invalid_txn_layout);
    Serial_PutString((uint8_t *)" st=");
    iap_serial_put_u32(diag->invalid_txn_state);
    Serial_PutString((uint8_t *)" part=");
    iap_serial_put_u32(diag->invalid_txn_partition);
    Serial_PutString((uint8_t *)" fld=");
    iap_serial_put_u32(diag->invalid_txn_fields);
    Serial_PutString((uint8_t *)" req=");
    iap_serial_put_u32(diag->invalid_txn_request_type);
    Serial_PutString((uint8_t *)" ver=");
    iap_serial_put_u32(diag->invalid_txn_version_text);
    Serial_PutString((uint8_t *)" off=");
    iap_serial_put_u32(diag->invalid_txn_offset);
    Serial_PutString((uint8_t *)" crc=");
    iap_serial_put_u32(diag->invalid_txn_data_crc);
    Serial_PutString((uint8_t *)"\r\n");
}

void iap_serial_report_uart_flags(uint32_t flags)
{
    if (flags == 0U)
    {
        return;
    }

    Serial_PutString((uint8_t *)"[BOOT] UART");
    if ((flags & UART_RX_RING_FLAG_OVERFLOW) != 0U)
    {
        Serial_PutString((uint8_t *)" OVF");
    }
    if ((flags & UART_RX_RING_FLAG_ORE) != 0U)
    {
        Serial_PutString((uint8_t *)" ORE");
    }
    if ((flags & UART_RX_RING_FLAG_FE) != 0U)
    {
        Serial_PutString((uint8_t *)" FE");
    }
    if ((flags & UART_RX_RING_FLAG_NE) != 0U)
    {
        Serial_PutString((uint8_t *)" NE");
    }
    Serial_PutString((uint8_t *)"\r\n");
}
