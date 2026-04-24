#include "ota_stm32_verify.h"
#include "../../../../protocol/ota_rsa_public_key_pem.h"

#define TAG OTA_STM32_TAG

static bool ota_is_development_key_id(const char *key_id)
{
    if (key_id == NULL || key_id[0] == '\0') {
        return false;
    }

    return (strcasecmp(key_id, "dev-fixed-aes256") == 0) ||
           (strcasecmp(key_id, "legacy-fixed-aes256") == 0);
}

static void ota_log_manifest_key_profile(const iap_manifest_t *manifest)
{
    const char *device_key_id = ota_aes_key_id_text();

    if (manifest == NULL) {
        return;
    }

    if (manifest->encryption_key_id[0] == '\0') {
        ESP_LOGW(TAG,
                 "Package encryptionKeyId is empty; device keyId=%s",
                 device_key_id);
        return;
    }

    if (ota_is_development_key_id(manifest->encryption_key_id)) {
        ESP_LOGW(TAG,
                 "Package uses development encryptionKeyId=%s; device keyId=%s",
                 manifest->encryption_key_id,
                 device_key_id);
        return;
    }

    if (device_key_id != NULL &&
        device_key_id[0] != '\0' &&
        strcasecmp(manifest->encryption_key_id, device_key_id) != 0) {
        ESP_LOGW(TAG,
                 "Package encryptionKeyId=%s differs from device keyId=%s",
                 manifest->encryption_key_id,
                 device_key_id);
        return;
    }

    ESP_LOGI(TAG,
             "Package encryptionKeyId=%s",
             manifest->encryption_key_id);
}

bool ota_compute_session_fingerprint(const ota_iap_context_t *context,
                                     uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN])
{
    const mbedtls_md_info_t *md_info = NULL;
    int ret = 0;

    if (context == NULL || session_fingerprint == NULL) {
        return false;
    }

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        ESP_LOGE(TAG, "SHA256 provider unavailable for session fingerprint");
        return false;
    }

    ret = mbedtls_md(md_info,
                     (const uint8_t *)&context->image_header,
                     OTA_IMAGE_HEADER_TOTAL_SIZE,
                     session_fingerprint);
    if (ret != 0) {
        ESP_LOGE(TAG, "Session fingerprint SHA256 failed: %d", ret);
        return false;
    }

    return true;
}

void ota_validation_result_set(ota_validation_result_t *result,
                               uint8_t stage,
                               uint16_t error_code)
{
    if (result == NULL) {
        return;
    }

    result->stage = stage;
    result->error_code = error_code;
}

bool verify_signature_payload(const uint8_t *payload,
                              size_t payload_len,
                              const char *signature_algorithm,
                              const char *hash_algorithm,
                              const uint8_t *signature,
                              size_t signature_len,
                              const char *context_label)
{
    mbedtls_pk_context public_key;
    int ret = 0;
    unsigned char hash[32];
    const mbedtls_md_info_t *md_info = NULL;

    if (payload == NULL || signature == NULL || context_label == NULL) {
        return false;
    }

    if (strcasecmp(signature_algorithm, "RSA") != 0 || strcasecmp(hash_algorithm, "SHA256") != 0) {
        ESP_LOGE(TAG,
                 "%s uses unsupported signature profile: %s-%s",
                 context_label,
                 signature_algorithm != NULL ? signature_algorithm : "(null)",
                 hash_algorithm != NULL ? hash_algorithm : "(null)");
        return false;
    }

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        ESP_LOGE(TAG, "%s SHA256 provider unavailable", context_label);
        return false;
    }

    mbedtls_pk_init(&public_key);
    ret = mbedtls_pk_parse_public_key(&public_key,
                                      (const unsigned char *)s_embedded_public_key_pem,
                                      strlen(s_embedded_public_key_pem) + 1U);
    if (ret != 0) {
        ESP_LOGE(TAG, "%s public key parse failed: %d", context_label, ret);
        mbedtls_pk_free(&public_key);
        return false;
    }

    ret = mbedtls_md(md_info, payload, payload_len, hash);
    if (ret != 0) {
        ESP_LOGE(TAG, "%s SHA256 failed: %d", context_label, ret);
        mbedtls_pk_free(&public_key);
        return false;
    }

    ret = mbedtls_pk_verify(&public_key,
                            MBEDTLS_MD_SHA256,
                            hash,
                            sizeof(hash),
                            signature,
                            signature_len);
    mbedtls_pk_free(&public_key);
    if (ret != 0) {
        ESP_LOGE(TAG, "%s signature verify failed: %d", context_label, ret);
        return false;
    }

    return true;
}

void crc32_to_hex(uint32_t crc32, char buffer[9])
{
    snprintf(buffer, 9, "%08" PRIX32, crc32);
}

void bytes_to_hex_string(const uint8_t *bytes,
                         size_t byte_count,
                         char *buffer,
                         size_t buffer_len)
{
    size_t index = 0U;

    if (buffer_len == 0U) {
        return;
    }

    for (index = 0U; index < byte_count && ((index * 2U) + 1U) < buffer_len; ++index) {
        snprintf(buffer + (index * 2U), buffer_len - (index * 2U), "%02X", bytes[index]);
    }

    buffer[(byte_count * 2U) < buffer_len ? (byte_count * 2U) : (buffer_len - 1U)] = '\0';
}

bool validate_firmware_stream(const ota_iap_context_t *context,
                              ota_validation_result_t *result,
                              uint8_t firmware_sha256[32])
{
    zip_entry_stream_t *firmware_stream = NULL;
    mbedtls_pk_context public_key;
    mbedtls_md_context_t md_context;
    const mbedtls_md_info_t *md_info = NULL;
    uint8_t *buffer = NULL;
    uint8_t hash[32];
    uint32_t crc32 = 0U;
    size_t total_read = 0U;
    bool ok = false;
    char actual_crc32[9];
    char actual_sha256[OTA_SHA256_HEX_LEN + 1U];
    int ret = 0;

    mbedtls_pk_init(&public_key);
    mbedtls_md_init(&md_context);

    firmware_stream = (zip_entry_stream_t *)calloc(1, sizeof(*firmware_stream));
    buffer = (uint8_t *)malloc(OTA_STREAM_BUFFER_SIZE);
    if (firmware_stream == NULL || buffer == NULL) {
        ESP_LOGE(TAG, "Validation workspace allocation failed");
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_ERR_PROTOCOL);
        goto cleanup;
    }

    if (!zip_entry_stream_init(firmware_stream,
                               &context->package_blob,
                               "firmware.bin",
                               &context->firmware_entry)) {
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_ERR_PROTOCOL);
        goto cleanup;
    }

    ret = mbedtls_pk_parse_public_key(&public_key,
                                      (const unsigned char *)s_embedded_public_key_pem,
                                      strlen(s_embedded_public_key_pem) + 1U);
    if (ret != 0) {
        ESP_LOGE(TAG, "Public key parse failed: %d", ret);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        goto cleanup;
    }

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        ESP_LOGE(TAG, "SHA256 digest provider not available");
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        goto cleanup;
    }

    ret = mbedtls_md_setup(&md_context, md_info, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA256 setup failed: %d", ret);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        goto cleanup;
    }

    ret = mbedtls_md_starts(&md_context);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA256 start failed: %d", ret);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        goto cleanup;
    }

    while (true) {
        size_t chunk_read = 0U;

        if (!zip_entry_stream_read(firmware_stream, buffer, OTA_STREAM_BUFFER_SIZE, &chunk_read)) {
            ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_ERR_PROTOCOL);
            goto cleanup;
        }

        if (chunk_read == 0U) {
            break;
        }

        crc32 = esp_rom_crc32_le(crc32, buffer, chunk_read);
        ret = mbedtls_md_update(&md_context, buffer, chunk_read);
        if (ret != 0) {
            ESP_LOGE(TAG, "SHA256 update failed: %d", ret);
            ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
            goto cleanup;
        }

        total_read += chunk_read;
    }

    if (total_read != context->manifest.firmware_size ||
        firmware_stream->output_total != context->manifest.firmware_size) {
        ESP_LOGE(TAG,
                 "Firmware stream size mismatch. Manifest=%u, Actual=%u",
                 (unsigned)context->manifest.firmware_size,
                 (unsigned)total_read);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_ERR_CRC32);
        goto cleanup;
    }

    crc32_to_hex(crc32, actual_crc32);
    if (strcasecmp(actual_crc32, context->manifest.firmware_crc32) != 0) {
        ESP_LOGE(TAG,
                 "CRC32 mismatch. Expected=%s, Actual=%s",
                 context->manifest.firmware_crc32,
                 actual_crc32);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_ERR_CRC32);
        goto cleanup;
    }

    ret = mbedtls_md_finish(&md_context, hash);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA256 finish failed: %d", ret);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        goto cleanup;
    }

    bytes_to_hex_string(hash, sizeof(hash), actual_sha256, sizeof(actual_sha256));
    if (context->manifest.firmware_sha256[0] != '\0' &&
        strcasecmp(actual_sha256, context->manifest.firmware_sha256) != 0) {
        ESP_LOGE(TAG,
                 "SHA256 mismatch. Expected=%s, Actual=%s",
                 context->manifest.firmware_sha256,
                 actual_sha256);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        goto cleanup;
    }

    ret = mbedtls_pk_verify(&public_key,
                            MBEDTLS_MD_SHA256,
                            hash,
                            0,
                            context->manifest.signature,
                            context->manifest.signature_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "RSA signature verify failed: %d", ret);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        goto cleanup;
    }

    if (firmware_sha256 != NULL) {
        memcpy(firmware_sha256, hash, 32U);
    }

    ESP_LOGI(TAG, "CRC32 verified: %s", actual_crc32);
    ESP_LOGI(TAG, "SHA256 verified: %s", actual_sha256);
    ESP_LOGI(TAG, "Signature verified with embedded RSA public key");
    ok = true;

cleanup:
    if (firmware_stream != NULL) {
        zip_entry_stream_free(firmware_stream);
        free(firmware_stream);
    }
    free(buffer);
    mbedtls_md_free(&md_context);
    mbedtls_pk_free(&public_key);
    return ok;
}

bool validate_image_header(const ota_iap_context_t *context,
                           uint8_t expected_target_partition,
                           const char *expected_version,
                           ota_validation_result_t *result,
                           const uint8_t firmware_sha256[32])
{
    uint8_t manifest_iv[OTA_AES_BLOCK_SIZE];
    const OtaImageHeaderBinary *header = NULL;

    if (context == NULL || expected_version == NULL || result == NULL || firmware_sha256 == NULL) {
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_PROTOCOL);
        return false;
    }

    header = &context->image_header;
    if (header->envelope.magic != OTA_IMAGE_HEADER_MAGIC ||
        header->envelope.header_version != OTA_IMAGE_HEADER_VERSION ||
        header->envelope.header_size != OTA_IMAGE_HEADER_TOTAL_SIZE ||
        header->envelope.signature_len != OTA_IMAGE_SIGNATURE_LEN) {
        ESP_LOGE(TAG, "image-header.bin envelope invalid");
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        return false;
    }

    if (header->payload.format_version != OTA_IMAGE_FORMAT_VERSION ||
        header->payload.signature_algorithm != OTA_IMAGE_SIG_ALG_RSA2048_SHA256_PKCS1V15) {
        ESP_LOGE(TAG,
                 "image-header.bin payload profile unsupported: format=%u sig_alg=%u",
                 (unsigned)header->payload.format_version,
                 (unsigned)header->payload.signature_algorithm);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        return false;
    }

    if (header->payload.target_slot != expected_target_partition) {
        ESP_LOGE(TAG,
                 "image-header target slot mismatch. Expected=APP%u Actual=APP%u",
                 (unsigned)expected_target_partition + 1U,
                 (unsigned)header->payload.target_slot + 1U);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_VERSION);
        return false;
    }

    if (strcmp(header->payload.firmware_version, expected_version) != 0) {
        ESP_LOGE(TAG,
                 "image-header version mismatch. Expected=%s Actual=%s",
                 expected_version,
                 header->payload.firmware_version);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_VERSION);
        return false;
    }

    if (header->payload.firmware_size != context->manifest.firmware_size ||
        header->payload.firmware_size != context->firmware_entry.uncompressed_size) {
        ESP_LOGE(TAG,
                 "image-header firmware size mismatch. Header=%u Manifest=%u Package=%u",
                 (unsigned)header->payload.firmware_size,
                 (unsigned)context->manifest.firmware_size,
                 (unsigned)context->firmware_entry.uncompressed_size);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_ERR_CRC32);
        return false;
    }

    if (!hex_to_bytes(context->manifest.encryption_iv_hex, manifest_iv, sizeof(manifest_iv))) {
        ESP_LOGE(TAG, "manifest encryptionIvHex invalid during image-header check");
        ota_validation_result_set(result, OTA_CTRL_STAGE_AES_PREPARE, OTA_CTRL_ERR_AES);
        return false;
    }

    if (memcmp(header->payload.iv, manifest_iv, sizeof(manifest_iv)) != 0) {
        ESP_LOGE(TAG, "image-header IV mismatch");
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        return false;
    }

    if (memcmp(header->payload.firmware_sha256, firmware_sha256, 32U) != 0) {
        ESP_LOGE(TAG, "image-header SHA256 mismatch");
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        return false;
    }

    if (!verify_signature_payload((const uint8_t *)&header->payload,
                                  OTA_IMAGE_HEADER_PAYLOAD_SIZE,
                                  "RSA",
                                  "SHA256",
                                  header->signature,
                                  header->envelope.signature_len,
                                  "image-header.bin")) {
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        return false;
    }

    ESP_LOGI(TAG,
             "image-header verified: target=APP%u version=%s size=%u",
             (unsigned)header->payload.target_slot + 1U,
             header->payload.firmware_version,
             (unsigned)header->payload.firmware_size);
    return true;
}

bool validate_iap_package(const ota_iap_context_t *context,
                          const ota_upgrade_request_t *request,
                          const ota_upgrade_plan_t *plan,
                          ota_validation_result_t *result)
{
    uint8_t firmware_sha256[32] = {0};

    if (context == NULL || request == NULL || plan == NULL || result == NULL) {
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_PROTOCOL);
        return false;
    }

    if (!context->manifest.requires_encryption) {
        ESP_LOGE(TAG, "Package does not require encryption, refusing to send to STM32");
        ota_validation_result_set(result, OTA_CTRL_STAGE_AES_PREPARE, OTA_CTRL_ERR_AES);
        return false;
    }

    if (strcasecmp(context->manifest.encryption_algorithm, "AES-256-CTR") != 0) {
        ESP_LOGE(TAG, "Unsupported encryption algorithm: %s", context->manifest.encryption_algorithm);
        ota_validation_result_set(result, OTA_CTRL_STAGE_AES_PREPARE, OTA_CTRL_ERR_AES);
        return false;
    }

    if (strcasecmp(context->manifest.signature_algorithm, "RSA") != 0) {
        ESP_LOGE(TAG, "Unsupported signature algorithm: %s", context->manifest.signature_algorithm);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        return false;
    }

    if (strcasecmp(context->manifest.hash_algorithm, "SHA256") != 0) {
        ESP_LOGE(TAG, "Unsupported hash algorithm: %s", context->manifest.hash_algorithm);
        ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
        return false;
    }

    if (strcasecmp(context->manifest.transfer_encoding, "RAW_CTR") != 0) {
        ESP_LOGE(TAG, "Unsupported transfer encoding: %s", context->manifest.transfer_encoding);
        ota_validation_result_set(result, OTA_CTRL_STAGE_AES_PREPARE, OTA_CTRL_ERR_AES);
        return false;
    }

    ota_log_manifest_key_profile(&context->manifest);

    if (context->manifest.package_format_version >= 2) {
        char signing_payload[OTA_SIGNING_PAYLOAD_MAX_SIZE];
        if (context->manifest.manifest_signature == NULL ||
            context->manifest.manifest_signature_len == 0U ||
            !build_iap_manifest_signing_payload(&context->manifest, signing_payload, sizeof(signing_payload)) ||
            !verify_signature_payload((const uint8_t *)signing_payload,
                                      strlen(signing_payload),
                                      context->manifest.signature_algorithm,
                                      context->manifest.hash_algorithm,
                                      context->manifest.manifest_signature,
                                      context->manifest.manifest_signature_len,
                                      "manifest.json")) {
            ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_SIG, OTA_CTRL_ERR_SIGNATURE);
            return false;
        }
    }

    ota_validation_result_set(result, OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_ERR_PROTOCOL);
    if (!validate_firmware_stream(context, result, firmware_sha256)) {
        return false;
    }

    return validate_image_header(context,
                                 request->target_partition,
                                 plan->selected_version,
                                 result,
                                 firmware_sha256);
}
