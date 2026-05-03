/**
 * @file    iap.c
 * @brief   IAP 核心模块 —— OTA 升级主流程
 * @note    本模块实现 bootloader 的 OTA 升级核心逻辑，包括：
 *          - 与 ESP32 主机的升级协商
 *          - 断点续传决策（resume match）
 *          - Ymodem 固件传输
 *          - 固件完整性验证（SHA-256 + RSA-2048）
 *          - BootInfo 更新与应用跳转
 *          - 事务记录的全生命周期管理
 *
 * @par 升级流程
 *      1. 加载 BootInfo 和事务记录，发送诊断信息
 *      2. 清除升级请求标志，等待 ESP32 就绪
 *      3. 断点续传匹配（resume match）
 *      4. 发送 GO 命令，接收 META 镜像头部
 *      5. 头部校验 + RSA 签名验证
 *      6. 配置 Ymodem 传输参数（含检查点回调）
 *      7. 执行 Ymodem 固件传输
 *      8. 传输完成 → SHA-256 + RSA 完整认证
 *      9. 认证通过 → 标记待安装、保存 BootInfo、跳转应用
 *      10. 传输失败 → 根据错误类型决定可重试/不可重试
 *
 * @par 事务状态机
 *      IDLE → NEGOTIATING → HEADER_VERIFIED → TRANSFERRING → RECEIVED
 *      → AUTHORIZED → COMMITTED
 *      任何阶段失败 → FAILED_RETRYABLE / FAILED_TERMINAL
 *      暂停状态 → PAUSED_RESUMABLE
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "iap_boot_info.h"
#include "iap_boot_entry.h"
#include "iap_auth.h"
#include "iap_ctrl.h"
#include "iap_ui.h"
#include "iap_platform.h"

/* =========================================================================
 *  2. 模块级变量
 * ======================================================================= */

/** Ymodem 传输缓冲区（1024 字节） */
uint8_t tab_1024[1024] = {0};

/** 检查点保存上下文 */
typedef struct
{
    OtaTxnRecord *txn;          /**< 事务记录指针             */
    uint8_t save_failed;        /**< 保存失败标志             */
} iap_checkpoint_context_t;

/* =========================================================================
 *  3. 内部函数实现 —— 诊断信息发送
 * ======================================================================= */

/**
 * @brief  通过 OTA 控制通道发送事务加载诊断信息
 * @note   分多条状态消息发送：核心信息、偏移量、元数据、无效槽位详情。
 * @param  txn  — 事务记录
 * @param  diag — 诊断信息
 */
static void iap_send_txn_load_diag(const OtaTxnRecord *txn, const iap_txn_load_diag_t *diag)
{
    uint16_t detail_code = 0U;

    if (txn == 0 || diag == 0)
    {
        return;
    }

    /* 发送核心信息：状态 + 来源 */
    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_CORE | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code, txn->state, txn->protocol_version);

    /* 发送偏移量信息 */
    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_OFFSETS | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code, txn->resume_offset, txn->last_acked_offset);

    /* 发送元数据信息 */
    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_META | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code, txn->checkpoint_size, diag->invalid_slots);

    /* 若无无效槽位，结束 */
    if (diag->invalid_slots == 0U)
    {
        return;
    }

    /* 发送无效槽位详情（4 组 × 2 字段） */
    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT1 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code, diag->invalid_slot_header, diag->invalid_slot_commit);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT2 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code, diag->invalid_slot_payload_crc, diag->invalid_slot_payload_validate);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_TXN1 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code, diag->invalid_txn_layout, diag->invalid_txn_state);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_TXN2 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code, diag->invalid_txn_partition, diag->invalid_txn_fields);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_TXN3 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code, diag->invalid_txn_request_type, diag->invalid_txn_version_text);

    detail_code = (uint16_t)(OTA_CTRL_DIAG_TXN_LOAD_INV_TXN4 | (diag->source & 0x000FU));
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               detail_code, diag->invalid_txn_offset, diag->invalid_txn_data_crc);
}

/* =========================================================================
 *  4. 内部函数实现 —— 应用跳转辅助
 * ======================================================================= */

/**
 * @brief  尝试跳转到指定分区的应用
 * @param  partition — 分区编号
 * @retval 1 — 跳转成功（不会返回）；0 — 应用无效
 */
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

/**
 * @brief  返回活跃应用（延迟 200ms 后跳转）
 * @param  boot_info — BootInfo 指针
 * @param  line1     — 显示行 1（未使用，保留接口兼容）
 * @param  line2     — 显示行 2（未使用，保留接口兼容）
 */
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

/**
 * @brief  重置升级请求并返回活跃应用
 * @note   清除 boot_magic 和 upgrade_flag，保存 BootInfo，
 *         显示失败信息后跳转到活跃应用并复位。
 * @param  boot_info — BootInfo 指针
 * @param  line1     — 显示行 1（未使用）
 * @param  line2     — 显示行 2（未使用）
 */
static void iap_reset_request_and_return_active_app(BootInfoTypeDef *boot_info,
                                                    const char *line1,
                                                    const char *line2)
{
    if (boot_info != 0)
    {
        boot_info->boot_magic   = MAGIC_NORMAL;
        boot_info->upgrade_flag = BOOT_UPGRADE_FLAG_NONE;

        /* 非试启动状态下更新已确认分区和版本 */
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

/* =========================================================================
 *  5. 内部函数实现 —— 事务记录操作
 * ======================================================================= */

/**
 * @brief  计算下一个事务计数器值
 * @param  current — 当前计数器
 * @return 下一个计数器（跳过 0 和 0xFFFFFFFF）
 */
static uint32_t iap_txn_next_counter(uint32_t current)
{
    uint32_t next = current + 1U;

    if (next == 0U || next == 0xFFFFFFFFUL)
    {
        next = 1U;
    }

    return next;
}

/**
 * @brief  重置事务传输信息
 * @param  txn — 事务记录指针
 */
static void iap_txn_reset_transfer_info(OtaTxnRecord *txn)
{
    if (txn == 0)
    {
        return;
    }

    txn->transfer_total_size = 0U;
    txn->plain_size          = 0U;
    txn->resume_offset       = 0U;
    txn->last_acked_offset   = 0U;
    txn->pause_reason        = IAP_TXN_PAUSE_NONE;
    memset(txn->session_fingerprint, 0, sizeof(txn->session_fingerprint));
}

/**
 * @brief  初始化新事务
 * @param  txn       — 事务记录指针
 * @param  boot_info — BootInfo 指针
 */
static void iap_txn_begin_new(OtaTxnRecord *txn, const BootInfoTypeDef *boot_info)
{
    uint32_t previous_counter = 0U;

    if (txn == 0 || boot_info == 0)
    {
        return;
    }

    previous_counter = txn->txn_counter;
    txn_init_default(txn);
    txn->txn_counter     = iap_txn_next_counter(previous_counter);
    txn->state           = IAP_TXN_STATE_NEGOTIATING;
    txn->request_type    = OTA_CTRL_REQ_TYPE_UPGRADE;
    txn->active_slot     = boot_info->active_partition;
    txn->target_slot     = boot_info->target_partition;
    version_text_copy(txn->current_version, BOOT_INFO_VERSION_LEN, boot_info->current_version);
    version_text_copy(txn->target_version,  BOOT_INFO_VERSION_LEN, IAP_DEFAULT_VERSION);
    txn->retryable       = 1U;
    txn->protocol_version = OTA_CTRL_PROTOCOL_VERSION;
    txn->checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    txn->chunk_size      = OTA_DATA_DEFAULT_CHUNK_SIZE;
}

/* =========================================================================
 *  6. 内部函数实现 —— 断点续传
 * ======================================================================= */

/**
 * @brief  计算有效的续传偏移量
 * @note   若事务状态为 PAUSED_RESUMABLE 或 FAILED_RETRYABLE，
 *         且 last_acked_offset 满足对齐要求，则返回续传偏移量。
 * @param  txn       — 事务记录指针
 * @param  ready_info — ESP32 就绪信息
 * @return 有效续传偏移量（0 表示不续传）
 */
static uint32_t iap_resume_effective_offset(const OtaTxnRecord *txn,
                                            const ota_ctrl_ready_info_t *ready_info)
{
    uint32_t resume_offset = 0U;

    if (txn == 0 || ready_info == 0)
    {
        return 0U;
    }

    /* 优先使用已有的 resume_offset */
    resume_offset = txn->resume_offset;
    if (resume_offset != 0U)
    {
        return resume_offset;
    }

    /* 从 last_acked_offset 推导 */
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

    /* 检查对齐：必须是 chunk_size 和 16 字节的整数倍 */
    if (txn->chunk_size == 0U ||
        (resume_offset % txn->chunk_size) != 0U ||
        (resume_offset % 16U) != 0U)
    {
        return 0U;
    }

    return resume_offset;
}

/**
 * @brief  保存事务状态到日志区
 * @param  txn         — 事务记录指针
 * @param  state       — 新状态
 * @param  error_stage — 错误阶段
 * @param  error_code  — 错误码
 * @param  retryable   — 是否可重试
 * @retval 1 — 保存成功；0 — 保存失败
 */
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

    txn->state            = state;
    txn->last_error_stage = error_stage;
    txn->last_error_code  = error_code;
    txn->retryable        = (retryable != 0U) ? 1U : 0U;
    return (txn_save(txn) == 0U) ? 1U : 0U;
}

/**
 * @brief  判断传输错误是否为不可重试的终端错误
 * @param  err_code — YMODEM 错误码
 * @retval 1 — 终端错误；0 — 可重试
 */
static uint8_t iap_transfer_error_is_terminal(uint8_t err_code)
{
    return (err_code == YMODEM_ERR_HEADER ||
            err_code == YMODEM_ERR_AUTH ||
            err_code == YMODEM_ERR_VERSION ||
            err_code == YMODEM_ERR_SLOT) ? 1U : 0U;
}

/**
 * @brief  断点续传匹配检查
 * @note   逐项比对事务记录与当前 ESP32 就绪信息，
 *         任何不匹配返回具体原因码。
 * @param  txn       — 事务记录指针
 * @param  boot_info — BootInfo 指针
 * @param  ready_info — ESP32 就绪信息
 * @return 匹配结果（OTA_CTRL_RESUME_REASON_xxx）
 */
static uint16_t iap_resume_match_reason(const OtaTxnRecord *txn,
                                        const BootInfoTypeDef *boot_info,
                                        const ota_ctrl_ready_info_t *ready_info)
{
    uint32_t resume_offset = 0U;

    if (txn == 0 || boot_info == 0 || ready_info == 0)
    {
        return OTA_CTRL_RESUME_REASON_STATE;
    }

    /* 检查事务状态 */
    if (txn->state != IAP_TXN_STATE_PAUSED_RESUMABLE &&
        txn->state != IAP_TXN_STATE_FAILED_RETRYABLE)
    {
        return OTA_CTRL_RESUME_REASON_STATE;
    }

    /* 检查请求类型 */
    if (txn->request_type != OTA_CTRL_REQ_TYPE_UPGRADE)
    {
        return OTA_CTRL_RESUME_REASON_REQ_TYPE;
    }

    /* 检查分区一致性 */
    if (txn->active_slot != boot_info->active_partition)
    {
        return OTA_CTRL_RESUME_REASON_ACTIVE_SLOT;
    }

    if (txn->target_slot != boot_info->target_partition)
    {
        return OTA_CTRL_RESUME_REASON_TARGET_SLOT;
    }

    /* 检查协议版本 */
    if (txn->protocol_version != OTA_CTRL_PROTOCOL_VERSION)
    {
        return OTA_CTRL_RESUME_REASON_PROTOCOL;
    }

    /* 检查版本一致性 */
    if (strcmp(txn->current_version, boot_info->current_version) != 0)
    {
        return OTA_CTRL_RESUME_REASON_CURRENT_VERSION;
    }

    if (strcmp(txn->target_version, ready_info->version) != 0)
    {
        return OTA_CTRL_RESUME_REASON_TARGET_VERSION;
    }

    /* 检查传输大小一致性 */
    if (txn->transfer_total_size != ready_info->transfer_size)
    {
        return OTA_CTRL_RESUME_REASON_TRANSFER_SIZE;
    }

    if (txn->plain_size != ready_info->plain_size)
    {
        return OTA_CTRL_RESUME_REASON_PLAIN_SIZE;
    }

    /* 检查检查点大小 */
    if (txn->checkpoint_size == 0U ||
        ready_info->checkpoint_size == 0U ||
        txn->checkpoint_size != ready_info->checkpoint_size)
    {
        return OTA_CTRL_RESUME_REASON_CHECKPOINT_SIZE;
    }

    /* 检查会话指纹 */
    if (memcmp(txn->session_fingerprint,
               ready_info->session_fingerprint,
               OTA_CTRL_FINGERPRINT_LEN) != 0)
    {
        return OTA_CTRL_RESUME_REASON_FINGERPRINT;
    }

    /* 检查续传偏移量 */
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

/* =========================================================================
 *  7. 内部函数实现 —— 检查点回调
 * ======================================================================= */

/**
 * @brief  Ymodem 检查点保存回调
 * @note   每传输一个 checkpoint_size 字节后调用，
 *         将当前进度持久化到事务日志区。
 * @param  durable_offset — 已确认的偏移量
 * @param  context        — iap_checkpoint_context_t 指针
 * @retval 1 — 保存成功；0 — 保存失败
 */
static uint8_t iap_checkpoint_save(uint32_t durable_offset, void *context)
{
    iap_checkpoint_context_t *checkpoint_context = (iap_checkpoint_context_t *)context;
    OtaTxnRecord *txn = 0;

    if (checkpoint_context == 0 || checkpoint_context->txn == 0)
    {
        return 0U;
    }

    txn = checkpoint_context->txn;
    txn->resume_offset     = durable_offset;
    txn->last_acked_offset = durable_offset;
    txn->state             = IAP_TXN_STATE_PAUSED_RESUMABLE;
    txn->pause_reason      = IAP_TXN_PAUSE_RETRYABLE;
    txn->last_error_stage  = OTA_CTRL_STAGE_TRANSFER;
    txn->last_error_code   = 0U;
    txn->retryable         = 1U;

    if (txn_save(txn) != 0U)
    {
        checkpoint_context->save_failed = 1U;
        return 0U;
    }

    return 1U;
}

/**
 * @brief  尽力发送升级结果（忽略发送失败）
 * @param  outcome      — 结果类型
 * @param  stage        — 阶段
 * @param  error_code   — 错误码
 * @param  final_offset — 最终偏移量
 */
static void iap_send_result_best_effort(uint8_t outcome,
                                        uint8_t stage,
                                        uint16_t error_code,
                                        uint32_t final_offset)
{
    (void)ota_ctrl_send_result(outcome, stage, error_code, final_offset);
}

/* =========================================================================
 *  8. 公共接口实现 —— IAP 升级主流程
 * ======================================================================= */

/**
 * @brief  IAP 升级主函数
 * @note   完整的 OTA 升级流程，从协商到传输到认证到跳转。
 *         任何步骤失败都会尝试安全回退到当前活跃应用。
 */
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

    /* --- 步骤 1：初始化 --- */
    iap_ui_show_upgrade_prepare();

    boot_info_load(&boot_info);
    iap_show_version_lines(&boot_info);
    txn_load_with_diag(&txn, &txn_load_diag);
    iap_serial_report_txn_load(&txn, &txn_load_diag);
    current_boot_info = boot_info;

    /* 校验目标分区 */
    if (boot_info.target_partition > OTA_CTRL_PARTITION_APP2 ||
        boot_info.target_partition == boot_info.active_partition)
    {
        boot_info.target_partition = boot_info_inactive_partition(boot_info.active_partition);
    }

    target_address = boot_info_partition_address(boot_info.target_partition);
    APPLICATION_ADDRESS = target_address;

    /* --- 步骤 2：清除升级请求标志 --- */
    boot_info.boot_magic   = MAGIC_NORMAL;
    boot_info.upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
    if (boot_info_save(&boot_info) != 0U)
    {
        iap_serial_report_code("BOOT");
        iap_return_to_active_app(&current_boot_info, "BootInfo save", "Run current APP");
        return;
    }
    current_boot_info = boot_info;

    /* --- 步骤 3：等待 ESP32 升级就绪 --- */
    if (!ota_ctrl_wait_for_upgrade_ready(&boot_info, &ready_info, &reject_reason, OTA_CTRL_REQ_FLAG_BASE))
    {
        if (reject_reason == OTA_CTRL_ERR_NO_UPDATE)
        {
            txn_clear();
            iap_reset_request_and_return_active_app(&boot_info, "Already latest",
                                                    ota_ctrl_error_name(reject_reason));
        }
        else if (reject_reason != 0U)
        {
            iap_reset_request_and_return_active_app(&boot_info, "ESP32 rejected",
                                                    ota_ctrl_error_name(reject_reason));
        }
        else
        {
            iap_reset_request_and_return_active_app(&boot_info, "ESP32 timeout", "Run current APP");
        }
        return;
    }

    /* --- 步骤 4：断点续传决策 --- */
    iap_send_txn_load_diag(&txn, &txn_load_diag);
    resume_reason = iap_resume_match_reason(&txn, &boot_info, &ready_info);
    same_request_resume = (resume_reason == OTA_CTRL_RESUME_REASON_OK) ? 1U : 0U;
    resume_offset = (same_request_resume != 0U) ?
                        iap_resume_effective_offset(&txn, &ready_info) : 0U;

    if (same_request_resume != 0U && resume_offset != 0U)
    {
        txn.resume_offset = resume_offset;
        if (txn.last_acked_offset < resume_offset)
        {
            txn.last_acked_offset = resume_offset;
        }
    }

    iap_show_resume_decision(same_request_resume, resume_reason, resume_offset, ready_info.transfer_size);
    (void)ota_ctrl_send_status(OTA_CTRL_STAGE_READY, OTA_CTRL_PERCENT_UNKNOWN,
                               resume_reason, resume_offset, ready_info.transfer_size);

    if (same_request_resume == 0U)
    {
        iap_txn_begin_new(&txn, &boot_info);
        iap_txn_reset_transfer_info(&txn);
    }

    /* --- 步骤 5：更新事务参数 --- */
    version_text_copy(txn.target_version, BOOT_INFO_VERSION_LEN, ready_info.version);
    txn.transfer_total_size = ready_info.transfer_size;
    txn.plain_size          = ready_info.plain_size;
    txn.protocol_version    = OTA_CTRL_PROTOCOL_VERSION;
    txn.chunk_size          = OTA_DATA_DEFAULT_CHUNK_SIZE;
    txn.checkpoint_size     = (ready_info.checkpoint_size != 0U) ?
                                  ready_info.checkpoint_size : OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    memcpy(txn.session_fingerprint, ready_info.session_fingerprint, OTA_CTRL_FINGERPRINT_LEN);
    txn.last_error_stage    = 0U;
    txn.last_error_code     = 0U;
    txn.retryable           = 1U;
    txn.pause_reason        = IAP_TXN_PAUSE_NONE;

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

    /* --- 步骤 6：压缩事务日志 --- */
    if (txn_compact_with_boot_info(&boot_info, &txn) != 0U)
    {
        iap_serial_report_code("TXN CP");
        iap_return_to_active_app(&current_boot_info, "Txn compact", "Run current APP");
        return;
    }

    /* 保存事务状态 */
    if (iap_txn_store_state(&txn, IAP_TXN_STATE_NEGOTIATING, OTA_CTRL_STAGE_READY, 0U, 1U) == 0U)
    {
        iap_serial_report_code("TXN");
        iap_return_to_active_app(&current_boot_info, "Txn save", "Run current APP");
        return;
    }

    /* --- 步骤 7：发送 GO 命令 --- */
    if (ota_ctrl_send_go((uint8_t)boot_info.target_partition, (uint16_t)go_flags, resume_offset) == 0U)
    {
        (void)iap_txn_store_state(&txn, IAP_TXN_STATE_FAILED_RETRYABLE,
                                  OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL, 1U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                    OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PROTOCOL, resume_offset);
        iap_serial_report_code("GO");
        iap_reset_request_and_return_active_app(&boot_info, "GO failed", "Run current APP");
        return;
    }

    /* --- 步骤 8：接收 META 镜像头部 --- */
    if (ota_ctrl_wait_for_meta_image_header(&meta_header, 10000U) == 0U)
    {
        (void)iap_txn_store_state(&txn, IAP_TXN_STATE_FAILED_RETRYABLE,
                                  OTA_CTRL_STAGE_READY, YMODEM_ERR_HEADER, 1U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                    OTA_CTRL_STAGE_READY, YMODEM_ERR_HEADER, resume_offset);
        iap_serial_report_code("HDR");
        iap_reset_request_and_return_active_app(&boot_info, "META timeout", "Run current APP");
        return;
    }

    /* --- 步骤 9：头部校验 --- */
    header_validation.boot_info   = &boot_info;
    header_validation.error_code  = YMODEM_OK;
    if (iap_validate_received_header(&meta_header, &header_validation) == 0U)
    {
        err_code = (header_validation.error_code != YMODEM_OK) ?
                       header_validation.error_code : YMODEM_ERR_HEADER;
        (void)iap_txn_store_state(&txn, IAP_TXN_STATE_FAILED_TERMINAL,
                                  OTA_CTRL_STAGE_READY, err_code, 0U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                    OTA_CTRL_STAGE_READY, err_code, resume_offset);
        iap_serial_report_code((err_code == YMODEM_ERR_SLOT) ? "YMD SLOT" :
                               (err_code == YMODEM_ERR_VERSION) ? "YMD VER" : "YMD HDR");
        iap_reset_request_and_return_active_app(&current_boot_info, "Header invalid", "Run current APP");
        return;
    }

    /* --- 步骤 10：RSA 签名验证 --- */
    if (iap_verify_image_header_signature(&meta_header) == 0U)
    {
        (void)iap_txn_store_state(&txn, IAP_TXN_STATE_FAILED_TERMINAL,
                                  OTA_CTRL_STAGE_VERIFY_SIG, YMODEM_ERR_AUTH, 0U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                    OTA_CTRL_STAGE_VERIFY_SIG, YMODEM_ERR_AUTH, resume_offset);
        iap_serial_report_code("AUTH RSA");
        iap_reset_request_and_return_active_app(&current_boot_info, "Header RSA bad", "Run current APP");
        return;
    }

    /* 保存头部验证通过状态 */
    if (iap_txn_store_state(&txn, IAP_TXN_STATE_HEADER_VERIFIED, OTA_CTRL_STAGE_READY, 0U, 1U) == 0U)
    {
        iap_serial_report_code("TXN");
        iap_return_to_active_app(&current_boot_info, "Txn save", "Run current APP");
        return;
    }

    /* --- 步骤 11：配置 Ymodem 传输 --- */
    checkpoint_context.txn = &txn;
    checkpoint_context.save_failed = 0U;
    Ymodem_SetHeaderValidator(iap_validate_received_header, &header_validation);

    if (Ymodem_ConfigureTransfer(&meta_header, ready_info.session_fingerprint,
                                 resume_offset, txn.checkpoint_size,
                                 iap_checkpoint_save, &checkpoint_context) == 0U)
    {
        Ymodem_SetHeaderValidator(0, 0);
        err_code = Ymodem_GetErrorCode();
        terminal_error = iap_transfer_error_is_terminal(err_code);
        (void)iap_txn_store_state(&txn,
                                  (terminal_error != 0U) ?
                                      IAP_TXN_STATE_FAILED_TERMINAL : IAP_TXN_STATE_FAILED_RETRYABLE,
                                  OTA_CTRL_STAGE_READY, err_code,
                                  (terminal_error == 0U) ? 1U : 0U);
        iap_send_result_best_effort((terminal_error != 0U) ?
                                        OTA_CTRL_RESULT_OUTCOME_TERMINAL : OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                    OTA_CTRL_STAGE_READY, err_code, resume_offset);
        iap_serial_report_code("HDR");
        iap_reset_request_and_return_active_app(&current_boot_info, "Transfer config", "Run current APP");
        return;
    }
    Ymodem_SetHeaderValidator(0, 0);

    /* --- 步骤 12：执行 Ymodem 传输 --- */
    if (iap_txn_store_state(&txn, IAP_TXN_STATE_TRANSFERRING, OTA_CTRL_STAGE_TRANSFER, 0U, 1U) == 0U)
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

    /* --- 步骤 13：传输成功处理 --- */
    if (result > 0)
    {
        txn.resume_offset     = 0U;
        txn.last_acked_offset = (uint32_t)result;
        txn.pause_reason      = IAP_TXN_PAUSE_NONE;

        if (iap_txn_store_state(&txn, IAP_TXN_STATE_RECEIVED, OTA_CTRL_STAGE_TRANSFER, 0U, 0U) == 0U)
        {
            iap_serial_report_code("TXN");
            iap_return_to_active_app(&current_boot_info, "Txn save", "Run current APP");
            return;
        }

        /* 验证接收到的头部与预期一致 */
        received_header = Ymodem_GetReceivedHeader();
        if (received_header == 0 ||
            received_header->payload.firmware_size != (uint32_t)result ||
            strcmp(ready_info.version, received_header->payload.firmware_version) != 0)
        {
            iap_txn_reset_transfer_info(&txn);
            (void)iap_txn_store_state(&txn, IAP_TXN_STATE_FAILED_TERMINAL,
                                      OTA_CTRL_STAGE_TRANSFER, YMODEM_ERR_VERSION, 0U);
            iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                        OTA_CTRL_STAGE_TRANSFER, YMODEM_ERR_VERSION, (uint32_t)result);
            iap_serial_report_code("HDR");
            iap_reset_request_and_return_active_app(&current_boot_info, "Header mismatch", "Run current APP");
            return;
        }

        /* --- 步骤 14：固件完整性认证 --- */
        auth_result = iap_authorize_received_image(&boot_info, received_header, target_address, &auth_diag);
        if (auth_result != IAP_AUTH_RESULT_OK)
        {
            iap_txn_reset_transfer_info(&txn);
            (void)iap_txn_store_state(&txn, IAP_TXN_STATE_FAILED_TERMINAL,
                                      OTA_CTRL_STAGE_VERIFY_SIG, (uint32_t)auth_result, 0U);
            iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                        OTA_CTRL_STAGE_VERIFY_SIG, (uint16_t)auth_result, (uint32_t)result);
            iap_serial_report_auth_failure(auth_result, &auth_diag);
            iap_reset_request_and_return_active_app(&current_boot_info, "Auth failed",
                                                    iap_auth_result_text(auth_result, &auth_diag));
            return;
        }

        /* --- 步骤 15：认证通过，提交升级 --- */
        if (iap_txn_store_state(&txn, IAP_TXN_STATE_AUTHORIZED, OTA_CTRL_STAGE_VERIFY_SIG, 0U, 0U) == 0U)
        {
            iap_serial_report_code("TXN");
            iap_return_to_active_app(&current_boot_info, "Txn save", "Run current APP");
            return;
        }

        /* 标记待安装 */
        boot_info_mark_pending_install(&boot_info, &received_header->payload);
        if (boot_info_save(&boot_info) != 0U)
        {
            iap_serial_report_code("BOOT");
            iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                        OTA_CTRL_STAGE_DONE, OTA_CTRL_ERR_PROTOCOL, (uint32_t)result);
            iap_reset_request_and_return_active_app(&current_boot_info, "BootInfo save", "Run current APP");
            return;
        }

        /* 清除传输信息，标记已提交 */
        iap_txn_reset_transfer_info(&txn);
        (void)iap_txn_store_state(&txn, IAP_TXN_STATE_COMMITTED, OTA_CTRL_STAGE_DONE, 0U, 0U);

        iap_ui_show_upgrade_success();
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_SUCCESS, OTA_CTRL_STAGE_DONE, 0U, (uint32_t)result);
        iap_serial_report_code("OK");
        delay_ms(800);

        /* 尝试跳转到新应用 */
        if (iap_try_jump_partition(boot_info.active_partition) != 0U)
        {
            while (1)
            {
            }
        }

        /* 跳转失败（新应用无效） */
        (void)iap_txn_store_state(&txn, IAP_TXN_STATE_FAILED_TERMINAL,
                                  OTA_CTRL_STAGE_DONE, OTA_CTRL_ERR_PROTOCOL, 0U);
        iap_send_result_best_effort(OTA_CTRL_RESULT_OUTCOME_TERMINAL,
                                    OTA_CTRL_STAGE_DONE, OTA_CTRL_ERR_PROTOCOL, (uint32_t)result);
        iap_serial_report_code("APPBAD");
        iap_reset_request_and_return_active_app(&current_boot_info, "New APP bad", "Run current APP");
        return;
    }

    /* --- 步骤 16：传输失败处理 --- */
    err_code        = Ymodem_GetErrorCode();
    err_stage       = Ymodem_GetErrorStage();
    uart_error_flags = Ymodem_GetUartErrorFlags();
    final_offset    = Ymodem_GetLastAckedOffset();
    terminal_error  = iap_transfer_error_is_terminal(err_code);

    /* 输出错误码 */
    switch (err_code)
    {
    case YMODEM_ERR_TIMEOUT:      iap_serial_report_code("YMD TIME");   break;
    case YMODEM_ERR_SEQ:          iap_serial_report_code("YMD SEQ");    break;
    case YMODEM_ERR_CRC:          iap_serial_report_code("YMD CRC");    break;
    case YMODEM_ERR_SIZE:         iap_serial_report_code("YMD SIZE");   break;
    case YMODEM_ERR_FLASH:        iap_serial_report_code("YMD FLASH");  break;
    case YMODEM_ERR_ABORT:        iap_serial_report_code("YMD ABORT");  break;
    case YMODEM_ERR_MAX_RETRIES:  iap_serial_report_code("YMD RETRY");  break;
    case YMODEM_ERR_HEADER:       iap_serial_report_code("YMD HDR");    break;
    case YMODEM_ERR_AUTH:         iap_serial_report_code("YMD AUTH");   break;
    case YMODEM_ERR_VERSION:      iap_serial_report_code("YMD VER");    break;
    case YMODEM_ERR_SLOT:         iap_serial_report_code("YMD SLOT");   break;
    case YMODEM_ERR_UART:
        iap_serial_report_code("YMD UART");
        iap_serial_report_uart_flags(uart_error_flags);
        break;
    default:                      iap_serial_report_code("YMD FAIL");   break;
    }

    iap_ui_show_upgrade_failure();

    /* 根据错误类型设置事务状态 */
    txn.last_acked_offset = final_offset;
    txn.last_error_stage  = err_stage;
    txn.last_error_code   = err_code;

    if (terminal_error != 0U)
    {
        iap_txn_reset_transfer_info(&txn);
        txn.pause_reason = IAP_TXN_PAUSE_TERMINAL;
        txn.state        = IAP_TXN_STATE_FAILED_TERMINAL;
        txn.retryable    = 0U;
    }
    else
    {
        txn.state        = IAP_TXN_STATE_PAUSED_RESUMABLE;
        txn.pause_reason = IAP_TXN_PAUSE_RETRYABLE;
        txn.retryable    = 1U;
    }
    (void)txn_save(&txn);

    iap_send_result_best_effort((terminal_error != 0U) ?
                                    OTA_CTRL_RESULT_OUTCOME_TERMINAL : OTA_CTRL_RESULT_OUTCOME_RETRYABLE,
                                (err_stage != 0U) ? err_stage : OTA_CTRL_STAGE_TRANSFER,
                                err_code, final_offset);

    delay_ms(800);
    iap_reset_request_and_return_active_app(&boot_info, "Upgrade failed", "Run current APP");
}

/**
 * @brief  IAP 引导入口（委托给 iap_boot_entry_run）
 */
void iap_boot_entry(void)
{
    iap_boot_entry_run();
}
