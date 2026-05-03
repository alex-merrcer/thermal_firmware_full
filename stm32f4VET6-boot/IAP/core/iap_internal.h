/**
 * @file    iap_internal.h
 * @brief   IAP 内部头文件 —— 模块间共享的类型、常量与函数声明
 * @note    本头文件定义 IAP 引导加载程序内部使用的所有共享类型，
 *          包括 OTA 控制帧、事务记录、认证诊断、BootInfo 操作等。
 *          仅供 IAP 内部模块包含，不对外暴露。
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef IAP_INTERNAL_H
#define IAP_INTERNAL_H

/* =========================================================================
 *  1. 依赖头文件
 * ======================================================================= */

#include "iap.h"
#include "flash_if.h"
#include "ymodem.h"
#include "lcd.h"
#include "delay.h"
#include "misc.h"
#include "common.h"
#include "stm32f4xx_tim.h"
#include "stm32f4xx_usart.h"
#include "../../../protocol/ota_ctrl_protocol.h"
#include "../../../protocol/ota_data_protocol.h"
#include "../../../protocol/ota_ctrl_protocol_text.h"
#include "../../../protocol/ota_image_header.h"
#include "../../../protocol/ota_rsa_public_key.h"
#include <string.h>

/* =========================================================================
 *  2. OTA 控制协议参数
 * ======================================================================= */

/** OTA 请求标志位掩码（bit0=升级, bit1=回滚） */
#define OTA_CTRL_REQ_FLAG_BASE          0x00000003UL

/* --- 设备标识 --- */
#define IAP_DEVICE_PRODUCT_ID           "LCD"       /**< 产品标识              */
#define IAP_DEVICE_HW_REV               "A1"        /**< 硬件版本              */
#define IAP_DEFAULT_VERSION             "0.0.0"     /**< 默认版本字符串        */
#define IAP_MAX_BOOT_TRIES              3U          /**< 最大试启动次数        */

/* --- OTA 控制超时参数 --- */
#define OTA_CTRL_REQ_RETRY_COUNT        3U          /**< 请求重试次数          */
#define OTA_CTRL_ACK_TIMEOUT_MS         5000U       /**< ACK 等待超时（ms）    */
#define OTA_CTRL_READY_TIMEOUT_MS       120000U     /**< READY 等待超时（ms）  */
#define OTA_CTRL_FRAME_WAIT_MS          500U        /**< 单帧等待超时（ms）    */
#define OTA_CTRL_POLL_STEP_US           50U         /**< 轮询步进（us）        */

/** STM32 唯一 ID 基地址（96 位） */
#define STM32_UID_BASE_ADDR             0x1FFF7A10U

/* =========================================================================
 *  3. 看门狗与启动参数
 * ======================================================================= */

/** IWDG 预分频器（256 分频，时钟 ≈ 125Hz） */
#define IWDG_PRESCALER                  IWDG_Prescaler_256

/** IWDG 超时时间（ms） */
#define IWDG_TIMEOUT_MS                 20000U

/**
 * IWDG 重载值计算
 * 公式：timeout_ms × (LSI_freq / prescaler) / 1000
 * 简化：timeout_ms × 32000 / 1024（LSI ≈ 32kHz，prescaler=256）
 */
#define IWDG_RELOAD                     ((uint16_t)((((uint32_t)IWDG_TIMEOUT_MS * 32000UL) / (256UL * 1000UL)) - 1UL))

/** 启动画面停留时间（ms） */
#define IAP_BOOT_SPLASH_HOLD_MS         3000U

/* =========================================================================
 *  4. 事务记录格式参数
 * ======================================================================= */

/** 事务记录布局魔数："TXN1" */
#define IAP_TXN_LAYOUT_MAGIC            0x54584E31UL

/** 事务记录布局版本 */
#define IAP_TXN_LAYOUT_VERSION          3U

/** 事务记录最大允许大小（字节） */
#define IAP_TXN_RECORD_MAX_SIZE         236U

/* =========================================================================
 *  5. 内部类型定义 —— OTA 控制帧
 * ======================================================================= */

/**
 * @brief  OTA 控制帧结构体
 */
typedef struct
{
    uint8_t msg_type;                               /**< 消息类型             */
    uint8_t seq;                                    /**< 序列号               */
    uint16_t payload_len;                           /**< 有效载荷长度         */
    uint8_t payload[OTA_CTRL_MAX_PAYLOAD_LEN];      /**< 有效载荷数据         */
} ota_ctrl_frame_t;

/* =========================================================================
 *  6. 内部类型定义 —— OTA 就绪信息
 * ======================================================================= */

/**
 * @brief  ESP32 返回的升级就绪信息
 */
typedef struct
{
    uint8_t target_partition;                       /**< 目标分区             */
    uint16_t ready_flags;                           /**< 就绪标志位           */
    char version[BOOT_INFO_VERSION_LEN];            /**< 目标固件版本         */
    uint32_t plain_size;                            /**< 明文固件大小         */
    uint32_t transfer_size;                         /**< 传输大小（含加密填充）*/
    uint32_t checkpoint_size;                       /**< 检查点间隔大小       */
    uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN]; /**< 会话指纹     */
} ota_ctrl_ready_info_t;

/* =========================================================================
 *  7. 内部类型定义 —— 头部校验上下文
 * ======================================================================= */

/**
 * @brief  头部校验上下文（传递给 iap_validate_received_header）
 */
typedef struct
{
    const BootInfoTypeDef *boot_info;               /**< BootInfo 指针        */
    uint8_t error_code;                             /**< 校验错误码输出       */
} iap_header_validation_context_t;

/* =========================================================================
 *  8. 内部类型定义 —— 事务加载诊断
 * ======================================================================= */

/**
 * @brief  事务加载诊断信息（用于调试无效槽位/事务的原因）
 */
typedef struct
{
    uint8_t source;                     /**< 加载来源（VALID/EMPTY/INVALID） */
    uint8_t has_valid;                  /**< 是否存在有效记录                */
    uint8_t reserved0;
    uint8_t reserved1;
    uint32_t latest_seq;                /**< 最新有效记录的序列号            */
    uint32_t programmed_slots;          /**< 已编程槽位数                    */
    uint32_t invalid_slots;             /**< 无效槽位数                      */
    uint32_t invalid_slot_header;       /**< 头部无效的槽位数                */
    uint32_t invalid_slot_commit;       /**< 提交魔数无效的槽位数            */
    uint32_t invalid_slot_payload_crc;  /**< 有效载荷 CRC 无效的槽位数       */
    uint32_t invalid_slot_payload_validate; /**< 有效载荷验证失败的槽位数    */
    uint32_t invalid_txn_layout;        /**< 布局无效的事务数                */
    uint32_t invalid_txn_state;         /**< 状态无效的事务数                */
    uint32_t invalid_txn_partition;     /**< 分区无效的事务数                */
    uint32_t invalid_txn_fields;        /**< 字段无效的事务数                */
    uint32_t invalid_txn_request_type;  /**< 请求类型无效的事务数            */
    uint32_t invalid_txn_version_text;  /**< 版本文本无效的事务数            */
    uint32_t invalid_txn_offset;        /**< 偏移量无效的事务数              */
    uint32_t invalid_txn_data_crc;      /**< 数据 CRC 无效的事务数           */
} iap_txn_load_diag_t;

/* =========================================================================
 *  9. 内部类型定义 —— 认证结果与诊断
 * ======================================================================= */

/**
 * @brief  认证结果枚举
 */
typedef enum
{
    IAP_AUTH_RESULT_OK              = 0U,   /**< 认证通过                 */
    IAP_AUTH_RESULT_SIZE            = 1U,   /**< 固件大小不匹配           */
    IAP_AUTH_RESULT_HASH_ENGINE     = 2U,   /**< 哈希引擎故障             */
    IAP_AUTH_RESULT_HASH_MISMATCH   = 3U,   /**< 哈希不匹配               */
    IAP_AUTH_RESULT_RSA             = 4U,   /**< RSA 签名验证失败         */
    IAP_AUTH_RESULT_INTERNAL        = 5U    /**< 内部错误                 */
} iap_auth_result_t;

/**
 * @brief  哈希诊断枚举（细分哈希验证失败原因）
 */
typedef enum
{
    IAP_HASH_DIAG_NONE              = 0U,   /**< 无诊断                   */
    IAP_HASH_DIAG_BODY_CALC         = 1U,   /**< 体哈希计算失败           */
    IAP_HASH_DIAG_FLASH_CALC        = 2U,   /**< Flash 哈希计算失败       */
    IAP_HASH_DIAG_BODY_MISMATCH     = 3U,   /**< 体哈希 ≠ 预期哈希        */
    IAP_HASH_DIAG_FLASH_MISMATCH    = 4U,   /**< Flash 哈希 ≠ 预期哈希    */
    IAP_HASH_DIAG_EXPECTED_MISMATCH = 5U,   /**< 预期哈希不匹配           */
    IAP_HASH_DIAG_SELF_TEST         = 6U    /**< SHA-256 自测失败         */
} iap_hash_diag_t;

/**
 * @brief  认证诊断信息（记录三组哈希值用于调试）
 */
typedef struct
{
    uint8_t expected_hash[32];          /**< 头部中的预期固件哈希       */
    uint8_t body_hash[32];              /**< 传输过程中计算的体哈希     */
    uint8_t flash_hash[32];             /**< 从 Flash 读取后计算的哈希  */
    uint8_t has_expected;               /**< 预期哈希是否有效           */
    uint8_t has_body;                   /**< 体哈希是否有效             */
    uint8_t has_flash;                  /**< Flash 哈希是否有效         */
    iap_hash_diag_t hash_diag;          /**< 哈希诊断细分原因           */
} iap_auth_diag_t;

/* =========================================================================
 *  10. 内部类型定义 —— 事务状态机
 * ======================================================================= */

/**
 * @brief  事务状态枚举
 * @note   状态转换路径：
 *         IDLE → NEGOTIATING → HEADER_VERIFIED → TRANSFERRING → RECEIVED
 *         → AUTHORIZED → COMMITTED
 *         任何阶段失败 → FAILED_RETRYABLE / FAILED_TERMINAL
 *         暂停状态 → PAUSED_RESUMABLE
 */
typedef enum
{
    IAP_TXN_STATE_IDLE              = 0U,   /**< 空闲                     */
    IAP_TXN_STATE_NEGOTIATING       = 1U,   /**< 正在协商                 */
    IAP_TXN_STATE_HEADER_VERIFIED   = 2U,   /**< 头部已验证               */
    IAP_TXN_STATE_TRANSFERRING      = 3U,   /**< 正在传输                 */
    IAP_TXN_STATE_PAUSED_RESUMABLE  = 4U,   /**< 已暂停（可续传）         */
    IAP_TXN_STATE_RECEIVED          = 5U,   /**< 已接收完成               */
    IAP_TXN_STATE_AUTHORIZED        = 6U,   /**< 已授权（认证通过）       */
    IAP_TXN_STATE_COMMITTED         = 7U,   /**< 已提交（升级完成）       */
    IAP_TXN_STATE_FAILED_RETRYABLE  = 8U,   /**< 失败（可重试）           */
    IAP_TXN_STATE_FAILED_TERMINAL   = 9U    /**< 失败（不可重试）         */
} iap_txn_state_t;

/** 暂停原因：无暂停 */
#define IAP_TXN_PAUSE_NONE              0U
/** 暂停原因：可重试的临时错误 */
#define IAP_TXN_PAUSE_RETRYABLE         1U
/** 暂停原因：协议版本不匹配 */
#define IAP_TXN_PAUSE_PROTOCOL_MISMATCH 2U
/** 暂停原因：不可恢复的终端错误 */
#define IAP_TXN_PAUSE_TERMINAL          3U

/* =========================================================================
 *  11. 内部类型定义 —— OTA 事务记录
 * ======================================================================= */

/**
 * @brief  OTA 事务记录结构体（持久化到日志区）
 * @note   记录一次 OTA 升级会话的完整状态，支持断点续传。
 *         大小必须 ≤ IAP_TXN_RECORD_MAX_SIZE（236 字节）。
 */
typedef struct
{
    uint32_t layout_magic;                          /**< 布局魔数（IAP_TXN_LAYOUT_MAGIC） */
    uint16_t layout_version;                        /**< 布局版本                         */
    uint16_t struct_size;                           /**< 结构体大小                       */
    uint32_t data_crc32;                            /**< 数据区 CRC32 校验               */
    uint32_t txn_counter;                           /**< 事务计数器（递增）               */
    uint32_t state;                                 /**< 事务状态（iap_txn_state_t）      */
    uint32_t request_type;                          /**< 请求类型（升级/回滚）            */
    uint32_t active_slot;                           /**< 当前活跃分区                     */
    uint32_t target_slot;                           /**< 目标升级分区                     */
    char current_version[BOOT_INFO_VERSION_LEN];    /**< 当前固件版本                     */
    char target_version[BOOT_INFO_VERSION_LEN];     /**< 目标固件版本                     */
    uint32_t last_error_stage;                      /**< 最后错误阶段                     */
    uint32_t last_error_code;                       /**< 最后错误码                       */
    uint32_t retryable;                             /**< 是否可重试（0/1）                */
    uint32_t transfer_total_size;                   /**< 传输总大小                       */
    uint32_t protocol_version;                      /**< 协议版本                         */
    uint32_t checkpoint_size;                       /**< 检查点间隔                       */
    uint32_t chunk_size;                            /**< 单次传输块大小                   */
    uint32_t plain_size;                            /**< 明文固件大小                     */
    uint32_t resume_offset;                         /**< 续传偏移量                       */
    uint32_t last_acked_offset;                     /**< 最后确认偏移量                   */
    uint32_t pause_reason;                          /**< 暂停原因                         */
    uint8_t session_fingerprint[OTA_CTRL_FINGERPRINT_LEN]; /**< 会话指纹                  */
    uint32_t reserved[24];                          /**< 保留字段（未来扩展）             */
} OtaTxnRecord;

/** 编译时校验：OtaTxnRecord 大小不超过 236 字节 */
typedef char iap_txn_record_size_check[(sizeof(OtaTxnRecord) <= IAP_TXN_RECORD_MAX_SIZE) ? 1 : -1];

/* =========================================================================
 *  12. 外部变量声明
 * ======================================================================= */

/** Ymodem 传输缓冲区（1024 字节） */
extern uint8_t tab_1024[1024];

/* =========================================================================
 *  13. 函数声明 —— 版本字符串操作
 * ======================================================================= */

uint8_t version_text_is_valid(const char *version);
void version_text_copy(char *target, uint32_t target_len, const char *source);
int32_t version_text_compare(const char *left, const char *right);

/* =========================================================================
 *  14. 函数声明 —— BootInfo 操作
 * ======================================================================= */

const char *boot_info_get_partition_version(const BootInfoTypeDef *info, uint32_t partition);
uint32_t boot_info_partition_address(uint32_t partition);
uint32_t boot_info_inactive_partition(uint32_t active_partition);
void boot_info_sync_current_version(BootInfoTypeDef *info);
void boot_info_mark_pending_install(BootInfoTypeDef *info, const OtaImageHeaderPayload *payload);
uint8_t boot_info_switch_to_confirmed_slot(BootInfoTypeDef *info, uint32_t slot);
uint32_t boot_info_save(const BootInfoTypeDef *info);
void boot_info_load(BootInfoTypeDef *info);

/* =========================================================================
 *  15. 函数声明 —— 事务记录操作
 * ======================================================================= */

void txn_init_default(OtaTxnRecord *txn);
void txn_load(OtaTxnRecord *txn);
void txn_load_with_diag(OtaTxnRecord *txn, iap_txn_load_diag_t *diag);
uint32_t txn_save(const OtaTxnRecord *txn);
uint32_t txn_compact_with_boot_info(const BootInfoTypeDef *boot_info, const OtaTxnRecord *txn);
uint32_t txn_clear(void);

/* =========================================================================
 *  16. 函数声明 —— 认证与验证
 * ======================================================================= */

uint8_t iap_validate_received_header(const OtaImageHeaderBinary *header, void *context);
uint8_t iap_verify_image_header_signature(const OtaImageHeaderBinary *header);
iap_auth_result_t iap_authorize_received_image(const BootInfoTypeDef *boot_info,
                                               const OtaImageHeaderBinary *header,
                                               uint32_t firmware_address,
                                               iap_auth_diag_t *diag);
const char *iap_auth_result_text(iap_auth_result_t result, const iap_auth_diag_t *diag);

/* =========================================================================
 *  17. 函数声明 —— 串口诊断报告
 * ======================================================================= */

void iap_serial_report_code(const char *code);
void iap_serial_report_auth_failure(iap_auth_result_t result, const iap_auth_diag_t *diag);
void iap_serial_report_txn_load(const OtaTxnRecord *txn, const iap_txn_load_diag_t *diag);
void iap_serial_report_uart_flags(uint32_t flags);

/* =========================================================================
 *  18. 函数声明 —— OTA 控制协议
 * ======================================================================= */

uint8_t ota_ctrl_wait_for_upgrade_ready(const BootInfoTypeDef *boot_info,
                                        ota_ctrl_ready_info_t *ready_info,
                                        uint16_t *reject_reason,
                                        uint32_t req_flags);
uint8_t ota_ctrl_send_status(uint8_t stage,
                             uint8_t percent,
                             uint16_t detail_code,
                             uint32_t current_value,
                             uint32_t total_value);
uint8_t ota_ctrl_send_go(uint8_t target_partition, uint16_t go_flags, uint32_t resume_offset);
uint8_t ota_ctrl_wait_for_meta_image_header(OtaImageHeaderBinary *header, uint32_t timeout_ms);
uint8_t ota_ctrl_send_result(uint8_t outcome, uint8_t stage, uint16_t error_code, uint32_t final_offset);
void ota_ctrl_flush_uart(void);

/* =========================================================================
 *  19. 函数声明 —— UI 显示
 * ======================================================================= */

void ota_ctrl_show_status_lines(u16 color,
                                const char *line1,
                                const char *line2,
                                const char *line3,
                                const char *line4);
void ota_ctrl_show_status_text(const char *line1, const char *line2);
void ota_ctrl_show_stage(uint8_t stage,
                         uint8_t percent,
                         uint16_t detail_code,
                         uint32_t current_value,
                         uint32_t total_value);
void ota_ctrl_show_ready_info(const ota_ctrl_frame_t *frame);
void ota_ctrl_show_error_code(uint8_t stage, uint16_t error_code);
void ota_ctrl_show_ack_reject_reason(uint16_t reason_code);
void iap_show_version_lines(const BootInfoTypeDef *boot_info);
void iap_show_resume_decision(uint8_t accepted,
                              uint16_t reason_code,
                              uint32_t saved_offset,
                              uint32_t total_size);
void iap_ui_show_boot_splash(void);
void iap_ui_run_normal_boot_2s(uint8_t feed_watchdog);
void iap_ui_boot_prepare(uint8_t warm_handoff);
void iap_ui_show_upgrade_prepare(void);
void iap_ui_show_upgrade_success(void);
void iap_ui_show_upgrade_failure(void);
void iap_reset_ymodem_progress(void);
void iap_show_ymodem_progress(uint32_t current, uint32_t total);

/* =========================================================================
 *  20. 函数声明 —— 平台抽象层
 * ======================================================================= */

void iap_init_watchdog(void);
void iap_feed_watchdog(void);
uint8_t is_app_valid(uint32_t app_addr);
void iap_cleanup_before_jump(void);
uint8_t get_reset_reason(void);
void clear_reset_flags(void);
uint32_t Send_Byte(uint8_t c);

#endif /* IAP_INTERNAL_H */
