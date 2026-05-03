/**
 * @file    ota_ctrl_protocol.h
 * @brief   OTA 控制协议常量定义 —— STM32 与 ESP32 之间的升级协商通道
 * @note    本头文件定义 OTA 控制平面协议的所有常量，包括：
 *          - 帧格式（SOF、协议版本、最大载荷长度）
 *          - 消息类型（REQ/ACK/STATUS/READY/ERROR/META/RESULT/GO/CANCEL）
 *          - 请求类型与标志位
 *          - 分区标识与字段长度
 *          - 各消息载荷长度
 *          - 升级阶段码与错误码
 *          - 续传拒绝原因码
 *          - 事务加载来源与诊断码
 *          - 主机命令与状态位
 *          - 帧开销计算
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef OTA_CTRL_PROTOCOL_H
#define OTA_CTRL_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 *  1. 帧格式常量
 * ======================================================================= */

/** 帧起始标识符（双字节 SOF） */
#define OTA_CTRL_SOF1                    0x55U      /**< SOF 第一字节  */
#define OTA_CTRL_SOF2                    0xAAU      /**< SOF 第二字节  */

/** 控制协议版本号 */
#define OTA_CTRL_PROTOCOL_VERSION        0x03U

/** 最大载荷长度（字节） */
#define OTA_CTRL_MAX_PAYLOAD_LEN         96U

/* =========================================================================
 *  2. 消息类型定义
 * ======================================================================= */

/* --- STM32 → ESP32（请求方向） --- */
#define OTA_CTRL_MSG_REQ                 0x01U      /**< 升级请求         */
#define OTA_CTRL_MSG_CANCEL              0x02U      /**< 取消升级         */
#define OTA_CTRL_MSG_GO                  0x03U      /**< 开始数据传输     */

/* --- ESP32 → STM32（应答方向） --- */
#define OTA_CTRL_MSG_ACK                 0x81U      /**< 确认/拒绝        */
#define OTA_CTRL_MSG_STATUS              0x82U      /**< 进度状态         */
#define OTA_CTRL_MSG_READY               0x83U      /**< 升级就绪         */
#define OTA_CTRL_MSG_ERROR               0x84U      /**< 错误报告         */
#define OTA_CTRL_MSG_META                0x85U      /**< 元数据（分片）   */
#define OTA_CTRL_MSG_RESULT              0x86U      /**< 升级结果         */

/* --- 主机通信（双向） --- */
#define OTA_CTRL_MSG_HOST_REQ            0x20U      /**< 主机命令请求     */
#define OTA_CTRL_MSG_HOST_RSP            0xA0U      /**< 主机命令响应     */
#define OTA_CTRL_MSG_HOST_EVT            0xA1U      /**< 主机事件通知     */

/* =========================================================================
 *  3. 请求类型与标志位
 * ======================================================================= */

/** 请求类型 */
#define OTA_CTRL_REQ_TYPE_UPGRADE        0x01U      /**< 升级请求         */
#define OTA_CTRL_REQ_TYPE_ROLLBACK       0x02U      /**< 回滚请求         */

/** 请求标志位 */
#define OTA_CTRL_REQ_FLAG_CHECK_ONLY     0x00000010UL   /**< 仅检查，不执行   */

/* =========================================================================
 *  4. 就绪/GO 标志位
 * ======================================================================= */

/** READY 消息标志位 */
#define OTA_CTRL_READY_FLAG_RESUME_SUPPORTED 0x0001U    /**< 支持断点续传     */
#define OTA_CTRL_READY_FLAG_CACHE_HIT        0x0002U    /**< 缓存命中         */

/** GO 消息标志位 */
#define OTA_CTRL_GO_FLAG_RESUME_REQUESTED    0x0001U    /**< 请求续传         */

/* =========================================================================
 *  5. META 消息类型
 * ======================================================================= */

#define OTA_CTRL_META_KIND_IMAGE_HEADER      0x01U      /**< 固件镜像头       */
#define OTA_CTRL_META_KIND_RESERVED          0x02U      /**< 保留类型         */

/* =========================================================================
 *  6. 分区标识
 * ======================================================================= */

#define OTA_CTRL_PARTITION_APP1          0x00U      /**< APP1 分区         */
#define OTA_CTRL_PARTITION_APP2          0x01U      /**< APP2 分区         */

/* =========================================================================
 *  7. 字段长度常量
 * ======================================================================= */

#define OTA_CTRL_VERSION_LEN             16U        /**< 版本字符串长度   */
#define OTA_CTRL_PRODUCT_ID_LEN          16U        /**< 产品标识长度     */
#define OTA_CTRL_HW_REV_LEN              8U         /**< 硬件版本长度     */
#define OTA_CTRL_UID_LEN                 12U        /**< 设备唯一 ID 长度 */
#define OTA_CTRL_FINGERPRINT_LEN         32U        /**< 会话指纹长度     */

/* =========================================================================
 *  8. 各消息载荷长度
 * ======================================================================= */

#define OTA_CTRL_REQ_PAYLOAD_LEN         60U        /**< REQ 载荷长度     */
#define OTA_CTRL_ACK_PAYLOAD_LEN         6U         /**< ACK 载荷长度     */
#define OTA_CTRL_STATUS_PAYLOAD_LEN      12U        /**< STATUS 载荷长度  */
#define OTA_CTRL_READY_PAYLOAD_LEN       64U        /**< READY 载荷长度   */
#define OTA_CTRL_ERROR_PAYLOAD_LEN       4U         /**< ERROR 载荷长度   */
#define OTA_CTRL_GO_PAYLOAD_LEN          8U         /**< GO 载荷长度      */
#define OTA_CTRL_RESULT_PAYLOAD_LEN      8U         /**< RESULT 载荷长度  */
#define OTA_CTRL_META_PAYLOAD_HDR_LEN    8U         /**< META 头部长度    */
#define OTA_CTRL_META_MAX_CHUNK_LEN      (OTA_CTRL_MAX_PAYLOAD_LEN - OTA_CTRL_META_PAYLOAD_HDR_LEN) /**< META 最大分片长度 */

/* =========================================================================
 *  9. 升级阶段码
 * ======================================================================= */

#define OTA_CTRL_STAGE_IDLE              0U         /**< 空闲             */
#define OTA_CTRL_STAGE_QUERY             10U        /**< 查询阶段         */
#define OTA_CTRL_STAGE_DOWNLOAD          20U        /**< 下载阶段         */
#define OTA_CTRL_STAGE_VERIFY_SIG        30U        /**< 签名验证         */
#define OTA_CTRL_STAGE_VERIFY_CRC        40U        /**< CRC 校验         */
#define OTA_CTRL_STAGE_AES_PREPARE       50U        /**< AES 准备         */
#define OTA_CTRL_STAGE_READY             60U        /**< 就绪             */
#define OTA_CTRL_STAGE_TRANSFER          70U        /**< 数据传输         */
#define OTA_CTRL_STAGE_YMODEM            OTA_CTRL_STAGE_TRANSFER  /**< 传输别名 */
#define OTA_CTRL_STAGE_DONE              80U        /**< 完成             */

/* =========================================================================
 *  10. 错误码
 * ======================================================================= */

#define OTA_CTRL_ERR_BUSY                1U         /**< 设备忙           */
#define OTA_CTRL_ERR_NO_WIFI             2U         /**< 无 WiFi 连接     */
#define OTA_CTRL_ERR_FETCH_PACKAGE       3U         /**< 获取固件包失败   */
#define OTA_CTRL_ERR_NO_PACKAGE          4U         /**< 无可用固件包     */
#define OTA_CTRL_ERR_PARTITION           5U         /**< 分区错误         */
#define OTA_CTRL_ERR_PRODUCT             6U         /**< 产品标识不匹配   */
#define OTA_CTRL_ERR_HW_REV              7U         /**< 硬件版本不匹配   */
#define OTA_CTRL_ERR_SIGNATURE           8U         /**< 签名验证失败     */
#define OTA_CTRL_ERR_CRC32               9U         /**< CRC32 校验失败   */
#define OTA_CTRL_ERR_AES                 10U        /**< AES 解密失败     */
#define OTA_CTRL_ERR_WAIT_GO             11U        /**< 等待 GO 超时     */
#define OTA_CTRL_ERR_TRANSFER            12U        /**< 数据传输错误     */
#define OTA_CTRL_ERR_YMODEM              OTA_CTRL_ERR_TRANSFER  /**< 传输别名 */
#define OTA_CTRL_ERR_PROTOCOL            13U        /**< 协议错误         */
#define OTA_CTRL_ERR_FETCH_METADATA      14U        /**< 获取元数据失败   */
#define OTA_CTRL_ERR_NO_UPDATE           15U        /**< 无可用更新       */
#define OTA_CTRL_ERR_VERSION             16U        /**< 版本不兼容       */

/* =========================================================================
 *  11. 续传拒绝原因码
 * ======================================================================= */

#define OTA_CTRL_RESUME_REASON_OK                0x0000U  /**< 续传允许     */
#define OTA_CTRL_RESUME_REASON_STATE             0x5201U  /**< 状态不匹配   */
#define OTA_CTRL_RESUME_REASON_REQ_TYPE          0x5202U  /**< 请求类型错误 */
#define OTA_CTRL_RESUME_REASON_ACTIVE_SLOT       0x5203U  /**< 活跃分区错误 */
#define OTA_CTRL_RESUME_REASON_TARGET_SLOT       0x5204U  /**< 目标分区错误 */
#define OTA_CTRL_RESUME_REASON_PROTOCOL          0x5205U  /**< 协议版本错误 */
#define OTA_CTRL_RESUME_REASON_CURRENT_VERSION   0x5206U  /**< 当前版本错误 */
#define OTA_CTRL_RESUME_REASON_TARGET_VERSION    0x5207U  /**< 目标版本错误 */
#define OTA_CTRL_RESUME_REASON_TRANSFER_SIZE     0x5208U  /**< 传输大小错误 */
#define OTA_CTRL_RESUME_REASON_PLAIN_SIZE        0x5209U  /**< 明文大小错误 */
#define OTA_CTRL_RESUME_REASON_CHECKPOINT_SIZE   0x520AU  /**< 检查点间隔错误 */
#define OTA_CTRL_RESUME_REASON_FINGERPRINT       0x520BU  /**< 指纹不匹配   */
#define OTA_CTRL_RESUME_REASON_OFFSET_ZERO       0x520CU  /**< 偏移量为零   */
#define OTA_CTRL_RESUME_REASON_OFFSET_RANGE      0x520DU  /**< 偏移量越界   */
#define OTA_CTRL_RESUME_REASON_OFFSET_CHECKPOINT 0x520EU  /**< 偏移量未对齐检查点 */
#define OTA_CTRL_RESUME_REASON_OFFSET_BLOCK      0x520FU  /**< 偏移量未对齐块 */

/* =========================================================================
 *  12. 事务加载来源
 * ======================================================================= */

#define OTA_CTRL_TXN_LOAD_SRC_NONE               0x0U     /**< 无来源       */
#define OTA_CTRL_TXN_LOAD_SRC_VALID              0x1U     /**< 有效记录     */
#define OTA_CTRL_TXN_LOAD_SRC_EMPTY              0x2U     /**< 空槽位       */
#define OTA_CTRL_TXN_LOAD_SRC_INVALID            0x3U     /**< 无效记录     */

/* =========================================================================
 *  13. 事务加载诊断码
 * ======================================================================= */

#define OTA_CTRL_DIAG_TXN_LOAD_CORE              0x5410U  /**< 核心诊断     */
#define OTA_CTRL_DIAG_TXN_LOAD_OFFSETS           0x5420U  /**< 偏移量诊断   */
#define OTA_CTRL_DIAG_TXN_LOAD_META              0x5430U  /**< 元数据诊断   */
#define OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT1         0x5440U  /**< 无效槽位 1   */
#define OTA_CTRL_DIAG_TXN_LOAD_INV_SLOT2         0x5450U  /**< 无效槽位 2   */
#define OTA_CTRL_DIAG_TXN_LOAD_INV_TXN1          0x5460U  /**< 无效事务 1   */
#define OTA_CTRL_DIAG_TXN_LOAD_INV_TXN2          0x5470U  /**< 无效事务 2   */
#define OTA_CTRL_DIAG_TXN_LOAD_INV_TXN3          0x5480U  /**< 无效事务 3   */
#define OTA_CTRL_DIAG_TXN_LOAD_INV_TXN4          0x5490U  /**< 无效事务 4   */

/* =========================================================================
 *  14. 进度与结果常量
 * ======================================================================= */

/** 进度百分比未知 */
#define OTA_CTRL_PERCENT_UNKNOWN         0xFFU

/** 升级结果类型 */
#define OTA_CTRL_RESULT_OUTCOME_NONE       0U       /**< 无结果           */
#define OTA_CTRL_RESULT_OUTCOME_SUCCESS    1U       /**< 升级成功         */
#define OTA_CTRL_RESULT_OUTCOME_RETRYABLE  2U       /**< 失败（可重试）   */
#define OTA_CTRL_RESULT_OUTCOME_TERMINAL   3U       /**< 失败（不可重试） */

/* =========================================================================
 *  15. 主机命令与响应
 * ======================================================================= */

/** 主机命令载荷长度 */
#define OTA_CTRL_HOST_PAYLOAD_LEN          8U

/** 主机命令码 */
#define OTA_HOST_CMD_GET_STATUS            0x01U    /**< 获取状态         */
#define OTA_HOST_CMD_SET_WIFI              0x02U    /**< 设置 WiFi        */
#define OTA_HOST_CMD_SET_BLE               0x03U    /**< 设置 BLE         */
#define OTA_HOST_CMD_SET_DEBUG_SCREEN      0x04U    /**< 设置调试屏幕     */
#define OTA_HOST_CMD_SET_REMOTE_KEYS       0x05U    /**< 设置遥控器按键   */
#define OTA_HOST_CMD_PING                  0x06U    /**< 心跳             */
#define OTA_HOST_CMD_SET_POWER_POLICY      0x07U    /**< 设置电源策略     */
#define OTA_HOST_CMD_SET_HOST_STATE        0x08U    /**< 设置主机状态     */
#define OTA_HOST_CMD_SEND_THERMAL_SNAPSHOT 0x09U    /**< 发送热成像快照   */
#define OTA_HOST_CMD_SET_MQTT              0x0AU    /**< 设置 MQTT        */

/** 主机命令结果码 */
#define OTA_HOST_RESULT_OK                 0x00U    /**< 成功             */
#define OTA_HOST_RESULT_UNSUPPORTED        0x01U    /**< 不支持的命令     */
#define OTA_HOST_RESULT_BUSY               0x02U    /**< 设备忙           */
#define OTA_HOST_RESULT_FAILED             0x03U    /**< 执行失败         */

/* =========================================================================
 *  16. 主机状态位掩码
 * ======================================================================= */

#define OTA_HOST_STATUS_WIFI_ENABLED           (1UL << 0)   /**< WiFi 已启用      */
#define OTA_HOST_STATUS_WIFI_CONNECTED         (1UL << 1)   /**< WiFi 已连接      */
#define OTA_HOST_STATUS_BLE_ENABLED            (1UL << 2)   /**< BLE 已启用       */
#define OTA_HOST_STATUS_DEBUG_SCREEN_ENABLED   (1UL << 3)   /**< 调试屏幕已启用   */
#define OTA_HOST_STATUS_REMOTE_KEYS_ENABLED    (1UL << 4)   /**< 遥控器按键已启用 */
#define OTA_HOST_STATUS_HAS_CREDENTIALS        (1UL << 5)   /**< 已保存凭据       */
#define OTA_HOST_STATUS_READY_FOR_SLEEP        (1UL << 6)   /**< 可进入休眠       */
#define OTA_HOST_STATUS_MQTT_ENABLED           (1UL << 7)   /**< MQTT 已启用      */
#define OTA_HOST_STATUS_BLE_CONNECTED          (1UL << 8)   /**< BLE 已连接       */
#define OTA_HOST_STATUS_MQTT_CONNECTED         (1UL << 9)   /**< MQTT 已连接      */

/* =========================================================================
 *  17. 遥控器按键与电源策略
 * ======================================================================= */

/** 遥控器按键码 */
#define OTA_HOST_REMOTE_KEY_NONE           0x00U    /**< 无按键           */
#define OTA_HOST_REMOTE_KEY_1              0x01U    /**< 按键 1           */
#define OTA_HOST_REMOTE_KEY_2              0x02U    /**< 按键 2           */
#define OTA_HOST_REMOTE_KEY_3              0x03U    /**< 按键 3           */

/** 电源策略 */
#define OTA_HOST_POWER_POLICY_PERFORMANCE  0x00U    /**< 性能模式         */
#define OTA_HOST_POWER_POLICY_BALANCED     0x01U    /**< 均衡模式         */
#define OTA_HOST_POWER_POLICY_ECO          0x02U    /**< 节能模式         */

/** 主机状态 */
#define OTA_HOST_STATE_ACTIVE              0x00U    /**< 活跃             */
#define OTA_HOST_STATE_SCREEN_OFF          0x01U    /**< 屏幕关闭         */
#define OTA_HOST_STATE_STOP_IDLE           0x02U    /**< 停止空闲         */
#define OTA_HOST_STATE_STANDBY_PREP        0x03U    /**< 待机准备         */

/* =========================================================================
 *  18. 帧开销计算
 * ======================================================================= */

/** 协议头长度（不含 SOF：版本 + 类型 + 序列号 + 长度 = 5 字节） */
#define OTA_CTRL_HEADER_LEN              7U

/** CRC 字段长度 */
#define OTA_CTRL_CRC_LEN                 2U

/** 帧开销（头 + CRC） */
#define OTA_CTRL_FRAME_OVERHEAD          (OTA_CTRL_HEADER_LEN + OTA_CTRL_CRC_LEN)

/** 最大帧长度（载荷 + 帧开销） */
#define OTA_CTRL_MAX_FRAME_LEN           (OTA_CTRL_MAX_PAYLOAD_LEN + OTA_CTRL_FRAME_OVERHEAD)

#ifdef __cplusplus
}
#endif

#endif /* OTA_CTRL_PROTOCOL_H */
