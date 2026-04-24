#include "ota_stm32_manifest.h"

#define TAG OTA_STM32_TAG

static void latest_manifest_free(ota_latest_json_t *latest)
{
    if (latest == NULL) {
        return;
    }

    free(latest->signature);
    memset(latest, 0, sizeof(*latest));
}

bool ota_parse_semver(const char *text, ota_semver_t *version)
{
    uint32_t part_value = 0U;
    uint32_t index = 0U;
    size_t i = 0U;
    bool has_digit = false;

    if (version == NULL) {
        return false;
    }

    memset(version, 0, sizeof(*version));
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    for (i = 0U;; ++i) {
        char ch = text[i];

        if (ch >= '0' && ch <= '9') {
            uint32_t digit = (uint32_t)(ch - '0');
            if (part_value > (UINT32_MAX - digit) / 10U) {
                return false;
            }

            part_value = part_value * 10U + digit;
            has_digit = true;
            continue;
        }

        if (ch == '.' || ch == '\0') {
            if (!has_digit || index >= 3U) {
                return false;
            }

            if (index == 0U) {
                version->major = part_value;
            } else if (index == 1U) {
                version->minor = part_value;
            } else {
                version->patch = part_value;
            }

            ++index;
            if (ch == '\0') {
                break;
            }

            part_value = 0U;
            has_digit = false;
            continue;
        }

        return false;
    }

    version->valid = (index == 3U);
    return version->valid;
}

int ota_compare_semver(const ota_semver_t *lhs, const ota_semver_t *rhs)
{
    if (lhs->major != rhs->major) {
        return (lhs->major > rhs->major) ? 1 : -1;
    }

    if (lhs->minor != rhs->minor) {
        return (lhs->minor > rhs->minor) ? 1 : -1;
    }

    if (lhs->patch != rhs->patch) {
        return (lhs->patch > rhs->patch) ? 1 : -1;
    }

    return 0;
}

bool ota_ctrl_build_metadata_url(char *url_buffer, size_t url_buffer_len)
{
    int written = 0;

    if (url_buffer == NULL || url_buffer_len == 0U) {
        return false;
    }

    written = snprintf(url_buffer, url_buffer_len, "%s/%s", OTA_PACKAGE_BASE_URL, OTA_LATEST_JSON_NAME);
    return written > 0 && (size_t)written < url_buffer_len;
}

bool copy_json_string_with_context(const char *context_name,
                                   cJSON *root,
                                   const char *field_name,
                                   char *buffer,
                                   size_t buffer_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field_name);

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        ESP_LOGE(TAG, "%s missing string field: %s", context_name, field_name);
        return false;
    }

    if (snprintf(buffer, buffer_len, "%s", item->valuestring) >= (int)buffer_len) {
        ESP_LOGE(TAG, "%s field too long: %s", context_name, field_name);
        return false;
    }

    return true;
}

bool copy_json_string(cJSON *root, const char *field_name, char *buffer, size_t buffer_len)
{
    return copy_json_string_with_context("manifest.json", root, field_name, buffer, buffer_len);
}

bool normalize_crc32_text(const char *input, char output[9])
{
    size_t out_index = 0U;

    if (input == NULL) {
        return false;
    }

    for (size_t i = 0U; input[i] != '\0'; ++i) {
        char ch = input[i];

        if (ch == '0' && (input[i + 1] == 'x' || input[i + 1] == 'X') && out_index == 0U) {
            ++i;
            continue;
        }

        if (ch == ' ' || ch == '_') {
            continue;
        }

        if (!isxdigit((unsigned char)ch) || out_index >= 8U) {
            return false;
        }

        output[out_index++] = (char)toupper((unsigned char)ch);
    }

    if (out_index != 8U) {
        return false;
    }

    output[8] = '\0';
    return true;
}

bool normalize_hex_text(const char *input, char *output, size_t expected_chars)
{
    size_t out_index = 0U;

    if (input == NULL || output == NULL) {
        return false;
    }

    for (size_t i = 0U; input[i] != '\0'; ++i) {
        char ch = input[i];

        if (ch == '0' && (input[i + 1] == 'x' || input[i + 1] == 'X') && out_index == 0U) {
            ++i;
            continue;
        }

        if (ch == ' ' || ch == '_') {
            continue;
        }

        if (!isxdigit((unsigned char)ch) || out_index >= expected_chars) {
            return false;
        }

        output[out_index++] = (char)toupper((unsigned char)ch);
    }

    if (out_index != expected_chars) {
        return false;
    }

    output[expected_chars] = '\0';
    return true;
}

bool append_signing_field(char *buffer,
                          size_t buffer_len,
                          size_t *offset,
                          const char *name,
                          const char *value)
{
    int written = 0;

    if (buffer == NULL || offset == NULL || name == NULL || value == NULL || *offset >= buffer_len) {
        return false;
    }

    written = snprintf(buffer + *offset, buffer_len - *offset, "%s=%s\n", name, value);
    if (written < 0 || (size_t)written >= (buffer_len - *offset)) {
        return false;
    }

    *offset += (size_t)written;
    return true;
}

bool build_release_manifest_signing_payload(const ota_latest_json_t *latest,
                                            char *buffer,
                                            size_t buffer_len)
{
    size_t offset = 0U;
    size_t index = 0U;
    const char *force_update = latest->force_update ? "true" : "false";

    if (latest == NULL || buffer == NULL || buffer_len == 0U) {
        return false;
    }

    buffer[0] = '\0';
    if (!append_signing_field(buffer, buffer_len, &offset, "releaseId", latest->release_id) ||
        !append_signing_field(buffer, buffer_len, &offset, "manifestVersion", latest->manifest_version) ||
        !append_signing_field(buffer, buffer_len, &offset, "productId", latest->product_id) ||
        !append_signing_field(buffer, buffer_len, &offset, "hwRev", latest->hw_rev) ||
        !append_signing_field(buffer, buffer_len, &offset, "latestVersion", latest->latest_version) ||
        !append_signing_field(buffer, buffer_len, &offset, "minVersion", latest->min_version) ||
        !append_signing_field(buffer, buffer_len, &offset, "forceUpdate", force_update) ||
        !append_signing_field(buffer, buffer_len, &offset, "signatureAlgorithm", latest->signature_algorithm) ||
        !append_signing_field(buffer, buffer_len, &offset, "hashAlgorithm", latest->hash_algorithm) ||
        !append_signing_field(buffer, buffer_len, &offset, "signedAt", latest->signed_at)) {
        return false;
    }

    for (index = 0U; index < latest->package_count; ++index) {
        char key_name[32];

        if (snprintf(key_name, sizeof(key_name), "packages[%u].partition", (unsigned)index) >= (int)sizeof(key_name) ||
            !append_signing_field(buffer, buffer_len, &offset, key_name, latest->packages[index].partition) ||
            snprintf(key_name, sizeof(key_name), "packages[%u].version", (unsigned)index) >= (int)sizeof(key_name) ||
            !append_signing_field(buffer, buffer_len, &offset, key_name, latest->packages[index].version) ||
            snprintf(key_name, sizeof(key_name), "packages[%u].url", (unsigned)index) >= (int)sizeof(key_name) ||
            !append_signing_field(buffer, buffer_len, &offset, key_name, latest->packages[index].url)) {
            return false;
        }
    }

    return true;
}

bool decode_base64_signature(const char *base64_text, uint8_t **signature, size_t *signature_len)
{
    size_t decoded_len = 0U;
    int ret = 0;
    uint8_t *buffer = NULL;

    ret = mbedtls_base64_decode(NULL,
                                0,
                                &decoded_len,
                                (const unsigned char *)base64_text,
                                strlen(base64_text));
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || decoded_len == 0U) {
        ESP_LOGE(TAG, "Signature base64 length probe failed: %d", ret);
        return false;
    }

    buffer = (uint8_t *)malloc(decoded_len);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Signature allocation failed");
        return false;
    }

    ret = mbedtls_base64_decode(buffer,
                                decoded_len,
                                &decoded_len,
                                (const unsigned char *)base64_text,
                                strlen(base64_text));
    if (ret != 0) {
        ESP_LOGE(TAG, "Signature base64 decode failed: %d", ret);
        free(buffer);
        return false;
    }

    *signature = buffer;
    *signature_len = decoded_len;
    return true;
}

bool parse_latest_json(const ota_blob_t *latest_blob, ota_latest_json_t *latest)
{
    cJSON *root = NULL;
    cJSON *packages = NULL;
    size_t package_index = 0U;
    char signing_payload[OTA_SIGNING_PAYLOAD_MAX_SIZE];
    bool ok = false;

    if (latest_blob == NULL || latest == NULL || latest_blob->data == NULL) {
        return false;
    }

    memset(latest, 0, sizeof(*latest));
    root = cJSON_Parse((const char *)latest_blob->data);
    if (root == NULL) {
        ESP_LOGE(TAG, "release-manifest.v2.json parse failed");
        return false;
    }

    if (!copy_json_string_with_context("release-manifest.v2.json", root, "releaseId", latest->release_id, sizeof(latest->release_id)) ||
        !copy_json_string_with_context("release-manifest.v2.json", root, "manifestVersion", latest->manifest_version, sizeof(latest->manifest_version)) ||
        !copy_json_string_with_context("release-manifest.v2.json", root, "productId", latest->product_id, sizeof(latest->product_id)) ||
        !copy_json_string_with_context("release-manifest.v2.json", root, "hwRev", latest->hw_rev, sizeof(latest->hw_rev)) ||
        !copy_json_string_with_context("release-manifest.v2.json", root, "latestVersion", latest->latest_version, sizeof(latest->latest_version)) ||
        !copy_json_string_with_context("release-manifest.v2.json", root, "minVersion", latest->min_version, sizeof(latest->min_version)) ||
        !copy_json_string_with_context("release-manifest.v2.json", root, "signatureAlgorithm", latest->signature_algorithm, sizeof(latest->signature_algorithm)) ||
        !copy_json_string_with_context("release-manifest.v2.json", root, "hashAlgorithm", latest->hash_algorithm, sizeof(latest->hash_algorithm)) ||
        !copy_json_string_with_context("release-manifest.v2.json", root, "signedAt", latest->signed_at, sizeof(latest->signed_at))) {
        goto cleanup;
    }

    {
        cJSON *force_update = cJSON_GetObjectItemCaseSensitive(root, "forceUpdate");
        cJSON *signature_item = cJSON_GetObjectItemCaseSensitive(root, "signatureBase64");

        if (!cJSON_IsBool(force_update)) {
            ESP_LOGE(TAG, "release-manifest.v2.json missing bool field: forceUpdate");
            goto cleanup;
        }

        if (!cJSON_IsString(signature_item) || signature_item->valuestring == NULL) {
            ESP_LOGE(TAG, "release-manifest.v2.json missing string field: signatureBase64");
            goto cleanup;
        }

        latest->force_update = cJSON_IsTrue(force_update);
        if (!decode_base64_signature(signature_item->valuestring, &latest->signature, &latest->signature_len)) {
            ESP_LOGE(TAG, "release-manifest.v2.json signatureBase64 invalid");
            goto cleanup;
        }
    }

    packages = cJSON_GetObjectItemCaseSensitive(root, "packages");
    if (!cJSON_IsArray(packages)) {
        ESP_LOGE(TAG, "release-manifest.v2.json packages must be an array");
        goto cleanup;
    }

    {
        cJSON *package_item = NULL;
        cJSON_ArrayForEach(package_item, packages) {
            ota_package_info_t *package_info = NULL;

            if (package_index >= OTA_MAX_PACKAGES) {
                ESP_LOGE(TAG, "release-manifest.v2.json package count exceeds limit");
                goto cleanup;
            }

            package_info = &latest->packages[package_index];
            if (!copy_json_string_with_context("release-manifest.v2.json package",
                                               package_item,
                                               "partition",
                                               package_info->partition,
                                               sizeof(package_info->partition)) ||
                !copy_json_string_with_context("release-manifest.v2.json package",
                                               package_item,
                                               "version",
                                               package_info->version,
                                               sizeof(package_info->version)) ||
                !copy_json_string_with_context("release-manifest.v2.json package",
                                               package_item,
                                               "url",
                                               package_info->url,
                                               sizeof(package_info->url))) {
                goto cleanup;
            }

            ++package_index;
        }
    }

    if (package_index == 0U) {
        ESP_LOGE(TAG, "release-manifest.v2.json packages is empty");
        goto cleanup;
    }

    latest->package_count = package_index;
    if (!build_release_manifest_signing_payload(latest, signing_payload, sizeof(signing_payload)) ||
        !verify_signature_payload((const uint8_t *)signing_payload,
                                  strlen(signing_payload),
                                  latest->signature_algorithm,
                                  latest->hash_algorithm,
                                  latest->signature,
                                  latest->signature_len,
                                  "release-manifest.v2.json")) {
        goto cleanup;
    }

    ok = true;

cleanup:
    if (!ok) {
        latest_manifest_free(latest);
    }

    cJSON_Delete(root);
    return ok;
}

ota_upgrade_decision_t ota_decide_upgrade(const ota_upgrade_request_t *request,
                                          const ota_latest_json_t *latest)
{
    ota_semver_t current = {0};
    ota_semver_t newest = {0};
    ota_semver_t minimum = {0};
    const char *current_version = OTA_DEFAULT_DEVICE_VERSION;

    if (request == NULL || latest == NULL) {
        return OTA_DECISION_ERROR;
    }

    if (!ota_parse_semver(latest->latest_version, &newest) ||
        !ota_parse_semver(latest->min_version, &minimum)) {
        return OTA_DECISION_ERROR;
    }

    if (request->version_valid &&
        request->current_version[0] != '\0' &&
        ota_parse_semver(request->current_version, &current)) {
        current_version = request->current_version;
    }

    if (!ota_parse_semver(current_version, &current)) {
        return OTA_DECISION_ERROR;
    }

    if (ota_compare_semver(&current, &newest) >= 0) {
        return OTA_DECISION_NO_UPGRADE;
    }

    if (latest->force_update && ota_compare_semver(&current, &minimum) < 0) {
        return OTA_DECISION_FORCE_UPGRADE;
    }

    if (ota_compare_semver(&current, &newest) < 0) {
        return OTA_DECISION_UPGRADE;
    }

    return OTA_DECISION_NO_UPGRADE;
}

const ota_package_info_t *ota_find_package_for_partition(const ota_latest_json_t *latest,
                                                         uint8_t target_partition)
{
    const char *expected_partition = (target_partition == OTA_CTRL_PARTITION_APP1) ? "app1" : "app2";

    if (latest == NULL) {
        return NULL;
    }

    for (size_t i = 0U; i < latest->package_count; ++i) {
        if (strcasecmp(latest->packages[i].partition, expected_partition) == 0) {
            return &latest->packages[i];
        }
    }

    return NULL;
}

bool ota_prepare_upgrade_plan(const ota_upgrade_request_t *request,
                              ota_upgrade_plan_t *plan,
                              uint16_t *reject_reason)
{
    ota_blob_t latest_blob = {0};
    ota_latest_json_t latest = {0};
    ota_upgrade_decision_t decision = OTA_DECISION_ERROR;
    const ota_package_info_t *package_info = NULL;
    char metadata_url[OTA_PACKAGE_URL_MAX_LEN] = {0};
    const char *request_product = OTA_SUPPORTED_PRODUCT_ID;
    const char *request_hw_rev = OTA_SUPPORTED_HW_REV;
    bool ok = false;

    if (plan == NULL || reject_reason == NULL || request == NULL) {
        return false;
    }

    memset(plan, 0, sizeof(*plan));
    *reject_reason = OTA_CTRL_ERR_VERSION;

    if (!ota_ctrl_build_metadata_url(metadata_url, sizeof(metadata_url))) {
        *reject_reason = OTA_CTRL_ERR_FETCH_METADATA;
        return false;
    }

    if (!download_text_blob(metadata_url, &latest_blob, OTA_METADATA_MAX_SIZE, "release-manifest.v2.json")) {
        *reject_reason = OTA_CTRL_ERR_FETCH_METADATA;
        goto cleanup;
    }

    if (!parse_latest_json(&latest_blob, &latest)) {
        *reject_reason = OTA_CTRL_ERR_VERSION;
        goto cleanup;
    }

    if (request->product_id[0] != '\0') {
        request_product = request->product_id;
    }

    if (request->hw_rev[0] != '\0') {
        request_hw_rev = request->hw_rev;
    }

    if (strcasecmp(latest.product_id, request_product) != 0) {
        *reject_reason = OTA_CTRL_ERR_PRODUCT;
        goto cleanup;
    }

    if (strcasecmp(latest.hw_rev, request_hw_rev) != 0) {
        *reject_reason = OTA_CTRL_ERR_HW_REV;
        goto cleanup;
    }

    decision = ota_decide_upgrade(request, &latest);
    if (decision == OTA_DECISION_ERROR) {
        *reject_reason = OTA_CTRL_ERR_VERSION;
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "Version decision: current=%s latest=%s min=%s force=%u -> %d",
             (request->version_valid && request->current_version[0] != '\0')
                 ? request->current_version
                 : OTA_DEFAULT_DEVICE_VERSION,
             latest.latest_version,
             latest.min_version,
             latest.force_update ? 1U : 0U,
             (int)decision);

    if (decision == OTA_DECISION_NO_UPGRADE) {
        *reject_reason = OTA_CTRL_ERR_NO_UPDATE;
        goto cleanup;
    }

    package_info = ota_find_package_for_partition(&latest, request->target_partition);
    if (package_info == NULL) {
        *reject_reason = OTA_CTRL_ERR_NO_PACKAGE;
        goto cleanup;
    }

    {
        ota_semver_t package_version = {0};
        if (!ota_parse_semver(package_info->version, &package_version) ||
            strcasecmp(package_info->version, latest.latest_version) != 0) {
            *reject_reason = OTA_CTRL_ERR_VERSION;
            goto cleanup;
        }
    }

    plan->decision = decision;
    if (snprintf(plan->selected_version, sizeof(plan->selected_version), "%s", latest.latest_version) >=
            (int)sizeof(plan->selected_version) ||
        snprintf(plan->package_url, sizeof(plan->package_url), "%s", package_info->url) >=
            (int)sizeof(plan->package_url)) {
        *reject_reason = OTA_CTRL_ERR_VERSION;
        goto cleanup;
    }

    ok = true;

cleanup:
    blob_free(&latest_blob);
    latest_manifest_free(&latest);
    return ok;
}

bool build_iap_manifest_signing_payload(const iap_manifest_t *manifest,
                                        char *buffer,
                                        size_t buffer_len)
{
    const char *requires_encryption = manifest->requires_encryption ? "true" : "false";
    size_t offset = 0U;

    if (manifest == NULL || buffer == NULL || buffer_len == 0U) {
        return false;
    }

    buffer[0] = '\0';
    {
        char package_format[12];
        char firmware_size[24];

        if (snprintf(package_format, sizeof(package_format), "%d", manifest->package_format_version) >= (int)sizeof(package_format) ||
            snprintf(firmware_size, sizeof(firmware_size), "%u", (unsigned)manifest->firmware_size) >= (int)sizeof(firmware_size)) {
            return false;
        }

        if (!append_signing_field(buffer, buffer_len, &offset, "packageFormatVersion", package_format) ||
            !append_signing_field(buffer, buffer_len, &offset, "firmwareFileName", manifest->firmware_file_name) ||
            !append_signing_field(buffer, buffer_len, &offset, "firmwareSize", firmware_size) ||
            !append_signing_field(buffer, buffer_len, &offset, "firmwareCrc32", manifest->firmware_crc32) ||
            !append_signing_field(buffer, buffer_len, &offset, "firmwareSha256", manifest->firmware_sha256) ||
            !append_signing_field(buffer, buffer_len, &offset, "signatureAlgorithm", manifest->signature_algorithm) ||
            !append_signing_field(buffer, buffer_len, &offset, "hashAlgorithm", manifest->hash_algorithm) ||
            !append_signing_field(buffer, buffer_len, &offset, "signatureBase64", manifest->signature_base64) ||
            !append_signing_field(buffer, buffer_len, &offset, "requiresEncryption", requires_encryption) ||
            !append_signing_field(buffer, buffer_len, &offset, "encryptionAlgorithm", manifest->encryption_algorithm) ||
            !append_signing_field(buffer, buffer_len, &offset, "encryptionKeyId", manifest->encryption_key_id) ||
            !append_signing_field(buffer, buffer_len, &offset, "encryptionIvHex", manifest->encryption_iv_hex) ||
            !append_signing_field(buffer, buffer_len, &offset, "transferEncoding", manifest->transfer_encoding)) {
            return false;
        }
    }

    return true;
}

bool parse_manifest(const ota_blob_t *manifest_blob, iap_manifest_t *manifest)
{
    cJSON *root = NULL;
    bool ok = false;
    cJSON *package_format_item = NULL;
    cJSON *firmware_size_item = NULL;
    cJSON *requires_encryption_item = NULL;
    cJSON *signature_base64_item = NULL;
    cJSON *manifest_signature_base64_item = NULL;

    if (manifest_blob == NULL || manifest == NULL || manifest_blob->data == NULL) {
        return false;
    }

    memset(manifest, 0, sizeof(*manifest));
    root = cJSON_Parse((const char *)manifest_blob->data);
    if (root == NULL) {
        ESP_LOGE(TAG, "manifest.json parse failed");
        return false;
    }

    package_format_item = cJSON_GetObjectItemCaseSensitive(root, "packageFormatVersion");
    firmware_size_item = cJSON_GetObjectItemCaseSensitive(root, "firmwareSize");
    requires_encryption_item = cJSON_GetObjectItemCaseSensitive(root, "requiresEncryption");
    signature_base64_item = cJSON_GetObjectItemCaseSensitive(root, "signatureBase64");
    manifest_signature_base64_item = cJSON_GetObjectItemCaseSensitive(root, "manifestSignatureBase64");

    if (!cJSON_IsNumber(package_format_item) ||
        (package_format_item->valueint != 1 &&
         package_format_item->valueint != 2 &&
         package_format_item->valueint != 3)) {
        ESP_LOGE(TAG, "Unsupported packageFormatVersion");
        goto cleanup;
    }

    if (!cJSON_IsNumber(firmware_size_item) || firmware_size_item->valuedouble <= 0) {
        ESP_LOGE(TAG, "manifest.json invalid firmwareSize");
        goto cleanup;
    }

    if (!cJSON_IsBool(requires_encryption_item)) {
        ESP_LOGE(TAG, "manifest.json missing requiresEncryption");
        goto cleanup;
    }

    if (!cJSON_IsString(signature_base64_item) || signature_base64_item->valuestring == NULL) {
        ESP_LOGE(TAG, "manifest.json missing signatureBase64");
        goto cleanup;
    }

    manifest->package_format_version = package_format_item->valueint;
    manifest->firmware_size = (size_t)firmware_size_item->valuedouble;
    manifest->requires_encryption = cJSON_IsTrue(requires_encryption_item);

    if (!copy_json_string(root, "firmwareFileName", manifest->firmware_file_name, sizeof(manifest->firmware_file_name)) ||
        !copy_json_string(root, "firmwareCrc32", manifest->firmware_crc32, sizeof(manifest->firmware_crc32)) ||
        !copy_json_string(root, "signatureAlgorithm", manifest->signature_algorithm, sizeof(manifest->signature_algorithm)) ||
        !copy_json_string(root, "hashAlgorithm", manifest->hash_algorithm, sizeof(manifest->hash_algorithm)) ||
        !copy_json_string(root, "encryptionAlgorithm", manifest->encryption_algorithm, sizeof(manifest->encryption_algorithm))) {
        goto cleanup;
    }

    if (!normalize_crc32_text(manifest->firmware_crc32, manifest->firmware_crc32)) {
        ESP_LOGE(TAG, "manifest.json invalid firmwareCrc32");
        goto cleanup;
    }

    if (snprintf(manifest->signature_base64,
                 sizeof(manifest->signature_base64),
                 "%s",
                 signature_base64_item->valuestring) >= (int)sizeof(manifest->signature_base64)) {
        ESP_LOGE(TAG, "manifest.json signatureBase64 too long");
        goto cleanup;
    }

    if (!decode_base64_signature(signature_base64_item->valuestring,
                                 &manifest->signature,
                                 &manifest->signature_len)) {
        goto cleanup;
    }

    if (copy_json_string(root, "firmwareSha256", manifest->firmware_sha256, sizeof(manifest->firmware_sha256))) {
        if (!normalize_hex_text(manifest->firmware_sha256, manifest->firmware_sha256, OTA_SHA256_HEX_LEN)) {
            ESP_LOGE(TAG, "manifest.json invalid firmwareSha256");
            goto cleanup;
        }
    }

    if (copy_json_string(root, "encryptionIvHex", manifest->encryption_iv_hex, sizeof(manifest->encryption_iv_hex))) {
        if (!normalize_hex_text(manifest->encryption_iv_hex, manifest->encryption_iv_hex, OTA_IV_HEX_LEN)) {
            ESP_LOGE(TAG, "manifest.json invalid encryptionIvHex");
            goto cleanup;
        }
    } else if (manifest->package_format_version <= 1) {
        snprintf(manifest->encryption_iv_hex, sizeof(manifest->encryption_iv_hex), "%s", "00000000000000000000000000000000");
    } else {
        ESP_LOGE(TAG, "manifest.json missing encryptionIvHex");
        goto cleanup;
    }

    if (!copy_json_string(root, "transferEncoding", manifest->transfer_encoding, sizeof(manifest->transfer_encoding))) {
        if (manifest->package_format_version <= 1) {
            snprintf(manifest->transfer_encoding, sizeof(manifest->transfer_encoding), "%s", "RAW_CTR");
        } else {
            ESP_LOGE(TAG, "manifest.json missing transferEncoding");
            goto cleanup;
        }
    }

    if (!copy_json_string(root, "encryptionKeyId", manifest->encryption_key_id, sizeof(manifest->encryption_key_id))) {
        if (manifest->package_format_version <= 1) {
            snprintf(manifest->encryption_key_id, sizeof(manifest->encryption_key_id), "%s", "legacy-fixed-aes256");
        } else {
            manifest->encryption_key_id[0] = '\0';
        }
    }

    if (manifest->package_format_version >= 2) {
        if (!cJSON_IsString(manifest_signature_base64_item) || manifest_signature_base64_item->valuestring == NULL) {
            ESP_LOGE(TAG, "manifest.json missing manifestSignatureBase64");
            goto cleanup;
        }

        if (!decode_base64_signature(manifest_signature_base64_item->valuestring,
                                     &manifest->manifest_signature,
                                     &manifest->manifest_signature_len)) {
            goto cleanup;
        }
    }

    ok = true;

cleanup:
    cJSON_Delete(root);
    return ok;
}
