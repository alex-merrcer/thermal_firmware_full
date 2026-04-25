#include "OTA_STM32.h"
#include "ota_stm32_ctrl.h"
#include "ota_stm32_manifest.h"
#include "ota_stm32_package.h"
#include "ota_stm32_verify.h"
#include "ota_stm32_encrypt.h"
#include "ota_stm32_transfer.h"
#include "ota_stm32_cache.h"
#include "WIFI.h"
#include "app_service_bus.h"

#define TAG OTA_STM32_TAG

static TaskHandle_t ota_task_handle = NULL;

static void log_task_stack_watermark(const char *stage)
{
    UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Stack watermark after %s: %u bytes free",
             stage,
             (unsigned)free_bytes);
}

static void ota_apply_boot_report_cache_policy(const ota_stm32_boot_report_t *report)
{
    if (report == NULL || !report->received_any) {
        return;
    }

    if (report->outcome == OTA_BOOT_REPORT_OUTCOME_SUCCESS) {
        ota_cache_clear(false);
    } else if (report->outcome == OTA_BOOT_REPORT_OUTCOME_TERMINAL) {
        ota_cache_clear(true);
    }
}

static bool ota_prepare_package_context(const ota_upgrade_request_t *request,
                                        const ota_upgrade_plan_t *plan,
                                        uint8_t seq,
                                        bool prefer_cached,
                                        size_t cached_package_size,
                                        ota_iap_context_t *context,
                                        ota_validation_result_t *validation_result,
                                        bool *used_cached_package)
{
    if (used_cached_package != NULL) {
        *used_cached_package = false;
    }

    if (prefer_cached) {
        ESP_LOGI(TAG, "Attempt cached package reuse before downloading");
        if (attach_cached_iap_package(cached_package_size, &context->package_blob) &&
            extract_iap_package(context) &&
            validate_iap_package(context, request, plan, validation_result)) {
            if (used_cached_package != NULL) {
                *used_cached_package = true;
            }

            ota_ctrl_send_status(seq,
                                 OTA_CTRL_STAGE_DOWNLOAD,
                                 100U,
                                 0U,
                                 (uint32_t)context->package_blob.size,
                                 (uint32_t)context->package_blob.size);
            ota_ctrl_send_status(seq,
                                 OTA_CTRL_STAGE_VERIFY_CRC,
                                 100U,
                                 0U,
                                 (uint32_t)context->manifest.firmware_size,
                                 (uint32_t)context->manifest.firmware_size);
            ota_ctrl_send_status(seq,
                                 OTA_CTRL_STAGE_VERIFY_SIG,
                                 100U,
                                 0U,
                                 (uint32_t)context->manifest.firmware_size,
                                 (uint32_t)context->manifest.firmware_size);
            return true;
        }

        ESP_LOGW(TAG, "Cached package is unavailable or invalid, fall back to fresh download");
        ota_context_free(context);
        ota_cache_clear(false);
    }

    if (!download_iap_package(plan->package_url, &context->package_blob)) {
        ESP_LOGE(TAG, "Download .iap package failed");
        ota_ctrl_send_error(seq, OTA_CTRL_STAGE_DOWNLOAD, OTA_CTRL_ERR_FETCH_PACKAGE);
        return false;
    }
    log_task_stack_watermark("download");
    ota_ctrl_send_status(seq,
                         OTA_CTRL_STAGE_DOWNLOAD,
                         100U,
                         0U,
                         (uint32_t)context->package_blob.size,
                         (uint32_t)context->package_blob.size);

    if (!extract_iap_package(context)) {
        ESP_LOGE(TAG, "Extract .iap package failed");
        ota_cache_clear(false);
        ota_ctrl_send_error(seq, OTA_CTRL_STAGE_DOWNLOAD, OTA_CTRL_ERR_NO_PACKAGE);
        return false;
    }
    log_task_stack_watermark("extract");

    ota_ctrl_send_status(seq, OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_PERCENT_UNKNOWN, 0U, 0U, 0U);
    if (!validate_iap_package(context, request, plan, validation_result)) {
        ESP_LOGE(TAG, "Package validation failed");
        ota_cache_clear(false);
        ota_ctrl_send_error(seq, validation_result->stage, validation_result->error_code);
        return false;
    }
    log_task_stack_watermark("validate");
    ota_ctrl_send_status(seq,
                         OTA_CTRL_STAGE_VERIFY_CRC,
                         100U,
                         0U,
                         (uint32_t)context->manifest.firmware_size,
                         (uint32_t)context->manifest.firmware_size);
    ota_ctrl_send_status(seq,
                         OTA_CTRL_STAGE_VERIFY_SIG,
                         100U,
                         0U,
                         (uint32_t)context->manifest.firmware_size,
                         (uint32_t)context->manifest.firmware_size);
    return true;
}

static bool ota_process_request(const ota_upgrade_request_t *request,
                                const ota_upgrade_plan_t *plan,
                                uint8_t seq,
                                bool prefer_cached,
                                size_t cached_package_size)
{
    ota_iap_context_t context = {0};
    ota_validation_result_t validation_result = {OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_ERR_PROTOCOL};
    ota_stm32_boot_report_t boot_report = {0};
    ota_go_request_t go_request = {0};
    char transfer_file_name[160] = {0};
    uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN] = {0};
    uint16_t ready_flags = 0U;
    uint32_t plain_size = 0U;
    uint32_t transfer_size = 0U;
    size_t start_transfer_offset = 0U;
    bool used_cached_package = false;
    bool ok = false;

    if (request == NULL || plan == NULL || plan->package_url[0] == '\0' || plan->selected_version[0] == '\0') {
        ota_ctrl_send_error(seq, OTA_CTRL_STAGE_QUERY, OTA_CTRL_ERR_NO_PACKAGE);
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "OTA request accepted: active=%u target=%u product=%s hw=%s current=%s decision=%d target_version=%s flags=0x%08" PRIX32,
             request->active_partition,
             request->target_partition,
             request->product_id[0] != '\0' ? request->product_id : OTA_SUPPORTED_PRODUCT_ID,
             request->hw_rev[0] != '\0' ? request->hw_rev : OTA_SUPPORTED_HW_REV,
             request->version_valid ? request->current_version : "unknown",
             (int)plan->decision,
             plan->selected_version,
             request->flags);
    ESP_LOGI(TAG, "Selected package URL: %s", plan->package_url);

    if (ota_request_is_check_only(request)) {
        ESP_LOGI(TAG, "Check-only request accepted, latest=%s", plan->selected_version);
        vTaskDelay(pdMS_TO_TICKS(OTA_CTRL_READY_GUARD_MS));
        if (!ota_ctrl_send_ready(seq,
                                 request,
                                 plan->selected_version,
                                 0U,
                                 0U,
                                 0U,
                                 0U,
                                 NULL)) {
            ota_ctrl_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL);
            goto cleanup;
        }

        ok = true;
        goto cleanup;
    }

    ota_ctrl_send_status(seq, OTA_CTRL_STAGE_QUERY, OTA_CTRL_PERCENT_UNKNOWN, 0U, 0U, 0U);

    if (!ota_prepare_package_context(request,
                                     plan,
                                     seq,
                                     prefer_cached,
                                     cached_package_size,
                                     &context,
                                     &validation_result,
                                     &used_cached_package)) {
        goto cleanup;
    }

    if (!aes_self_test()) {
        ESP_LOGE(TAG, "AES compatibility self-test failed");
        ota_ctrl_send_error(seq, OTA_CTRL_STAGE_AES_PREPARE, OTA_CTRL_ERR_AES);
        goto cleanup;
    }
    log_task_stack_watermark("aes-self-test");
    ota_ctrl_send_status(seq, OTA_CTRL_STAGE_AES_PREPARE, 100U, 0U, 0U, 0U);

    build_transfer_file_name(context.manifest.firmware_file_name,
                             transfer_file_name,
                             sizeof(transfer_file_name));
    plain_size = (uint32_t)context.manifest.firmware_size;
    transfer_size = (uint32_t)context.manifest.firmware_size;
    ready_flags = OTA_CTRL_READY_FLAG_RESUME_SUPPORTED;
    if (used_cached_package) {
        ready_flags |= OTA_CTRL_READY_FLAG_CACHE_HIT;
    }

    if (!ota_compute_session_fingerprint(&context, session_fingerprint)) {
        ESP_LOGE(TAG, "Failed to compute session fingerprint");
        ota_ctrl_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL);
        goto cleanup;
    }

    if (!ota_cache_store_valid(request,
                               plan,
                               context.package_blob.size,
                               transfer_size,
                               OTA_TRANSFER_CHECKPOINT_SIZE,
                               session_fingerprint)) {
        ESP_LOGW(TAG, "OTA cache metadata persist failed, restart retry may require redownload");
    }

    ESP_LOGI(TAG, "Package ready. Firmware=%s, plain size=%u bytes, transfer size=%u bytes",
             transfer_file_name,
             (unsigned)plain_size,
             (unsigned)transfer_size);

    vTaskDelay(pdMS_TO_TICKS(OTA_CTRL_READY_GUARD_MS));
    if (!ota_ctrl_send_ready(seq,
                             request,
                             plan->selected_version,
                             ready_flags,
                             plain_size,
                             transfer_size,
                             OTA_TRANSFER_CHECKPOINT_SIZE,
                             session_fingerprint)) {
        ESP_LOGE(TAG, "Sending READY failed");
        goto cleanup;
    }

    if (!ota_ctrl_wait_for_go(request, &go_request)) {
        ota_ctrl_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_WAIT_GO);
        goto cleanup;
    }

    if ((go_request.go_flags & OTA_CTRL_GO_FLAG_RESUME_REQUESTED) != 0U) {
        start_transfer_offset = go_request.resume_transfer_offset;
        if (start_transfer_offset == 0U ||
            start_transfer_offset >= transfer_size ||
            (start_transfer_offset % OTA_DATA_DEFAULT_CHUNK_SIZE) != 0U ||
            (start_transfer_offset % OTA_AES_BLOCK_SIZE) != 0U) {
            ESP_LOGE(TAG,
                     "Invalid resume request offset=%u transfer=%u chunk=%u",
                     (unsigned)start_transfer_offset,
                     (unsigned)transfer_size,
                     (unsigned)OTA_DATA_DEFAULT_CHUNK_SIZE);
            ota_ctrl_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL);
            goto cleanup;
        }
    } else if ((ready_flags & OTA_CTRL_READY_FLAG_RESUME_SUPPORTED) != 0U &&
               (ready_flags & OTA_CTRL_READY_FLAG_CACHE_HIT) != 0U) {
        ESP_LOGW(TAG,
                 "STM32 requested fresh transfer; cached package will be reused but resume was not accepted");
    }

    if (!ota_ctrl_send_image_header_meta(seq, &context.image_header)) {
        ESP_LOGE(TAG, "Sending image-header META failed");
        ota_ctrl_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL);
        goto cleanup;
    }

    if (!ymodem_send_encrypted_stream(&context,
                                      transfer_file_name,
                                      start_transfer_offset)) {
        ESP_LOGE(TAG, "OTA data transmit failed");
        boot_report = ota_read_stm32_boot_report_with_timeouts(OTA_BOOT_REPORT_FAIL_TIMEOUT_MS,
                                                               OTA_BOOT_REPORT_FAIL_IDLE_MS);
        ota_apply_boot_report_cache_policy(&boot_report);
        goto cleanup;
    }

    ESP_LOGI(TAG, "STM32 custom OTA data transfer completed");
    if (used_cached_package) {
        ESP_LOGI(TAG, "Cached package reuse completed transfer path");
    }
    boot_report = ota_read_stm32_boot_report();
    ota_apply_boot_report_cache_policy(&boot_report);
    ok = (boot_report.outcome == OTA_BOOT_REPORT_OUTCOME_SUCCESS);

cleanup:
    ota_context_free(&context);
    return ok;
}

static void ota_service_task(void *arg)
{
    (void)arg;
    QueueHandle_t ota_queue = app_service_bus_ota_frame_queue();

    ESP_LOGI(TAG, "OTA service task start");

    while (true) {
        ota_ctrl_frame_t frame = {0};
        ota_upgrade_request_t request = {0};
        ota_upgrade_plan_t plan = {0};
        size_t cached_package_size = 0U;
        bool have_cached_plan = false;
        uint8_t req_seq = 0U;
        uint16_t validation_error = 0U;

        if (ota_queue == NULL ||
            xQueueReceive(ota_queue, &frame, pdMS_TO_TICKS(OTA_CTRL_SERVICE_IDLE_MS)) != pdTRUE) {
            continue;
        }

        if (frame.msg_type != OTA_CTRL_MSG_REQ) {
            continue;
        }

        if (!ota_ctrl_parse_request_payload(frame.payload, frame.payload_len, &request)) {
            ESP_LOGE(TAG, "Invalid request payload");
            continue;
        }
        req_seq = frame.seq;
        ota_ctrl_log_request_event("RX", frame.seq, &request);

        validation_error = ota_ctrl_validate_request(&request);
        if (validation_error != 0U) {
            ota_ctrl_send_ack(req_seq, &request, false, validation_error);
            continue;
        }

        if (!ota_request_is_check_only(&request) && !ota_aes_runtime_ready()) {
            ESP_LOGW(TAG, "Reject OTA transfer because runtime AES key is not configured");
            ota_ctrl_send_ack(req_seq, &request, false, OTA_CTRL_ERR_AES);
            continue;
        }

        if (wifi_service_is_enabled() == 0U) {
            ESP_LOGI(TAG, "Temporarily enable WiFi for OTA request");
            if (wifi_service_set_enabled(1U) != ESP_OK) {
                ota_ctrl_send_ack(req_seq, &request, false, OTA_CTRL_ERR_NO_WIFI);
                continue;
            }
        }

        if (!ota_request_is_check_only(&request)) {
            have_cached_plan = ota_cache_try_prepare_plan(&request, &plan, &cached_package_size);
        }

        if (!have_cached_plan) {
            if (!ota_prepare_upgrade_plan(&request, &plan, &validation_error)) {
                ota_ctrl_send_ack(req_seq, &request, false, validation_error);
                continue;
            }
        }

        ota_ctrl_send_ack(req_seq, &request, true, 0U);
        app_service_bus_set_bits(APP_EVENT_OTA_RUNNING);
        ota_process_request(&request, &plan, req_seq, have_cached_plan, cached_package_size);
        app_service_bus_clear_bits(APP_EVENT_OTA_RUNNING);
    }
}

void OTA_STM32_Init(void)
{
    ESP_LOGI(TAG, "OTA STM32 init");
    ota_log_aes_security_profile();
    ESP_LOGI(TAG, "OTA metadata=%s/%s, product=%s",
             OTA_PACKAGE_BASE_URL,
             OTA_LATEST_JSON_NAME,
             OTA_SUPPORTED_PRODUCT_ID);
}

void OTA_STM32_Start(void)
{
    if (ota_task_handle != NULL) {
        ESP_LOGW(TAG, "OTA already running");
        return;
    }

    ESP_LOGI(TAG, "Start STM32 OTA control service");
    if (xTaskCreate(ota_service_task, "OTA_Task", OTA_TASK_STACK_SIZE, NULL, 5, &ota_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        ota_task_handle = NULL;
    }
}
