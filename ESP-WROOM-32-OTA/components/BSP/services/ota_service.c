#include "ota_service.h"

#include "OTA_STM32.h"
#include "WIFI.h"
#include "app_service_bus.h"
#include "host_ctrl_service.h"
#include "ota_stm32_internal.h"

static const char *TAG = "OTA_SERVICE";
#define OTA_REQUEST_DEDUP_WINDOW_MS 60000U

static TaskHandle_t s_ota_task_handle = NULL;
static uint8_t s_last_request_seq = 0U;
static uint8_t s_last_request_seq_valid = 0U;
static uint16_t s_last_request_payload_len = 0U;
static uint8_t s_last_request_payload[OTA_CTRL_MAX_PAYLOAD_LEN] = {0};
static TickType_t s_last_request_tick = 0U;

static void ota_service_report_cloud_status(uint8_t stage,
                                            uint8_t percent,
                                            uint16_t detail_code,
                                            uint32_t current_value,
                                            uint32_t total_value)
{
    esp_err_t err = app_service_bus_submit_ota_status(stage,
                                                      percent,
                                                      detail_code,
                                                      current_value,
                                                      total_value);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Submit OTA status to cloud queue failed: 0x%04X", (unsigned int)err);
    }
}

static void ota_service_send_status(uint8_t seq,
                                    uint8_t stage,
                                    uint8_t percent,
                                    uint16_t detail_code,
                                    uint32_t current_value,
                                    uint32_t total_value)
{
    ota_service_report_cloud_status(stage, percent, detail_code, current_value, total_value);
    (void)ota_ctrl_send_status(seq, stage, percent, detail_code, current_value, total_value);
}

static void ota_service_send_error(uint8_t seq, uint8_t stage, uint16_t error_code)
{
    ota_service_report_cloud_status(stage, OTA_CTRL_PERCENT_UNKNOWN, error_code, 0U, 0U);
    (void)ota_ctrl_send_error(seq, stage, error_code);
}

static void ota_service_begin_request_guard(void)
{
    app_service_bus_set_bits(APP_EVENT_OTA_RUNNING);
    if (wifi_service_set_ota_guard(1U) != ESP_OK)
    {
        ESP_LOGW(TAG, "Enable WiFi OTA guard failed");
    }
    host_ctrl_service_request_runtime_apply();
    /* Apply OTA runtime radio policy immediately so BLE/coexist state is settled
     * before the following HTTPS package fetch starts. */
    host_ctrl_service_step();
}

static void ota_service_end_request_guard(void)
{
    if (wifi_service_set_ota_guard(0U) != ESP_OK)
    {
        ESP_LOGW(TAG, "Disable WiFi OTA guard failed");
    }
    host_ctrl_service_request_runtime_apply();
    app_service_bus_clear_bits(APP_EVENT_OTA_RUNNING);
}

static bool ota_service_is_duplicate_request(const ota_ctrl_frame_t *frame)
{
    TickType_t now = xTaskGetTickCount();

    if (frame == NULL ||
        s_last_request_seq_valid == 0U ||
        s_last_request_seq != frame->seq ||
        s_last_request_payload_len != frame->payload_len)
    {
        return false;
    }

    if (memcmp(s_last_request_payload, frame->payload, frame->payload_len) != 0)
    {
        return false;
    }

    return ((now - s_last_request_tick) < pdMS_TO_TICKS(OTA_REQUEST_DEDUP_WINDOW_MS));
}

static void ota_service_note_request(const ota_ctrl_frame_t *frame)
{
    if (frame == NULL)
    {
        return;
    }

    s_last_request_seq = frame->seq;
    s_last_request_seq_valid = 1U;
    s_last_request_payload_len = frame->payload_len;
    if (frame->payload_len != 0U)
    {
        memcpy(s_last_request_payload, frame->payload, frame->payload_len);
    }
    s_last_request_tick = xTaskGetTickCount();
}

static void log_task_stack_watermark(const char *stage)
{
    UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Stack watermark after %s: %u bytes free",
             stage,
             (unsigned)free_bytes);
}

static void ota_apply_boot_report_cache_policy(const ota_stm32_boot_report_t *report)
{
    if (report == NULL || !report->received_any)
    {
        return;
    }

    if (report->outcome == OTA_BOOT_REPORT_OUTCOME_SUCCESS)
    {
        ota_cache_clear(false);
    }
    else if (report->outcome == OTA_BOOT_REPORT_OUTCOME_TERMINAL)
    {
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
    if (used_cached_package != NULL)
    {
        *used_cached_package = false;
    }

    if (prefer_cached)
    {
        ESP_LOGI(TAG, "Attempt cached package reuse before downloading");
        if (attach_cached_iap_package(cached_package_size, &context->package_blob) &&
            extract_iap_package(context) &&
            validate_iap_package(context, request, plan, validation_result))
        {
            if (used_cached_package != NULL)
            {
                *used_cached_package = true;
            }

            ota_service_send_status(seq,
                                    OTA_CTRL_STAGE_DOWNLOAD,
                                    100U,
                                    0U,
                                    (uint32_t)context->package_blob.size,
                                    (uint32_t)context->package_blob.size);
            ota_service_send_status(seq,
                                    OTA_CTRL_STAGE_VERIFY_CRC,
                                    100U,
                                    0U,
                                    (uint32_t)context->manifest.firmware_size,
                                    (uint32_t)context->manifest.firmware_size);
            ota_service_send_status(seq,
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

    if (!download_iap_package(plan->package_url, &context->package_blob))
    {
        ESP_LOGE(TAG, "Download .iap package failed");
        ota_service_send_error(seq, OTA_CTRL_STAGE_DOWNLOAD, OTA_CTRL_ERR_FETCH_PACKAGE);
        return false;
    }
    log_task_stack_watermark("download");
    ota_service_send_status(seq,
                            OTA_CTRL_STAGE_DOWNLOAD,
                            100U,
                            0U,
                            (uint32_t)context->package_blob.size,
                            (uint32_t)context->package_blob.size);

    if (!extract_iap_package(context))
    {
        ESP_LOGE(TAG, "Extract .iap package failed");
        ota_cache_clear(false);
        ota_service_send_error(seq, OTA_CTRL_STAGE_DOWNLOAD, OTA_CTRL_ERR_NO_PACKAGE);
        return false;
    }
    log_task_stack_watermark("extract");

    ota_service_send_status(seq, OTA_CTRL_STAGE_VERIFY_CRC, OTA_CTRL_PERCENT_UNKNOWN, 0U, 0U, 0U);
    if (!validate_iap_package(context, request, plan, validation_result))
    {
        ESP_LOGE(TAG, "Package validation failed");
        ota_cache_clear(false);
        ota_service_send_error(seq, validation_result->stage, validation_result->error_code);
        return false;
    }
    log_task_stack_watermark("validate");
    ota_service_send_status(seq,
                            OTA_CTRL_STAGE_VERIFY_CRC,
                            100U,
                            0U,
                            (uint32_t)context->manifest.firmware_size,
                            (uint32_t)context->manifest.firmware_size);
    ota_service_send_status(seq,
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

    if (request == NULL || plan == NULL || plan->package_url[0] == '\0' || plan->selected_version[0] == '\0')
    {
        ota_service_send_error(seq, OTA_CTRL_STAGE_QUERY, OTA_CTRL_ERR_NO_PACKAGE);
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

    if (ota_request_is_check_only(request))
    {
        ESP_LOGI(TAG, "Check-only request accepted, latest=%s", plan->selected_version);
        vTaskDelay(pdMS_TO_TICKS(OTA_CTRL_READY_GUARD_MS));
        if (!ota_ctrl_send_ready(seq,
                                 request,
                                 plan->selected_version,
                                 0U,
                                 0U,
                                 0U,
                                 0U,
                                 NULL))
        {
            ota_service_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL);
            goto cleanup;
        }

        ota_service_report_cloud_status(OTA_CTRL_STAGE_READY, 100U, 0U, 0U, 0U);
        ok = true;
        goto cleanup;
    }

    ota_service_send_status(seq, OTA_CTRL_STAGE_QUERY, OTA_CTRL_PERCENT_UNKNOWN, 0U, 0U, 0U);

    if (!ota_prepare_package_context(request,
                                     plan,
                                     seq,
                                     prefer_cached,
                                     cached_package_size,
                                     &context,
                                     &validation_result,
                                     &used_cached_package))
    {
        goto cleanup;
    }

    if (!aes_self_test())
    {
        ESP_LOGE(TAG, "AES compatibility self-test failed");
        ota_service_send_error(seq, OTA_CTRL_STAGE_AES_PREPARE, OTA_CTRL_ERR_AES);
        goto cleanup;
    }
    log_task_stack_watermark("aes-self-test");
    ota_service_send_status(seq, OTA_CTRL_STAGE_AES_PREPARE, 100U, 0U, 0U, 0U);

    build_transfer_file_name(context.manifest.firmware_file_name,
                             transfer_file_name,
                             sizeof(transfer_file_name));
    plain_size = (uint32_t)context.manifest.firmware_size;
    transfer_size = (uint32_t)context.manifest.firmware_size;
    ready_flags = OTA_CTRL_READY_FLAG_RESUME_SUPPORTED;
    if (used_cached_package)
    {
        ready_flags |= OTA_CTRL_READY_FLAG_CACHE_HIT;
    }

    if (!ota_compute_session_fingerprint(&context, session_fingerprint))
    {
        ESP_LOGE(TAG, "Failed to compute session fingerprint");
        ota_service_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL);
        goto cleanup;
    }

    if (!ota_cache_store_valid(request,
                               plan,
                               context.package_blob.size,
                               transfer_size,
                               OTA_TRANSFER_CHECKPOINT_SIZE,
                               session_fingerprint))
    {
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
                             session_fingerprint))
    {
        ESP_LOGE(TAG, "Sending READY failed");
        ota_service_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL);
        goto cleanup;
    }
    ota_service_report_cloud_status(OTA_CTRL_STAGE_READY, 100U, 0U, plain_size, transfer_size);

    if (!ota_ctrl_wait_for_go(request, &go_request))
    {
        ota_service_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_WAIT_GO);
        goto cleanup;
    }

    if ((go_request.go_flags & OTA_CTRL_GO_FLAG_RESUME_REQUESTED) != 0U)
    {
        start_transfer_offset = go_request.resume_transfer_offset;
        if (start_transfer_offset == 0U ||
            start_transfer_offset >= transfer_size ||
            (start_transfer_offset % OTA_DATA_DEFAULT_CHUNK_SIZE) != 0U ||
            (start_transfer_offset % OTA_AES_BLOCK_SIZE) != 0U)
        {
            ESP_LOGE(TAG,
                     "Invalid resume request offset=%u transfer=%u chunk=%u",
                     (unsigned)start_transfer_offset,
                     (unsigned)transfer_size,
                     (unsigned)OTA_DATA_DEFAULT_CHUNK_SIZE);
            ota_service_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL);
            goto cleanup;
        }
    }
    else if ((ready_flags & OTA_CTRL_READY_FLAG_RESUME_SUPPORTED) != 0U &&
             (ready_flags & OTA_CTRL_READY_FLAG_CACHE_HIT) != 0U)
    {
        ESP_LOGW(TAG,
                 "STM32 requested fresh transfer; cached package will be reused but resume was not accepted");
    }

    if (!ota_ctrl_send_image_header_meta(seq, &context.image_header))
    {
        ESP_LOGE(TAG, "Sending image-header META failed");
        ota_service_send_error(seq, OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL);
        goto cleanup;
    }

    ota_service_report_cloud_status(OTA_CTRL_STAGE_TRANSFER,
                                    OTA_CTRL_PERCENT_UNKNOWN,
                                    0U,
                                    (uint32_t)start_transfer_offset,
                                    transfer_size);
    if (!ymodem_send_encrypted_stream(&context,
                                      transfer_file_name,
                                      start_transfer_offset))
    {
        ESP_LOGE(TAG, "OTA data transmit failed");
        ota_service_send_error(seq, OTA_CTRL_STAGE_TRANSFER, OTA_CTRL_ERR_TRANSFER);
        boot_report = ota_read_stm32_boot_report_with_timeouts(OTA_BOOT_REPORT_FAIL_TIMEOUT_MS,
                                                               OTA_BOOT_REPORT_FAIL_IDLE_MS);
        ota_apply_boot_report_cache_policy(&boot_report);
        goto cleanup;
    }

    ESP_LOGI(TAG, "STM32 custom OTA data transfer completed");
    if (used_cached_package)
    {
        ESP_LOGI(TAG, "Cached package reuse completed transfer path");
    }
    boot_report = ota_read_stm32_boot_report();
    ota_apply_boot_report_cache_policy(&boot_report);
    ok = (boot_report.outcome == OTA_BOOT_REPORT_OUTCOME_SUCCESS);
    if (ok)
    {
        ota_service_report_cloud_status(OTA_CTRL_STAGE_DONE, 100U, 0U, transfer_size, transfer_size);
    }

cleanup:
    ota_context_free(&context);
    return ok;
}

static void ota_service_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "OTA service task start");
    ota_ctrl_flush_uart();
    host_ctrl_service_init();

    while (true)
    {
        ota_ctrl_frame_t frame = {0};
        ota_upgrade_request_t request = {0};
        ota_upgrade_plan_t plan = {0};
        size_t cached_package_size = 0U;
        bool have_cached_plan = false;
        bool request_guard_active = false;
        uint8_t req_seq = 0U;
        uint16_t validation_error = 0U;

        host_ctrl_service_step();

        if (!ota_ctrl_receive_frame(&frame, OTA_CTRL_SERVICE_IDLE_MS))
        {
            continue;
        }

        app_service_bus_set_bits(APP_EVENT_STM32_ONLINE);

        if (frame.msg_type == OTA_CTRL_MSG_HOST_REQ)
        {
            (void)host_ctrl_service_handle_frame(&frame);
            continue;
        }

        if (frame.msg_type != OTA_CTRL_MSG_REQ)
        {
            continue;
        }

        if (!ota_ctrl_parse_request_payload(frame.payload, frame.payload_len, &request))
        {
            ESP_LOGE(TAG, "Invalid request payload");
            continue;
        }
        req_seq = frame.seq;
        ota_ctrl_log_request_event("RX", frame.seq, &request);

        if (ota_service_is_duplicate_request(&frame))
        {
            ESP_LOGW(TAG, "Drop duplicate OTA request seq=%u within dedupe window", (unsigned int)req_seq);
            ota_ctrl_send_ack(req_seq, &request, false, OTA_CTRL_ERR_BUSY);
            ota_service_report_cloud_status(OTA_CTRL_STAGE_QUERY,
                                            OTA_CTRL_PERCENT_UNKNOWN,
                                            OTA_CTRL_ERR_BUSY,
                                            0U,
                                            0U);
            continue;
        }

        ota_service_note_request(&frame);
        ota_service_begin_request_guard();
        request_guard_active = true;

        validation_error = ota_ctrl_validate_request(&request);
        if (validation_error != 0U)
        {
            ota_ctrl_send_ack(req_seq, &request, false, validation_error);
            ota_service_report_cloud_status(OTA_CTRL_STAGE_QUERY,
                                            OTA_CTRL_PERCENT_UNKNOWN,
                                            validation_error,
                                            0U,
                                            0U);
            goto request_cleanup;
        }

        if (!ota_request_is_check_only(&request) && !ota_aes_runtime_ready())
        {
            ESP_LOGW(TAG, "Reject OTA transfer because runtime AES key is not configured");
            ota_ctrl_send_ack(req_seq, &request, false, OTA_CTRL_ERR_AES);
            ota_service_report_cloud_status(OTA_CTRL_STAGE_AES_PREPARE,
                                            OTA_CTRL_PERCENT_UNKNOWN,
                                            OTA_CTRL_ERR_AES,
                                            0U,
                                            0U);
            goto request_cleanup;
        }

        if (wifi_service_is_enabled() == 0U)
        {
            ESP_LOGI(TAG, "Temporarily enable WiFi for OTA request");
            if (wifi_service_set_enabled(1U) != ESP_OK)
            {
                ota_ctrl_send_ack(req_seq, &request, false, OTA_CTRL_ERR_NO_WIFI);
                ota_service_report_cloud_status(OTA_CTRL_STAGE_QUERY,
                                                OTA_CTRL_PERCENT_UNKNOWN,
                                                OTA_CTRL_ERR_NO_WIFI,
                                                0U,
                                                0U);
                goto request_cleanup;
            }
        }

        if (!ota_request_is_check_only(&request))
        {
            have_cached_plan = ota_cache_try_prepare_plan(&request, &plan, &cached_package_size);
        }

        if (!have_cached_plan)
        {
            if (!ota_prepare_upgrade_plan(&request, &plan, &validation_error))
            {
                ota_ctrl_send_ack(req_seq, &request, false, validation_error);
                ota_service_report_cloud_status(OTA_CTRL_STAGE_QUERY,
                                                OTA_CTRL_PERCENT_UNKNOWN,
                                                validation_error,
                                                0U,
                                                0U);
                goto request_cleanup;
            }
        }

        ota_ctrl_send_ack(req_seq, &request, true, 0U);
        ota_process_request(&request, &plan, req_seq, have_cached_plan, cached_package_size);

request_cleanup:
        if (request_guard_active)
        {
            ota_service_end_request_guard();
        }
    }
}

void ota_service_init(void)
{
    OTA_STM32_Init();
}

void ota_service_start(void)
{
    if (s_ota_task_handle != NULL)
    {
        ESP_LOGW(TAG, "OTA service already running");
        return;
    }

    ESP_LOGI(TAG, "Start OTA service task");
    if (xTaskCreate(ota_service_task,
                    "ota_service",
                    OTA_TASK_STACK_SIZE,
                    NULL,
                    5,
                    &s_ota_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create OTA service task");
        s_ota_task_handle = NULL;
    }
}
