#include "ota_stm32_cache.h"
#include "nvs.h"

#define TAG OTA_STM32_TAG

#define OTA_CACHE_NVS_NAMESPACE  "ota_stm32"
#define OTA_CACHE_NVS_KEY        "txn"
#define OTA_CACHE_RECORD_MAGIC   0x4F544331UL
#define OTA_CACHE_RECORD_VERSION 2U

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t struct_size;
    uint8_t valid;
    uint8_t terminal;
    uint8_t target_partition;
    uint8_t reserved0;
    uint32_t package_size;
    uint32_t transfer_size;
    uint32_t checkpoint_size;
    char package_url[OTA_PACKAGE_URL_MAX_LEN];
    char current_version[OTA_CTRL_VERSION_LEN + 1U];
    char selected_version[OTA_CTRL_VERSION_LEN + 1U];
    uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN];
} ota_cache_record_t;

static void ota_cache_record_init(ota_cache_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = OTA_CACHE_RECORD_MAGIC;
    record->version = OTA_CACHE_RECORD_VERSION;
    record->struct_size = (uint16_t)sizeof(*record);
}

static bool ota_cache_record_string_valid(const char *text, size_t text_len, bool allow_empty)
{
    if (text == NULL) {
        return false;
    }

    if (memchr(text, '\0', text_len) == NULL) {
        return false;
    }

    if (!allow_empty && text[0] == '\0') {
        return false;
    }

    return true;
}

static bool ota_cache_record_is_valid(const ota_cache_record_t *record)
{
    static const uint8_t s_zero_fingerprint[OTA_CTRL_FINGERPRINT_LEN] = {0};

    if (record == NULL) {
        return false;
    }

    if (record->magic != OTA_CACHE_RECORD_MAGIC ||
        record->version != OTA_CACHE_RECORD_VERSION ||
        record->struct_size != sizeof(*record)) {
        return false;
    }

    if (record->valid > 1U || record->terminal > 1U) {
        return false;
    }

    if (record->target_partition > OTA_CTRL_PARTITION_APP2) {
        return false;
    }

    if (!ota_cache_record_string_valid(record->package_url, sizeof(record->package_url), true) ||
        !ota_cache_record_string_valid(record->current_version, sizeof(record->current_version), true) ||
        !ota_cache_record_string_valid(record->selected_version, sizeof(record->selected_version), true)) {
        return false;
    }

    if (record->valid != 0U) {
        if (record->package_size == 0U ||
            record->transfer_size == 0U ||
            record->checkpoint_size != OTA_TRANSFER_CHECKPOINT_SIZE ||
            record->package_url[0] == '\0' ||
            record->current_version[0] == '\0' ||
            record->selected_version[0] == '\0' ||
            memcmp(record->session_fingerprint,
                   s_zero_fingerprint,
                   sizeof(record->session_fingerprint)) == 0) {
            return false;
        }
    }

    return true;
}

static bool ota_cache_record_load(ota_cache_record_t *record)
{
    nvs_handle_t handle = 0;
    size_t size = sizeof(*record);
    esp_err_t err = ESP_FAIL;

    if (record == NULL) {
        return false;
    }

    ota_cache_record_init(record);
    err = nvs_open(OTA_CACHE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open OTA cache NVS failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_get_blob(handle, OTA_CACHE_NVS_KEY, record, &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Read OTA cache NVS failed: %s", esp_err_to_name(err));
        }
        return false;
    }

    if (size != sizeof(*record) || !ota_cache_record_is_valid(record)) {
        ESP_LOGW(TAG, "Ignoring invalid OTA cache metadata");
        return false;
    }

    return true;
}

static bool ota_cache_record_save(const ota_cache_record_t *record)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_FAIL;

    if (record == NULL || !ota_cache_record_is_valid(record)) {
        return false;
    }

    err = nvs_open(OTA_CACHE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open OTA cache NVS for write failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(handle, OTA_CACHE_NVS_KEY, record, sizeof(*record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Persist OTA cache NVS failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool ota_cache_request_matches(const ota_upgrade_request_t *request,
                                      const ota_cache_record_t *record)
{
    if (request == NULL || record == NULL) {
        return false;
    }

    if (!request->version_valid || request->current_version[0] == '\0') {
        return false;
    }

    if (request->target_partition != record->target_partition) {
        return false;
    }

    return strcmp(request->current_version, record->current_version) == 0;
}

static void ota_cache_log_request_mismatch(const ota_upgrade_request_t *request,
                                           const ota_cache_record_t *record)
{
    if (request == NULL || record == NULL) {
        return;
    }

    if (!request->version_valid || request->current_version[0] == '\0') {
        ESP_LOGW(TAG, "Skip cached OTA metadata reuse: request current version is unavailable");
        return;
    }

    if (request->target_partition != record->target_partition) {
        ESP_LOGW(TAG,
                 "Skip cached OTA metadata reuse: target partition mismatch req=APP%u record=APP%u",
                 (unsigned)request->target_partition + 1U,
                 (unsigned)record->target_partition + 1U);
        return;
    }

    if (strcmp(request->current_version, record->current_version) != 0) {
        ESP_LOGW(TAG,
                 "Skip cached OTA metadata reuse: current version mismatch req=%s record=%s",
                 request->current_version,
                 record->current_version);
        return;
    }
}

bool ota_cache_try_prepare_plan(const ota_upgrade_request_t *request,
                                ota_upgrade_plan_t *plan,
                                size_t *package_size)
{
    ota_cache_record_t record;

    if (plan != NULL) {
        memset(plan, 0, sizeof(*plan));
    }
    if (package_size != NULL) {
        *package_size = 0U;
    }

    if (plan == NULL || request == NULL) {
        return false;
    }

    if (!ota_cache_record_load(&record)) {
        return false;
    }

    if (record.valid == 0U || record.terminal != 0U) {
        return false;
    }

    if (!ota_cache_request_matches(request, &record)) {
        ota_cache_log_request_mismatch(request, &record);
        return false;
    }

    plan->decision = OTA_DECISION_UPGRADE;
    snprintf(plan->selected_version, sizeof(plan->selected_version), "%s", record.selected_version);
    snprintf(plan->package_url, sizeof(plan->package_url), "%s", record.package_url);
    if (package_size != NULL) {
        *package_size = record.package_size;
    }

    ESP_LOGI(TAG,
             "Reusing cached OTA metadata: current=%s target=%s partition=APP%u size=%u transfer=%u fp=%02X%02X%02X%02X",
             record.current_version,
             record.selected_version,
             (unsigned)record.target_partition + 1U,
             (unsigned)record.package_size,
             (unsigned)record.transfer_size,
             record.session_fingerprint[0],
             record.session_fingerprint[1],
             record.session_fingerprint[2],
             record.session_fingerprint[3]);
    return true;
}

bool ota_cache_store_valid(const ota_upgrade_request_t *request,
                           const ota_upgrade_plan_t *plan,
                           size_t package_size,
                           uint32_t transfer_size,
                           uint32_t checkpoint_size,
                           const uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN])
{
    ota_cache_record_t record;

    if (request == NULL || plan == NULL || !request->version_valid ||
        request->current_version[0] == '\0' ||
        plan->selected_version[0] == '\0' ||
        plan->package_url[0] == '\0' ||
        package_size == 0U ||
        transfer_size == 0U ||
        checkpoint_size != OTA_TRANSFER_CHECKPOINT_SIZE ||
        session_fingerprint == NULL) {
        ESP_LOGW(TAG, "Skip OTA cache persist because request/plan is incomplete");
        return false;
    }

    ota_cache_record_init(&record);
    record.valid = 1U;
    record.terminal = 0U;
    record.target_partition = request->target_partition;
    record.package_size = (uint32_t)package_size;
    record.transfer_size = transfer_size;
    record.checkpoint_size = checkpoint_size;
    snprintf(record.package_url, sizeof(record.package_url), "%s", plan->package_url);
    snprintf(record.current_version, sizeof(record.current_version), "%s", request->current_version);
    snprintf(record.selected_version, sizeof(record.selected_version), "%s", plan->selected_version);
    memcpy(record.session_fingerprint,
           session_fingerprint,
           sizeof(record.session_fingerprint));

    if (!ota_cache_record_save(&record)) {
        return false;
    }

    ESP_LOGI(TAG,
             "Persisted OTA cache metadata: current=%s target=%s partition=APP%u size=%u transfer=%u checkpoint=%u fp=%02X%02X%02X%02X",
             record.current_version,
             record.selected_version,
             (unsigned)record.target_partition + 1U,
             (unsigned)record.package_size,
             (unsigned)record.transfer_size,
             (unsigned)record.checkpoint_size,
             record.session_fingerprint[0],
             record.session_fingerprint[1],
             record.session_fingerprint[2],
             record.session_fingerprint[3]);
    return true;
}

void ota_cache_clear(bool terminal)
{
    ota_cache_record_t record;

    if (!ota_cache_record_load(&record)) {
        ota_cache_record_init(&record);
    }

    record.valid = 0U;
    record.terminal = terminal ? 1U : 0U;
    (void)ota_cache_record_save(&record);
}
