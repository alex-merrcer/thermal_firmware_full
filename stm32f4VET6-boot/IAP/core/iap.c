#include "iap_boot_info.h"
#include "iap_boot_entry.h"
#include "iap_auth.h"
#include "iap_ctrl.h"
#include "iap_ui.h"
#include "iap_platform.h"

uint8_t tab_1024[1024] = {0};

typedef struct
{
    OtaTxnRecord *txn;
    uint8_t save_failed;
} iap_checkpoint_context_t;

static void iap_send_txn_load_diag(const OtaTxnRecord *txn, const iap_txn_load_diag_t *diag)
{
    uint16_t detail_code = 0U;

    if (txn == 0 || diag == 0)
    {
        return;
    }

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_CORE | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code,
                               txn->state,
                               txn->protocol_version);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_OFFSETS | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code,
                               txn->resume_offset,
                               txn->last_acked_offset);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_META | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code,
                               txn->checkpoint_size,
                               diag->invalid_slots);

    if (diag->invalid_slots == 0U)
    {
        return;
    }

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT1 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code,
                               diag->invalid_slot_header,
                               diag->invalid_slot_commit);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT2 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code,
                               diag->invalid_slot_payload_crc,
                               diag->invalid_slot_payload_validate);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_TXN1 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code,
                               diag->invalid_txn_layout,
                               diag->invalid_txn_state);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_TXN2 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code,
                               diag->invalid_txn_partition,
                               diag->invalid_txn_fields);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_TXN3 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code,
                               diag->invalid_txn_request_type,
                               diag->invalid_txn_version_text);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_TXN4 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code,
                               diag->invalid_txn_offset,
                               diag->invalid_txn_data_crc);
}

static uint8_t iap_try_jump_partition(uint32_t partition)
{
    APPLICATION_ADDRESS = boot_info_partition_address(partition);
    if (!is_app_valid(APPLICATION_ADDRESS))
    {
        return 0U;
    }

    iap_init_watchdog();
    jump_to_app(APPLICATION_ADDRESS);
    return 1U;
}

static void iap_return_to_active_app(const BootInfoTypeDef *boot_info,
                                     const char *line1,
                                     const char *line2)
{
    (void)line1;
    (void)line2;
    delay_ms(200);

    if (boot_info != 0 && iap_try_jump_partition(boot_info->active_partition) != 0U)
    {
        return;
    }

    iap_ui_show_upgrade_failure();
}

static void iap_reset_request_and_return_active_app(BootInfoTypeDef *boot_info,
                                                    const char *line1,
                                                    const char *line2)
{
    if (boot_info != 0)
    {
        boot_info->boot_magic = MAGIC_NORMAL;
        boot_info->upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
        if (boot_info->trial_state == BOOT_INFO_TRIAL_NONE)
        {
            boot_info->confirmed_slot = boot_info->active_partition;
            version_text_copy(boot_info->last_good_version,
                              BOOT_INFO_VERSION_LEN,
                              boot_info->current_version);
            version_text_copy(boot_info->pending_floor_version,
                              BOOT_INFO_VERSION_LEN,
                              IAP_DEFAULT_VERSION);
        }
        boot_info_sync_current_version(boot_info);
        boot_info_save(boot_info);
    }

    (void)line1;
    (void)line2;
    iap_ui_show_upgrade_failure();
    delay_ms(1000);
    iap_return_to_active_app(boot_info, line1, line2);
    NVIC_SystemReset();
}

static uint32_t iap_txn_next_counter(uint32_t current)
{
    uint32_t next = current + 1U;

    if (next == 0U || next == 0xFFFFFFFFUL)
    {
        next = 1U;
    }

    return next;
}

static void iap_txn_reset_transfer_info(OtaTxnRecord *txn)
{
    if (txn == 0)
    {
        return;
    }

    txn->transfer_total_size = 0U;
    txn->plain_size = 0U;
    txn->resume_offset = 0U;
    txn->last_acked_offset = 0U;
    txn->pause_reason = IAP_TXN_PAUSE_NONE;
    memset(txn->session_fingerprint, 0, sizeof(txn->session_fingerprint));
}

static void iap_txn_begin_new(OtaTxnRecord *txn, const BootInfoTypeDef *boot_info)
{
    uint32_t previous_counter = 0U;

    if (txn == 0 || boot_info == 0)
    {
        return;
    }

    previous_counter = txn->txn_counter;
    txn_init_default(txn);
    txn->txn_counter = iap_txn_next_counter(previous_counter);
    txn->state = IAP_TXN_STATE_NEGOTIATING;
    txn->request_type = OTA_CTRL_REQ_TYPE_UPGRADE;
    txn->active_slot = boot_info->active_partition;
    txn->target_slot = boot_info->target_partition;
    version_text_copy(txn->current_version, BOOT_INFO_VERSION_LEN, boot_info->current_version);
    version_text_copy(txn->target_version, BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    txn->retryable = 1U;
    txn->protocol_version = OTA_CTRL_PROTOCOL_VERSION;
    txn->checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    txn->chunk_size = OTA_DATA_DEFAULT_CHUNK_SIZE;
}

static uint32_t iap_resume_effective_offset(const OtaTxnRecord *txn,
                                            const ota_ctrl_ready_info_t *ready_info)
{
    uint32_t resume_offset = 0U;

    if (txn == 0 || ready_info == 0)
    {
        return 0U;
    }

    resume_offset = txn->resume_offset;
    if (resume_offset != 0U)
    {
        return resume_offset;
    }

    if ((txn->state != IAP_TXN_STATE_PAUSED_RESUMABLE &&
         txn->state != IAP_TXN_STATE_FAILED_RETRYABLE) ||
        txn->last_acked_offset == 0U)
    {
        return 0U;
    }

    resume_offset = txn->last_acked_offset;
    if (resume_offset >= ready_info->transfer_size)
    {
        return 0U;
    }

    if (txn->chunk_size == 0U ||
        (resume_offset % txn->chunk_size) != 0U ||
        (resume_offset % 16U) != 0U)
    {
        return 0U;
    }

    return resume_offset;
}

static uint8_t iap_txn_store_state(OtaTxnRecord *txn,
                                   uint32_t state,
                                   uint32_t error_stage,
                                   uint32_t error_code,
                                   uint32_t retryable)
{
    if (txn == 0)
    {
        return 0U;
    }

    txn->state = state;
    txn->last_error_stage = error_stage;
    txn->last_error_code = error_code;
    txn->retryable = (retryable != 0U) ? 1U : 0U;
    return (txn_save(txn) == 0U) ? 1U : 0U;
}

static uint8_t iap_transfer_error_is_terminal(uint8_t err_code)
{
    return (err_code == YMODEM_ERR_HEADER ||
            err_code == YMODEM_ERR_AUTH ||
            err_code == YMODEM_ERR_VERSION ||
            err_code == YMODEM_ERR_SLOT) ? 1U : 0U;
}

static uint16_t iap_resume_match_reason(const OtaTxnRecord *txn,
                                        const BootInfoTypeDef *boot_info,
                                        const ota_ctrl_ready_info_t *ready_info)
{
    uint32_t resume_offset = 0U;

    if (txn == 0 || boot_info == 0 || ready_info == 0)
    {
        return OTA_CTRL_RESUME_REASON_STATE;
    }

    if (txn->state != IAP_TXN_STATE_PAUSED_RESUMABLE &&
        txn->state != IAP_TXN_STATE_FAILED_RETRYABLE)
    {
        return OTA_CTRL_RESUME_REASON_STATE;
    }

    if (txn->request_type != OTA_CTRL_REQ_TYPE_UPGRADE)
    {
        return OTA_CTRL_RESUME_REASON_REQ_TYPE;
    }

    if (txn->active_slot != boot_info->active_partition)
    {
        return OTA_CTRL_RESUME_REASON_ACTIVE_SLOT;
    }

    if (txn->target_slot != boot_info->target_partition)
    {
        return OTA_CTRL_RESUME_REASON_TARGET_SLOT;
    }

    if (txn->protocol_version != OTA_CTRL_PROTOCOL_VERSION)
    {
        return OTA_CTRL_RESUME_REASON_PROTOCOL;
    }

    if (strcmp(txn->current_version, boot_info->current_version) != 0)
    {
        return OTA_CTRL_RESUME_REASON_CURRENT_VERSION;
    }

    if (strcmp(txn->target_version, ready_info->version) != 0)
    {
        return OTA_CTRL_RESUME_REASON_TARGET_VERSION;
    }

    if (txn->transfer_total_size != ready_info->transfer_size)
    {
        return OTA_CTRL_RESUME_REASON_TRANSFER_SIZE;
    }

    if (txn->plain_size != ready_info->plain_size)
    {
        return OTA_CTRL_RESUME_REASON_PLAIN_SIZE;
    }

    if (txn->checkpoint_size == 0U ||
        ready_info->checkpoint_size == 0U ||
        txn->checkpoint_size != ready_info->checkpoint_size)
    {
        return OTA_CTRL_RESUME_REASON_CHECKPOINT_SIZE;
    }

    if (memcmp(txn->session_fingerprint,
               ready_info->session_fingerprint,
               OTA_CTRL_FINGERPRINT_LEN) != 0)
    {
        return OTA_CTRL_RESUME_REASON_FINGERPRINT;
    }

    resume_offset = iap_resume_effective_offset(txn, ready_info);
    if (resume_offset == 0U)
    {
        return OTA_CTRL_RESUME_REASON_OFFSET_ZERO;
    }

    if (resume_offset >= ready_info->transfer_size)
    {
        return OTA_CTRL_RESUME_REASON_OFFSET_RANGE;
    }

    if (txn->chunk_size == 0U ||
        (resume_offset % txn->chunk_size) != 0U)
    {
        return OTA_CTRL_RESUME_REASON_OFFSET_CHECKPOINT;
    }

    if ((resume_offset % 16U) != 0U)
    {
        return OTA_CTRL_RESUME_REASON_OFFSET_BLOCK;
    }

    return OTA_CTRL_RESUME_REASON_OK;
}

static uint8_t iap_checkpoint_save(uint32_t durable_offset, void *context)
{
    iap_checkpoint_context_t *checkpoint_context = (iap_checkpoint_context_t *)context;
    OtaTxnRecord *txn = 0;

    if (checkpoint_context == 0 || checkpoint_context->txn == 0)
    {
        return 0U;
    }

    txn = checkpoint_context->txn;
    txn->resume_offset = durable_offset;
    txn->last_acked_offset = durable_offset;
    txn->state = IAP_TXN_STATE_PAUSED_RESUMABLE;
    txn->pause_reason = IAP_TXN_PAUSE_RETRYABLE;
    txn->last_error_stage = OTA_CTRL_STAGE_TRANSFER;
    txn->last_error_code = 0U;
    txn->retryable = 1U;

    if (txn_save(txn) != 0U)
    {
        checkpoint_context->save_failed = 1U;
        return 0U;
    }

    return 1U;
}

static void iap_send_result_best_effort(uint8_t outcome,
                                        uint8_t stage,
                                        uint16_t error_code,
                                        uint32_t final_offset)
{
    (void)ota_ctrl_send_result(outcome, stage, error_code, final_offset);
}

void iap_main(void)
{
    int32_t result = 0;
    BootInfoTypeDef boot_info;
    BootInfoTypeDef current_boot_info;
    OtaTxnRecord txn;
    iap_txn_load_diag_t txn_load_diag;
    ota_ctrl_ready_info_t ready_info;
    OtaImageHeaderBinary meta_header;
    const OtaImageHeaderBinary *received_header = 0;
    iap_header_validation_context_t header_validation;
    iap_checkpoint_context_t checkpoint_context;
    iap_auth_result_t auth_result = IAP_AUTH_RESULT_OK;
    iap_auth_diag_t auth_diag;
    uint32_t target_address = 0U;
    uint32_t resume_offset = 0U;
    uint16_t reject_reason = 0U;
    uint16_t resume_reason = OTA_CTRL_RESUME_REASON_OK;
    uint8_t go_flags = 0U;
    uint8_t same_request_resume = 0U;
    uint8_t err_code = 0U;
    uint8_t err_stage = 0U;
    uint8_t terminal_error = 0U;
    uint32_t uart_error_flags = 0U;
    uint32_t final_offset = 0U;

    iap_ui_show_upgrade_prepare();

    boot_info_load(&boot_info);
    iap_show_version_lines(&boot_info);
    txn_load_with_diag(&txn, &txn_load_diag);
    iap_serial_report_txn_load(&txn, &txn_load_diag);
    current_boot_info = boot_info;

    if (boot_info.target_partition > OTA_CTRL_PARTITION_APP2 ||
        boot_info.target_partition == boot_info.active_partition)
    {
        boot_info.target_partition = boot_info_inactive_partition(boot_info.active_partition);
    }

    target_address = boot_info_partition_address(boot_info.target_partition);
    APPLICATION_ADDRESS = target_address;

    boot_info.boot_magic = MAGIC_NORMAL;
    boot_info.upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
    if (boot_info_save(&boot_info) != 0U)
    {
        iap_serial_report_code("BOOT");
        iap_return_to_active_app(&current_boot_info, "BootInfo save", "Run current APP");
        return;
    }
    current_boot_info = boot_info;

    if (!ota_ctrl_wait_for_upgrade_ready(&boot_info,
                                         &ready_info,
                                         &reject_reason,
                                         OTA_CTRL_REQ_FLAG_BASE))
    {
        if (reject_reason == OTA_CTRL_ERR_NO_UPDATE)
        {
            txn_clear();
            iap_reset_request_and_return_active_app(&boot_info,
                                                    "Already latest",
                                                    ota_ctrl_error_name(reject_reason));
        }
        else if (reject_reason != 0U)
        {
            iap_reset_request_and_return_active_app(&boot_info,
                                                    "ESP32 rejected",
                                                    ota_ctrl_error_name(reject_reason));
        }
        else
        {
            iap_reset_request_and_return_active_app(&boot_info,
                                                    "ESP32 timeout",
                                                    "Run current APP");
        }
        return;
    }

    iap_send_txn_load_diag(&txn, &txn_load_diag);
    resume_reason = iap_resume_match_reason(&txn, &boot_info, &ready_info);
    same_request_resume = (resume_reason == OTA_CTRL_RESUME_REASON_OK) ? 1U : 0U;
    resume_offset = (same_request_resume != 0U) ?
                        iap_resume_effective_offset(&txn, &ready_info) :
                        0U;
    if (same_request_resume != 0U && resume_offset != 0U)
    {
        txn.resume_offset = resume_offset;
        if (txn.last_acked_offset < resume_offset)
        {
            txn.last_acked_offset = resume_offset;
        }
    }
    iap_show_resume_decision(same_request_resume,
                             resume_reason,
                             resume_offset,
                             ready_info.transfer_size);
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY,
                               OTA_CTRL_PERCENT_UNKNOWN,
                               resume_reason,
                               resume_offset,
                               ready_info.transfer_size);
    if (same_request_resume == 0U)
    {
        iap_txn_begin_new(&txn, &boot_info);
        iap_txn_reset_transfer_info(&txn);
    }

    version_text_copy(txn.target_version, BOOT_INFO_VERSION_LEN, ready_info.version);
    txn.transfer_total_size = ready_info.transfer_size;
    txn.plain_size = ready_info.plain_size;
    txn.protocol_version = OTA_CTRL_PROTOCOL_VERSION;
    txn.chunk_size = OTA_DATA_DEFAULT_CHUNK_SIZE;
    txn.checkpoint_size = (ready_info.checkpoint_size != 0U) ?
                              ready_info.checkpoint_size :
                              OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    memcpy(txn.session_fingerprint,
           ready_info.session_fingerprint,
           OTA_CTRL_FINGERPRINT_LEN);
    txn.last_error_stage = 0U;
    txn.last_error_code = 0U;
    txn.retryable = 1U;
    txn.pause_reason = IAP_TXN_PAUSE_NONE;

    if (same_request_resume != 0U)
    {
        go_flags = OTA_CTRL_GO_FLAG_RESUME_REQUESTED;
    }
    else
    {
        resume_offset = 0U;
        txn.resume_offset = 0U;
        txn.last_acked_offset = 0U;
    }

    if (txn_compact_with_boot_info(&boot_info, &txn) != 0U)
    {
        iap_serial_report_code("TXN CP");
        iap_return_to_active_app(&current_boot_info, "Txn compact", "Run current APP");
        return;
    }

    if (iap_txn_store_state(&txn,
                            IAP_TXN_STATE_NEGOTIATING,
                            OTA_CTRL_STAGE_READY,
                            0U,
                            1U) == 0U)
    {
        iap_serial_report_code("TXN");
        iap_return_to_active_app(&current_boot_info, "Txn save", "Run current APP");
        return;
    }

    if (ota_ctrl_send_go((uint8_t)boot_info.target_partition,
                         (uint16_t)go_flags,
                         resume_offset) == 0U)
    {
        (void)iap_txn_store_state(&txn,
                                  IAP_TXN_STATE_FAILED_RETRYABLE,
                                  OTA_CTRL_STAGE_READY,
                                  OTA_CTRL_ERR_PROTOCOL,
                                  1U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                    OTA_CTRL_STAGE_READY,
                                    OTA_CTRL_ERR_PROTOCOL,
                                    resume_offset);
        iap_serial_report_code("GO");
        iap_reset_request_and_return_active_app(&boot_info, "GO failed", "Run current APP");
        return;
    }

    if (ota_ctrl_wait_for_meta_image_header(&meta_header, 10000U) == 0U)
    {
        (void)iap_txn_store_state(&txn,
                                  IAP_TXN_STATE_FAILED_RETRYABLE,
                                  OTA_CTRL_STAGE_READY,
                                  YMODEM_ERR_HEADER,
                                  1U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                    OTA_CTRL_STAGE_READY,
                                    YMODEM_ERR_HEADER,
                                    resume_offset);
        iap_serial_report_code("HDR");
        iap_reset_request_and_return_active_app(&boot_info, "META timeout", "Run current APP");
        return;
    }

    header_validation.boot_info = &boot_info;
    header_validation.error_code = YMODEM_OK;
    if (iap_validate_received_header(&meta_header, &header_validation) == 0U)
    {
        err_code = (header_validation.error_code != YMODEM_OK) ?
                       header_validation.error_code :
                       YMODEM_ERR_HEADER;
        (void)iap_txn_store_state(&txn,
                                  IAP_TXN_STATE_FAILED_TERMINAL,
                                  OTA_CTRL_STAGE_READY,
                                  err_code,
                                  0U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                    OTA_CTRL_STAGE_READY,
                                    err_code,
                                    resume_offset);
        iap_serial_report_code((err_code == YMODEM_ERR_SLOT) ? "YMD SLOT" :
                               (err_code == YMODEM_ERR_VERSION) ? "YMD VER" :
                               "YMD HDR");
        iap_reset_request_and_return_active_app(&current_boot_info,
                                                "Header invalid",
                                                "Run current APP");
        return;
    }

    if (iap_verify_image_header_signature(&meta_header) == 0U)
    {
        (void)iap_txn_store_state(&txn,
                                  IAP_TXN_STATE_FAILED_TERMINAL,
                                  OTA_CTRL_STAGE_VERIFY_SIG,
                                  YMODEM_ERR_AUTH,
                                  0U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                    OTA_CTRL_STAGE_VERIFY_SIG,
                                    YMODEM_ERR_AUTH,
                                    resume_offset);
        iap_serial_report_code("AUTH RSA");
        iap_reset_request_and_return_active_app(&current_boot_info,
                                                "Header RSA bad",
                                                "Run current APP");
        return;
    }

    if (iap_txn_store_state(&txn,
                            IAP_TXN_STATE_HEADER_VERIFIED,
                            OTA_CTRL_STAGE_READY,
                            0U,
                            1U) == 0U)
    {
        iap_serial_report_code("TXN");
        iap_return_to_active_app(&current_boot_info, "Txn save", "Run current APP");
        return;
    }

    checkpoint_context.txn = &txn;
    checkpoint_context.save_failed = 0U;
    Ymodem_SetHeaderValidator(iap_validate_received_header, &header_validation);
    if (Ymodem_ConfigureTransfer(&meta_header,
                                 ready_info.session_fingerprint,
                                 resume_offset,
                                 txn.checkpoint_size,
                                 iap_checkpoint_save,
                                 &checkpoint_context) == 0U)
    {
        Ymodem_SetHeaderValidator(0, 0);
        err_code = Ymodem_GetErrorCode();
        terminal_error = iap_transfer_error_is_terminal(err_code);
        (void)iap_txn_store_state(&txn,
                                  (terminal_error != 0U) ?
                                      IAP_TXN_STATE_FAILED_TERMINAL :
                                      IAP_TXN_STATE_FAILED_RETRYABLE,
                                  OTA_CTRL_STAGE_READY,
                                  err_code,
                                  (terminal_error == 0U) ? 1U : 0U);
        iap_send_result_best_effort((terminal_error != 0U) ?
                                        OTA_CTRL_RESULT_OUTCOME_TERMINAL :
                                        OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                    OTA_CTRL_STAGE_READY,
                                    err_code,
                                    resume_offset);
        iap_serial_report_code("HDR");
        iap_reset_request_and_return_active_app(&current_boot_info,
                                                "Transfer config",
                                                "Run current APP");
        return;
    }
    Ymodem_SetHeaderValidator(0, 0);

    if (iap_txn_store_state(&txn,
                            IAP_TXN_STATE_TRANSFERRING,
                            OTA_CTRL_STAGE_TRANSFER,
                            0U,
                            1U) == 0U)
    {
        iap_serial_report_code("TXN");
        iap_return_to_active_app(&current_boot_info, "Txn save", "Run current APP");
        return;
    }

    ota_ctrl_show_stage(OTA_CTRL_STAGE_TRANSFER, OTA_CTRL_PERCENT_UNKNOWN, 0U, 0U, 0U);
    iap_reset_ymodem_progress();
    Ymodem_SetProgressCallback(iap_show_ymodem_progress);
    iap_show_ymodem_progress(resume_offset, ready_info.transfer_size);
    iap_init_watchdog();
    iap_feed_watchdog();
    result = Ymodem_Receive(&tab_1024[0]);
    iap_feed_watchdog();
    Ymodem_SetProgressCallback(0);

    if (result > 0)
    {
        txn.resume_offset = 0U;
        txn.last_acked_offset = (uint32_t)result;
        txn.pause_reason = IAP_TXN_PAUSE_NONE;
        if (iap_txn_store_state(&txn,
                                IAP_TXN_STATE_RECEIVED,
                                OTA_CTRL_STAGE_TRANSFER,
                                0U,
                                0U) == 0U)
        {
            iap_serial_report_code("TXN");
            iap_return_to_active_app(&current_boot_info, "Txn save", "Run current APP");
            return;
        }

        received_header = Ymodem_GetReceivedHeader();
        if (received_header == 0 ||
            received_header->payload.firmware_size != (uint32_t)result ||
            strcmp(ready_info.version, received_header->payload.firmware_version) != 0)
        {
            iap_txn_reset_transfer_info(&txn);
            (void)iap_txn_store_state(&txn,
                                      IAP_TXN_STATE_FAILED_TERMINAL,
                                      OTA_CTRL_STAGE_TRANSFER,
                                      YMODEM_ERR_VERSION,
                                      0U);
            iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                        OTA_CTRL_STAGE_TRANSFER,
                                        YMODEM_ERR_VERSION,
                                        (uint32_t)result);
            iap_serial_report_code("HDR");
            iap_reset_request_and_return_active_app(&current_boot_info,
                                                    "Header mismatch",
                                                    "Run current APP");
            return;
        }

        auth_result = iap_authorize_received_image(&boot_info,
                                                   received_header,
                                                   target_address,
                                                   &auth_diag);
        if (auth_result != IAP_AUTH_RESULT_OK)
        {
            iap_txn_reset_transfer_info(&txn);
            (void)iap_txn_store_state(&txn,
                                      IAP_TXN_STATE_FAILED_TERMINAL,
                                      OTA_CTRL_STAGE_VERIFY_SIG,
                                      (uint32_t)auth_result,
                                      0U);
            iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                        OTA_CTRL_STAGE_VERIFY_SIG,
                                        (uint16_t)auth_result,
                                        (uint32_t)result);
            iap_serial_report_auth_failure(auth_result, &auth_diag);
            iap_reset_request_and_return_active_app(&current_boot_info,
                                                    "Auth failed",
                                                    iap_auth_result_text(auth_result, &auth_diag));
            return;
        }

        if (iap_txn_store_state(&txn,
                                IAP_TXN_STATE_AUTHORIZED,
                                OTA_CTRL_STAGE_VERIFY_SIG,
                                0U,
                                0U) == 0U)
        {
            iap_serial_report_code("TXN");
            iap_return_to_active_app(&current_boot_info, "Txn save", "Run current APP");
            return;
        }

        boot_info_mark_pending_install(&boot_info, &received_header->payload);
        if (boot_info_save(&boot_info) != 0U)
        {
            iap_serial_report_code("BOOT");
            iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                        OTA_CTRL_STAGE_DONE,
                                        OTA_CTRL_ERR_PROTOCOL,
                                        (uint32_t)result);
            iap_reset_request_and_return_active_app(&current_boot_info,
                                                    "BootInfo save",
                                                    "Run current APP");
            return;
        }

        iap_txn_reset_transfer_info(&txn);
        (void)iap_txn_store_state(&txn,
                                  IAP_TXN_STATE_COMMITTED,
                                  OTA_CTRL_STAGE_DONE,
                                  0U,
                                  0U);

        iap_ui_show_upgrade_success();
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_SUCCESS,
                                    OTA_CTRL_STAGE_DONE,
                                    0U,
                                    (uint32_t)result);
        iap_serial_report_code("OK");
        delay_ms(800);

        if (iap_try_jump_partition(boot_info.active_partition) != 0U)
        {
            while (1)
            {
            }
        }

        (void)iap_txn_store_state(&txn,
                                  IAP_TXN_STATE_FAILED_TERMINAL,
                                  OTA_CTRL_STAGE_DONE,
                                  OTA_CTRL_ERR_PROTOCOL,
                                  0U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                    OTA_CTRL_STAGE_DONE,
                                    OTA_CTRL_ERR_PROTOCOL,
                                    (uint32_t)result);
        iap_serial_report_code("APPBAD");
        iap_reset_request_and_return_active_app(&current_boot_info,
                                                "New APP bad",
                                                "Run current APP");
        return;
    }

    err_code = Ymodem_GetErrorCode();
    err_stage = Ymodem_GetErrorStage();
    uart_error_flags = Ymodem_GetUartErrorFlags();
    final_offset = Ymodem_GetLastAckedOffset();
    terminal_error = iap_transfer_error_is_terminal(err_code);

    switch (err_code)
    {
    case YMODEM_ERR_TIMEOUT:
        iap_serial_report_code("YMD TIME");
        break;
    case YMODEM_ERR_SEQ:
        iap_serial_report_code("YMD SEQ");
        break;
    case YMODEM_ERR_CRC:
        iap_serial_report_code("YMD CRC");
        break;
    case YMODEM_ERR_SIZE:
        iap_serial_report_code("YMD SIZE");
        break;
    case YMODEM_ERR_FLASH:
        iap_serial_report_code("YMD FLASH");
        break;
    case YMODEM_ERR_ABORT:
        iap_serial_report_code("YMD ABORT");
        break;
    case YMODEM_ERR_MAX_RETRIES:
        iap_serial_report_code("YMD RETRY");
        break;
    case YMODEM_ERR_HEADER:
        iap_serial_report_code("YMD HDR");
        break;
    case YMODEM_ERR_AUTH:
        iap_serial_report_code("YMD AUTH");
        break;
    case YMODEM_ERR_VERSION:
        iap_serial_report_code("YMD VER");
        break;
    case YMODEM_ERR_SLOT:
        iap_serial_report_code("YMD SLOT");
        break;
    case YMODEM_ERR_UART:
        iap_serial_report_code("YMD UART");
        iap_serial_report_uart_flags(uart_error_flags);
        break;
    default:
        iap_serial_report_code("YMD FAIL");
        break;
    }

    iap_ui_show_upgrade_failure();

    txn.last_acked_offset = final_offset;
    txn.last_error_stage = err_stage;
    txn.last_error_code = err_code;
    if (terminal_error != 0U)
    {
        iap_txn_reset_transfer_info(&txn);
        txn.pause_reason = IAP_TXN_PAUSE_TERMINAL;
        txn.state = IAP_TXN_STATE_FAILED_TERMINAL;
        txn.retryable = 0U;
    }
    else
    {
        txn.state = IAP_TXN_STATE_PAUSED_RESUMABLE;
        txn.pause_reason = IAP_TXN_PAUSE_RETRYABLE;
        txn.retryable = 1U;
    }
    (void)txn_save(&txn);

    iap_send_result_best_effort((terminal_error != 0U) ?
                                    OTA_CTRL_RESULT_OUTCOME_TERMINAL :
                                    OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                (err_stage != 0U) ? err_stage : OTA_CTRL_STAGE_TRANSFER,
                                err_code,
                                final_offset);

    delay_ms(800);
    iap_reset_request_and_return_active_app(&boot_info, "Upgrade failed", "Run current APP");
}

void iap_boot_entry(void)
{
    iap_boot_entry_run();
}
