#include "ota_stm32_package.h"

#define TAG OTA_STM32_TAG
#define OTA_CACHE_ERASE_CHUNK_SIZE (64U * 1024U)

static size_t ota_align_up_size(size_t value, size_t align)
{
    if (align == 0U) {
        return value;
    }

    return ((value + align - 1U) / align) * align;
}

static bool ota_partition_erase_chunked(const esp_partition_t *partition,
                                        size_t offset,
                                        size_t total_size)
{
    size_t erased_size = 0U;

    if (partition == NULL) {
        return false;
    }

    while (erased_size < total_size) {
        size_t chunk_size = total_size - erased_size;
        esp_err_t err = ESP_OK;

        if (chunk_size > OTA_CACHE_ERASE_CHUNK_SIZE) {
            chunk_size = OTA_CACHE_ERASE_CHUNK_SIZE;
        }

        err = esp_partition_erase_range(partition, offset + erased_size, chunk_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "Partition erase failed at offset=%u size=%u: %s",
                     (unsigned)(offset + erased_size),
                     (unsigned)chunk_size,
                     esp_err_to_name(err));
            return false;
        }

        erased_size += chunk_size;

        /* SPI Flash 擦除会占用较长时间。
         * 分块后主动让出一次调度，避免把 IDLE0 长时间饿死触发 task_wdt。 */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return true;
}

void blob_free(ota_blob_t *blob)
{
    if (blob == NULL) {
        return;
    }

    if (blob->is_mapped) {
        if (blob->map_handle != 0) {
            esp_partition_munmap(blob->map_handle);
        }
    } else {
        free(blob->data);
    }

    memset(blob, 0, sizeof(*blob));
}

bool blob_attach_mmap(ota_blob_t *blob,
                      const uint8_t *mapped_data,
                      size_t data_size,
                      esp_partition_mmap_handle_t map_handle)
{
    blob_free(blob);
    blob->data = (uint8_t *)mapped_data;
    blob->size = data_size;
    blob->capacity = 0U;
    blob->is_mapped = true;
    blob->map_handle = map_handle;
    return true;
}

bool blob_reserve(ota_blob_t *blob, size_t required_capacity)
{
    uint8_t *new_buffer = NULL;

    if (blob->is_mapped) {
        ESP_LOGE(TAG, "Cannot resize a memory-mapped blob");
        return false;
    }

    if (required_capacity <= blob->capacity) {
        return true;
    }

    new_buffer = (uint8_t *)realloc(blob->data, required_capacity);
    if (new_buffer == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed, requested=%u bytes", (unsigned)required_capacity);
        return false;
    }

    blob->data = new_buffer;
    blob->capacity = required_capacity;
    return true;
}

bool blob_resize(ota_blob_t *blob, size_t size, bool append_terminator)
{
    size_t required_capacity = size + (append_terminator ? 1U : 0U);

    if (!blob_reserve(blob, required_capacity)) {
        return false;
    }

    if (append_terminator) {
        blob->data[size] = '\0';
        blob->size = size + 1U;
    } else {
        blob->size = size;
    }

    return true;
}

void manifest_free(iap_manifest_t *manifest)
{
    if (manifest == NULL) {
        return;
    }

    free(manifest->signature);
    free(manifest->manifest_signature);
    memset(manifest, 0, sizeof(*manifest));
}

void ota_context_free(ota_iap_context_t *context)
{
    if (context == NULL) {
        return;
    }

    blob_free(&context->package_blob);
    blob_free(&context->manifest_blob);
    blob_free(&context->image_header_blob);
    manifest_free(&context->manifest);
    memset(&context->image_header, 0, sizeof(context->image_header));
}

bool download_text_blob(const char *url,
                        ota_blob_t *blob,
                        size_t max_size,
                        const char *label)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    bool ok = false;
    int64_t content_length = -1;
    size_t write_offset = 0U;
    uint8_t read_buffer[OTA_HTTP_READ_BUFFER_SIZE];

    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed for %s", label);
        return false;
    }

    blob_free(blob);

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed for %s", label);
        goto cleanup;
    }

    content_length = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        ESP_LOGE(TAG, "%s HTTP status=%d", label, esp_http_client_get_status_code(client));
        goto cleanup;
    }

    if (content_length > 0 && (size_t)content_length > max_size) {
        ESP_LOGE(TAG,
                 "%s too large. Size=%" PRId64 ", Limit=%u",
                 label,
                 content_length,
                 (unsigned)max_size);
        goto cleanup;
    }

    while (true) {
        int read_len = esp_http_client_read(client, (char *)read_buffer, sizeof(read_buffer));
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read failed for %s", label);
            goto cleanup;
        }

        if (read_len == 0) {
            break;
        }

        if (write_offset + (size_t)read_len > max_size) {
            ESP_LOGE(TAG, "%s exceeds max buffer size", label);
            goto cleanup;
        }

        if (!blob_reserve(blob, write_offset + (size_t)read_len + 1U)) {
            goto cleanup;
        }

        memcpy(blob->data + write_offset, read_buffer, (size_t)read_len);
        write_offset += (size_t)read_len;
    }

    if (write_offset == 0U) {
        ESP_LOGE(TAG, "%s is empty", label);
        goto cleanup;
    }

    blob->data[write_offset] = '\0';
    blob->size = write_offset + 1U;
    ok = true;
    ESP_LOGI(TAG, "Downloaded %s: %u bytes", label, (unsigned)write_offset);

cleanup:
    if (!ok) {
        blob_free(blob);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

bool attach_cached_iap_package(size_t package_size, ota_blob_t *package_blob)
{
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_ANY,
                                 OTA_CACHE_PARTITION_LABEL);
    const void *mapped_ptr = NULL;
    esp_partition_mmap_handle_t map_handle = 0;
    esp_err_t err = ESP_FAIL;

    if (package_blob == NULL || package_size == 0U) {
        ESP_LOGE(TAG, "Invalid cached package mapping request");
        return false;
    }

    if (partition == NULL) {
        ESP_LOGE(TAG, "Partition not found: %s", OTA_CACHE_PARTITION_LABEL);
        return false;
    }

    if (package_size > partition->size) {
        ESP_LOGE(TAG,
                 "Cached package size exceeds partition. Size=%u, Partition=%u",
                 (unsigned)package_size,
                 (unsigned)partition->size);
        return false;
    }

    err = esp_partition_mmap(partition,
                             0,
                             package_size,
                             ESP_PARTITION_MMAP_DATA,
                             &mapped_ptr,
                             &map_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cached package mmap failed: %s", esp_err_to_name(err));
        return false;
    }

    blob_attach_mmap(package_blob, (const uint8_t *)mapped_ptr, package_size, map_handle);
    ESP_LOGI(TAG, "Attached cached .iap package from partition %s: %u bytes",
             partition->label,
             (unsigned)package_size);
    return true;
}

static bool find_zip_eocd(const ota_blob_t *package_blob, size_t *eocd_offset)
{
    size_t min_offset = 0U;
    size_t offset = 0U;

    if (package_blob == NULL || eocd_offset == NULL || package_blob->size < 22U) {
        ESP_LOGE(TAG, ".iap package too small for ZIP EOCD");
        return false;
    }

    if (package_blob->size > (size_t)UINT16_MAX + 22U) {
        min_offset = package_blob->size - ((size_t)UINT16_MAX + 22U);
    }

    offset = package_blob->size - 22U;
    while (true) {
        if (read_le32(package_blob->data + offset) == ZIP_EOCD_SIGNATURE) {
            uint16_t comment_len = read_le16(package_blob->data + offset + 20U);
            if (offset + 22U + comment_len == package_blob->size) {
                *eocd_offset = offset;
                return true;
            }
        }

        if (offset == min_offset) {
            break;
        }

        --offset;
    }

    ESP_LOGE(TAG, "ZIP EOCD not found");
    return false;
}

static bool zip_entry_name_equals(const uint8_t *name_data, uint16_t name_len, const char *expected_name)
{
    size_t expected_len = strlen(expected_name);

    return expected_len == (size_t)name_len &&
           memcmp(name_data, expected_name, expected_len) == 0;
}

static bool find_zip_entry(const ota_blob_t *package_blob,
                           const char *entry_name,
                           zip_entry_info_t *entry_info)
{
    size_t eocd_offset = 0U;
    const uint8_t *eocd = NULL;
    uint16_t total_entries = 0U;
    uint32_t central_dir_size = 0U;
    uint32_t central_dir_offset = 0U;
    size_t cursor = 0U;
    size_t central_dir_end = 0U;

    if (!find_zip_eocd(package_blob, &eocd_offset)) {
        return false;
    }

    eocd = package_blob->data + eocd_offset;
    total_entries = read_le16(eocd + 10U);
    central_dir_size = read_le32(eocd + 12U);
    central_dir_offset = read_le32(eocd + 16U);

    if ((size_t)central_dir_offset + (size_t)central_dir_size > package_blob->size) {
        ESP_LOGE(TAG, "ZIP central directory exceeds package bounds");
        return false;
    }

    cursor = (size_t)central_dir_offset;
    central_dir_end = cursor + (size_t)central_dir_size;

    for (uint16_t index = 0U; index < total_entries; ++index) {
        const uint8_t *header = NULL;
        uint16_t file_name_len = 0U;
        uint16_t extra_len = 0U;
        uint16_t comment_len = 0U;
        size_t entry_len = 0U;
        const uint8_t *file_name = NULL;

        if (cursor + 46U > central_dir_end) {
            ESP_LOGE(TAG, "ZIP central directory entry truncated");
            return false;
        }

        header = package_blob->data + cursor;
        if (read_le32(header) != ZIP_CDIR_SIGNATURE) {
            ESP_LOGE(TAG, "Invalid ZIP central directory signature");
            return false;
        }

        file_name_len = read_le16(header + 28U);
        extra_len = read_le16(header + 30U);
        comment_len = read_le16(header + 32U);
        entry_len = 46U + (size_t)file_name_len + (size_t)extra_len + (size_t)comment_len;

        if (cursor + entry_len > central_dir_end) {
            ESP_LOGE(TAG, "ZIP central directory entry out of range");
            return false;
        }

        file_name = header + 46U;
        if (zip_entry_name_equals(file_name, file_name_len, entry_name)) {
            entry_info->flags = read_le16(header + 8U);
            entry_info->compression_method = read_le16(header + 10U);
            entry_info->crc32 = read_le32(header + 16U);
            entry_info->compressed_size = read_le32(header + 20U);
            entry_info->uncompressed_size = read_le32(header + 24U);
            entry_info->local_header_offset = read_le32(header + 42U);
            return true;
        }

        cursor += entry_len;
    }

    ESP_LOGE(TAG, "Required entry not found in .iap: %s", entry_name);
    return false;
}

static bool zip_locate_entry_data(const ota_blob_t *package_blob,
                                  const char *entry_name,
                                  const zip_entry_info_t *entry_info,
                                  const uint8_t **data_ptr)
{
    size_t header_offset = (size_t)entry_info->local_header_offset;
    const uint8_t *local_header = NULL;
    uint16_t local_flags = 0U;
    uint16_t local_method = 0U;
    uint16_t file_name_len = 0U;
    uint16_t extra_len = 0U;
    size_t data_offset = 0U;

    if (header_offset + 30U > package_blob->size) {
        ESP_LOGE(TAG, "ZIP local header out of range: %s", entry_name);
        return false;
    }

    local_header = package_blob->data + header_offset;
    if (read_le32(local_header) != ZIP_LOCAL_SIGNATURE) {
        ESP_LOGE(TAG, "Invalid ZIP local header signature: %s", entry_name);
        return false;
    }

    local_flags = read_le16(local_header + 6U);
    local_method = read_le16(local_header + 8U);
    file_name_len = read_le16(local_header + 26U);
    extra_len = read_le16(local_header + 28U);
    data_offset = header_offset + 30U + (size_t)file_name_len + (size_t)extra_len;

    if (local_method != entry_info->compression_method || local_flags != entry_info->flags) {
        ESP_LOGE(TAG, "ZIP header mismatch for entry: %s", entry_name);
        return false;
    }

    if (data_offset + (size_t)entry_info->compressed_size > package_blob->size) {
        ESP_LOGE(TAG, "ZIP entry data out of range: %s", entry_name);
        return false;
    }

    *data_ptr = package_blob->data + data_offset;
    return true;
}

static bool zip_extract_entry(const ota_blob_t *package_blob,
                              const char *entry_name,
                              const zip_entry_info_t *entry_info,
                              ota_blob_t *output_blob,
                              bool append_terminator)
{
    const uint8_t *compressed_data = NULL;
    size_t output_size = (size_t)entry_info->uncompressed_size;

    if ((entry_info->flags & 0x0001U) != 0U) {
        ESP_LOGE(TAG, "Encrypted ZIP entry is not supported: %s", entry_name);
        return false;
    }

    if (entry_info->compression_method != 0U && entry_info->compression_method != MZ_DEFLATED) {
        ESP_LOGE(TAG,
                 "Unsupported ZIP compression method %u for %s",
                 (unsigned)entry_info->compression_method,
                 entry_name);
        return false;
    }

    if (!zip_locate_entry_data(package_blob, entry_name, entry_info, &compressed_data)) {
        return false;
    }

    if (!blob_resize(output_blob, output_size, append_terminator)) {
        return false;
    }

    if (entry_info->compression_method == 0U) {
        if (entry_info->compressed_size != entry_info->uncompressed_size) {
            ESP_LOGE(TAG, "Stored ZIP entry size mismatch: %s", entry_name);
            return false;
        }

        memcpy(output_blob->data, compressed_data, output_size);
    } else {
        size_t actual_size = tinfl_decompress_mem_to_mem(output_blob->data,
                                                         output_size,
                                                         compressed_data,
                                                         (size_t)entry_info->compressed_size,
                                                         TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (actual_size == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || actual_size != output_size) {
            ESP_LOGE(TAG, "Deflate extract failed for %s", entry_name);
            return false;
        }
    }

    if (esp_rom_crc32_le(0, output_blob->data, output_size) != entry_info->crc32) {
        ESP_LOGE(TAG, "ZIP entry CRC32 mismatch for %s", entry_name);
        return false;
    }

    if (append_terminator) {
        output_blob->data[output_size] = '\0';
        output_blob->size = output_size + 1U;
    } else {
        output_blob->size = output_size;
    }

    return true;
}

bool download_iap_package(const char *url, ota_blob_t *package_blob)
{
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_ANY,
                                 OTA_CACHE_PARTITION_LABEL);
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = NULL;
    bool ok = false;
    int64_t content_length = -1;
    size_t write_offset = 0U;
    size_t erased_size = 0U;
    esp_err_t err = ESP_FAIL;

    if (partition == NULL) {
        ESP_LOGE(TAG, "Partition not found: %s", OTA_CACHE_PARTITION_LABEL);
        return false;
    }

    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG,
             "HTTP status=%d, content_length=%" PRId64,
             esp_http_client_get_status_code(client),
             content_length);

    if (esp_http_client_get_status_code(client) != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status: %d", esp_http_client_get_status_code(client));
        goto cleanup;
    }

    if (content_length > 0 && (size_t)content_length > partition->size) {
        ESP_LOGE(TAG,
                 "Package too large for partition. Size=%" PRId64 ", Partition=%u",
                 content_length,
                 (unsigned)partition->size);
        goto cleanup;
    }

    if (content_length > 0) {
        erased_size = ota_align_up_size((size_t)content_length, OTA_FLASH_SECTOR_SIZE);
        if (erased_size > partition->size) {
            ESP_LOGE(TAG,
                     "Aligned erase size exceeds partition. Size=%u, Partition=%u",
                     (unsigned)erased_size,
                     (unsigned)partition->size);
            goto cleanup;
        }

        if (!ota_partition_erase_chunked(partition, 0U, erased_size)) {
            goto cleanup;
        }
    }

    {
        uint8_t read_buffer[OTA_HTTP_READ_BUFFER_SIZE];

        while (true) {
            int read_len = esp_http_client_read(client, (char *)read_buffer, sizeof(read_buffer));
            if (read_len < 0) {
                ESP_LOGE(TAG, "HTTP read failed");
                goto cleanup;
            }

            if (read_len == 0) {
                break;
            }

            if (write_offset + (size_t)read_len > partition->size) {
                ESP_LOGE(TAG, "Package exceeds partition capacity while downloading");
                goto cleanup;
            }

            if (write_offset + (size_t)read_len > erased_size) {
                size_t required_size = ota_align_up_size(write_offset + (size_t)read_len,
                                                         OTA_FLASH_SECTOR_SIZE);
                if (required_size > partition->size) {
                    ESP_LOGE(TAG, "Required erase size exceeds partition capacity");
                    goto cleanup;
                }

                if (!ota_partition_erase_chunked(partition,
                                                 erased_size,
                                                 required_size - erased_size)) {
                    goto cleanup;
                }

                erased_size = required_size;
            }

            err = esp_partition_write(partition, write_offset, read_buffer, (size_t)read_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Partition write failed: %s", esp_err_to_name(err));
                goto cleanup;
            }

            write_offset += (size_t)read_len;
        }
    }

    if (write_offset == 0U) {
        ESP_LOGE(TAG, "Downloaded package is empty");
        goto cleanup;
    }

    {
        const void *mapped_ptr = NULL;
        esp_partition_mmap_handle_t map_handle = 0;

        err = esp_partition_mmap(partition,
                                 0,
                                 write_offset,
                                 ESP_PARTITION_MMAP_DATA,
                                 &mapped_ptr,
                                 &map_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Partition mmap failed: %s", esp_err_to_name(err));
            goto cleanup;
        }

        blob_attach_mmap(package_blob, (const uint8_t *)mapped_ptr, write_offset, map_handle);
    }

    ok = true;
    ESP_LOGI(TAG, "Downloaded .iap package to partition %s: %u bytes", partition->label, (unsigned)write_offset);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool extract_iap_package(ota_iap_context_t *context)
{
    if (!find_zip_entry(&context->package_blob, "manifest.json", &context->manifest_entry) ||
        !find_zip_entry(&context->package_blob, "image-header.bin", &context->image_header_entry) ||
        !find_zip_entry(&context->package_blob, "firmware.bin", &context->firmware_entry)) {
        return false;
    }

    if (context->manifest_entry.uncompressed_size == 0U ||
        context->image_header_entry.uncompressed_size == 0U ||
        context->firmware_entry.uncompressed_size == 0U) {
        ESP_LOGE(TAG, ".iap contains empty required entry");
        return false;
    }

    if (!zip_extract_entry(&context->package_blob,
                           "manifest.json",
                           &context->manifest_entry,
                           &context->manifest_blob,
                           true)) {
        return false;
    }

    if (!parse_manifest(&context->manifest_blob, &context->manifest)) {
        return false;
    }

    if (context->manifest.package_format_version != 3) {
        ESP_LOGE(TAG, "Phase 2 requires packageFormatVersion=3, actual=%d", context->manifest.package_format_version);
        return false;
    }

    if (context->image_header_entry.uncompressed_size != OTA_IMAGE_HEADER_TOTAL_SIZE) {
        ESP_LOGE(TAG,
                 "image-header.bin size mismatch. Expected=%u, Package=%u",
                 (unsigned)OTA_IMAGE_HEADER_TOTAL_SIZE,
                 (unsigned)context->image_header_entry.uncompressed_size);
        return false;
    }

    if (!zip_extract_entry(&context->package_blob,
                           "image-header.bin",
                           &context->image_header_entry,
                           &context->image_header_blob,
                           false)) {
        return false;
    }

    if (context->image_header_blob.size != OTA_IMAGE_HEADER_TOTAL_SIZE) {
        ESP_LOGE(TAG,
                 "image-header.bin extract size mismatch. Expected=%u, Actual=%u",
                 (unsigned)OTA_IMAGE_HEADER_TOTAL_SIZE,
                 (unsigned)context->image_header_blob.size);
        return false;
    }

    memcpy(&context->image_header, context->image_header_blob.data, OTA_IMAGE_HEADER_TOTAL_SIZE);

    if (context->manifest.firmware_size != context->firmware_entry.uncompressed_size) {
        ESP_LOGE(TAG,
                 "Firmware size mismatch. Manifest=%u, Package=%u",
                 (unsigned)context->manifest.firmware_size,
                 (unsigned)context->firmware_entry.uncompressed_size);
        return false;
    }

    ESP_LOGI(TAG,
             ".iap extracted: firmware=%s, size=%u bytes",
             context->manifest.firmware_file_name,
             (unsigned)context->manifest.firmware_size);
    ESP_LOGI(TAG,
             "image-header extracted: target=APP%u version=%s size=%u min=%s",
             (unsigned)context->image_header.payload.target_slot + 1U,
             context->image_header.payload.firmware_version,
             (unsigned)context->image_header.payload.firmware_size,
             context->image_header.payload.min_allowed_version);
    return true;
}

void zip_entry_stream_free(zip_entry_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    free(stream->dictionary);
    memset(stream, 0, sizeof(*stream));
}

bool zip_entry_stream_init(zip_entry_stream_t *stream,
                           const ota_blob_t *package_blob,
                           const char *entry_name,
                           const zip_entry_info_t *entry)
{
    memset(stream, 0, sizeof(*stream));

    if ((entry->flags & 0x0001U) != 0U) {
        ESP_LOGE(TAG, "Encrypted ZIP entry is not supported: %s", entry_name);
        return false;
    }

    if (entry->compression_method != 0U && entry->compression_method != MZ_DEFLATED) {
        ESP_LOGE(TAG,
                 "Unsupported ZIP compression method %u for %s",
                 (unsigned)entry->compression_method,
                 entry_name);
        return false;
    }

    if (!zip_locate_entry_data(package_blob, entry_name, entry, &stream->compressed_data)) {
        return false;
    }

    stream->package_blob = package_blob;
    stream->compressed_size = (size_t)entry->compressed_size;
    stream->uncompressed_size = (size_t)entry->uncompressed_size;
    stream->compression_method = entry->compression_method;

    if (stream->compression_method == MZ_DEFLATED) {
        stream->dictionary = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE);
        if (stream->dictionary == NULL) {
            ESP_LOGE(TAG, "Deflate dictionary allocation failed");
            return false;
        }

        tinfl_init(&stream->inflator);
    }

    return true;
}

bool zip_entry_stream_refill(zip_entry_stream_t *stream)
{
    while (stream->pending_count == 0U && !stream->finished) {
        if (stream->compression_method == 0U) {
            stream->finished = true;
            break;
        }

        {
            size_t out_capacity = TINFL_LZ_DICT_SIZE - stream->dictionary_write_offset;
            size_t in_before = stream->compressed_offset;
            size_t in_available = stream->compressed_size - stream->compressed_offset;
            size_t out_available = 0U;
            tinfl_status status = TINFL_STATUS_FAILED;

            if (out_capacity == 0U) {
                stream->dictionary_write_offset = 0U;
                out_capacity = TINFL_LZ_DICT_SIZE;
            }

            out_available = out_capacity;
            status = tinfl_decompress(&stream->inflator,
                                      stream->compressed_data + stream->compressed_offset,
                                      &in_available,
                                      stream->dictionary,
                                      stream->dictionary + stream->dictionary_write_offset,
                                      &out_available,
                                      0);
            stream->compressed_offset += in_available;

            if (status < 0) {
                ESP_LOGE(TAG, "Deflate stream error: %d", status);
                return false;
            }

            if (out_available > 0U) {
                stream->pending_offset = stream->dictionary_write_offset;
                stream->pending_count = out_available;
                stream->dictionary_write_offset =
                    (stream->dictionary_write_offset + out_available) % TINFL_LZ_DICT_SIZE;
            }

            if (status == TINFL_STATUS_DONE) {
                stream->inflate_done = true;
                if (stream->pending_count == 0U) {
                    stream->finished = true;
                }
                break;
            }

            if (stream->compressed_offset == in_before && out_available == 0U) {
                ESP_LOGE(TAG, "Deflate stream made no progress");
                return false;
            }

            if (status == TINFL_STATUS_NEEDS_MORE_INPUT &&
                stream->compressed_offset >= stream->compressed_size) {
                ESP_LOGE(TAG, "Deflate stream truncated");
                return false;
            }
        }
    }

    return true;
}

bool zip_entry_stream_read(zip_entry_stream_t *stream,
                           uint8_t *buffer,
                           size_t buffer_len,
                           size_t *bytes_read)
{
    size_t total_read = 0U;

    if (bytes_read == NULL) {
        return false;
    }

    *bytes_read = 0U;
    if (buffer_len == 0U) {
        return true;
    }

    if (stream->compression_method == 0U) {
        size_t remaining = stream->uncompressed_size - stream->output_total;
        if (remaining == 0U) {
            stream->finished = true;
            return true;
        }

        total_read = remaining < buffer_len ? remaining : buffer_len;
        memcpy(buffer, stream->compressed_data + stream->output_total, total_read);
        stream->output_total += total_read;
        if (stream->output_total == stream->uncompressed_size) {
            stream->finished = true;
        }

        *bytes_read = total_read;
        return true;
    }

    while (total_read < buffer_len) {
        size_t copy_len = 0U;

        if (stream->pending_count == 0U) {
            if (!zip_entry_stream_refill(stream)) {
                return false;
            }

            if (stream->pending_count == 0U) {
                if (stream->inflate_done) {
                    stream->finished = true;
                    break;
                }

                continue;
            }
        }

        copy_len = (buffer_len - total_read) < stream->pending_count
                       ? (buffer_len - total_read)
                       : stream->pending_count;
        memcpy(buffer + total_read, stream->dictionary + stream->pending_offset, copy_len);
        stream->pending_offset += copy_len;
        stream->pending_count -= copy_len;
        total_read += copy_len;
        stream->output_total += copy_len;

        if (stream->pending_count == 0U && stream->inflate_done) {
            stream->finished = true;
            break;
        }
    }

    if (stream->output_total > stream->uncompressed_size) {
        ESP_LOGE(TAG, "ZIP stream output exceeds expected size");
        return false;
    }

    *bytes_read = total_read;
    return true;
}
