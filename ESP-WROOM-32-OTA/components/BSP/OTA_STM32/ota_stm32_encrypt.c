#include "ota_stm32_encrypt.h"

#define TAG OTA_STM32_TAG

#if defined(__has_include)
#if __has_include("ota_security_secrets.h")
#include "ota_security_secrets.h"
#define OTA_HAS_EXTERNAL_AES_KEY   (1)
#endif
#endif

#if defined(OTA_HAS_EXTERNAL_AES_KEY)
#ifndef OTA_SECURITY_AES_KEY_ID_TEXT
#define OTA_SECURITY_AES_KEY_ID_TEXT "prod-env-aes256"
#endif
#else
#define OTA_SECURITY_AES_KEY_ID_TEXT "not-configured"
#endif

#if defined(OTA_HAS_EXTERNAL_AES_KEY)
const uint8_t g_ota_aes_key[OTA_AES_KEY_BYTES] = OTA_SECURITY_AES_KEY_BYTES;
#else
const uint8_t g_ota_aes_key[OTA_AES_KEY_BYTES] =
{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
#endif

static const uint8_t s_self_test_key[OTA_AES_KEY_BYTES] =
{
    0x9D, 0xD2, 0x00, 0x24, 0x84, 0x60, 0x2E, 0xDA,
    0x0C, 0xDD, 0x52, 0x7B, 0x05, 0xC1, 0x6B, 0x01,
    0xFF, 0x17, 0xCD, 0x6F, 0x8C, 0x1E, 0x3E, 0x09,
    0xCF, 0x1F, 0x0C, 0x78, 0x87, 0xEF, 0x8A, 0xEC
};
static const uint8_t s_self_test_iv[OTA_AES_BLOCK_SIZE] = {0};
static const char s_expected_self_test_cipher_hex[] = "225D1E09230FC3BEF289E71360796ABD58BCCE777E7F8DD465E072367C";

bool ota_aes_uses_external_key(void)
{
#if defined(OTA_HAS_EXTERNAL_AES_KEY)
    return true;
#else
    return false;
#endif
}

bool ota_aes_runtime_ready(void)
{
    if (!ota_aes_uses_external_key()) {
        return false;
    }

    for (size_t i = 0U; i < OTA_AES_KEY_BYTES; ++i) {
        if (g_ota_aes_key[i] != 0U) {
            return true;
        }
    }

    return false;
}

const char *ota_aes_key_id_text(void)
{
    return OTA_SECURITY_AES_KEY_ID_TEXT;
}

void ota_log_aes_security_profile(void)
{
    if (ota_aes_runtime_ready()) {
        ESP_LOGI(TAG,
                 "OTA AES key source: local ota_security_secrets.h, keyId=%s",
                 ota_aes_key_id_text());
        return;
    }

    ESP_LOGW(TAG,
             "OTA AES key is not configured. Copy ota_security_secrets.h.example to ota_security_secrets.h and fill a real 32-byte key before enabling OTA transfer. keyId=%s",
             ota_aes_key_id_text());
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

int hex_char_to_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }

    ch = (char)toupper((unsigned char)ch);
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }

    return -1;
}

bool hex_to_bytes(const char *hex, uint8_t *buffer, size_t buffer_len)
{
    size_t hex_len = 0U;

    if (hex == NULL || buffer == NULL) {
        return false;
    }

    hex_len = strlen(hex);
    if ((hex_len % 2U) != 0U || (hex_len / 2U) != buffer_len) {
        return false;
    }

    for (size_t i = 0U; i < buffer_len; ++i) {
        int high = hex_char_to_value(hex[i * 2U]);
        int low = hex_char_to_value(hex[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }

        buffer[i] = (uint8_t)((high << 4) | low);
    }

    return true;
}

bool aes_self_test(void)
{
    static const uint8_t sample_text[] = "Hello_OTA_CTR_Test_1234567890";
    uint8_t expected_cipher[(sizeof(s_expected_self_test_cipher_hex) - 1U) / 2U];
    uint8_t actual_cipher[sizeof(sample_text) - 1U];
    mbedtls_aes_context aes;
    bool ok = false;

    if (!hex_to_bytes(s_expected_self_test_cipher_hex, expected_cipher, sizeof(expected_cipher))) {
        ESP_LOGE(TAG, "AES self-test expected vector parse failed");
        return false;
    }

    memcpy(actual_cipher, sample_text, sizeof(actual_cipher));
    mbedtls_aes_init(&aes);
    /*
     * The compatibility self-test must stay independent from the runtime OTA key.
     * Otherwise any production key will fail against the fixed expected vector.
     */
    if (mbedtls_aes_setkey_enc(&aes, s_self_test_key, OTA_AES_KEY_BYTES * 8U) != 0) {
        ESP_LOGE(TAG, "AES self-test setkey failed");
        mbedtls_aes_free(&aes);
        return false;
    }

    ok = ota_ctr_crypt_buffer(&aes, s_self_test_iv, 0U, actual_cipher, sizeof(actual_cipher)) &&
         memcmp(actual_cipher, expected_cipher, sizeof(actual_cipher)) == 0;
    mbedtls_aes_free(&aes);

    if (!ok) {
        ESP_LOGE(TAG, "AES-CTR self-test failed");
        return false;
    }

    ESP_LOGI(TAG, "AES-CTR self-test passed");
    return true;
}

size_t calculate_encrypted_size(size_t plain_size)
{
    return plain_size;
}

