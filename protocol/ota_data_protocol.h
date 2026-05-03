/**
 * @file    ota_data_protocol.h
 * @brief   OTA 数据协议常量定义 —— 固件数据传输通道
 * @note    本头文件定义 OTA 数据平面协议的所有常量，包括：
 *          - 帧格式（SOF、协议版本、最大载荷长度）
 *          - 帧类型（START/CHUNK/FINISH/ACK/NAK/ABORT）
 *          - 帧结构参数（头部长度、尾部长度、帧开销）
 *          - 传输参数（默认块大小、检查点间隔、最大重试次数）
 *          - 各帧载荷长度
 *          - START 标志位与 NAK 拒绝原因码
 *          - ABORT 错误类别
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef OTA_DATA_PROTOCOL_H
#define OTA_DATA_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 *  1. 帧格式常量
 * ======================================================================= */

/** 数据协议版本号 */
#define OTA_DATA_PROTOCOL_VERSION          0x01U

/** 帧起始标识符（双字节 SOF，与控制协议区分） */
#define OTA_DATA_SOF1                      0xA5U    /**< SOF 第一字节  */
#define OTA_DATA_SOF2                      0x5AU    /**< SOF 第二字节  */

/* =========================================================================
 *  2. 帧类型定义
 * ======================================================================= */

/* --- STM32 → ESP32（数据方向） --- */
#define OTA_DATA_TYPE_START                0x01U    /**< 传输启动帧       */
#define OTA_DATA_TYPE_CHUNK                0x02U    /**< 固件数据块帧     */
#define OTA_DATA_TYPE_FINISH               0x03U    /**< 传输完成帧       */

/* --- ESP32 → STM32（应答方向） --- */
#define OTA_DATA_TYPE_ACK                  0x81U    /**< 确认帧           */
#define OTA_DATA_TYPE_NAK                  0x82U    /**< 否定确认帧       */
#define OTA_DATA_TYPE_ABORT                0x84U    /**< 终止帧           */

/* =========================================================================
 *  3. 帧结构参数
 * ======================================================================= */

/** 最大载荷长度（字节） */
#define OTA_DATA_MAX_PAYLOAD_LEN           512U

/** 固定头部长度（字节）：SOF(2) + VER(1) + TYPE(1) + SID(4) + OFF(4) + LEN(2) + CRC(2) = 16 */
#define OTA_DATA_FIXED_HEADER_LEN          16U

/** 尾部长度（字节）：载荷 CRC32 = 4 */
#define OTA_DATA_TRAILER_LEN               4U

/** 帧开销（头部 + 尾部） */
#define OTA_DATA_FRAME_OVERHEAD            (OTA_DATA_FIXED_HEADER_LEN + OTA_DATA_TRAILER_LEN)

/** 最大帧长度（载荷 + 帧开销） */
#define OTA_DATA_MAX_FRAME_LEN             (OTA_DATA_MAX_PAYLOAD_LEN + OTA_DATA_FRAME_OVERHEAD)

/* =========================================================================
 *  4. 传输参数
 * ======================================================================= */

/** 默认单次传输块大小（字节） */
#define OTA_DATA_DEFAULT_CHUNK_SIZE        512U

/** 默认检查点间隔（字节） */
#define OTA_DATA_DEFAULT_CHECKPOINT_SIZE   4096U

/** 最大重试次数 */
#define OTA_DATA_MAX_RETRIES               5U

/* =========================================================================
 *  5. 各帧载荷长度
 * ======================================================================= */

#define OTA_DATA_START_PAYLOAD_LEN         48U      /**< START 帧载荷长度 */
#define OTA_DATA_NAK_PAYLOAD_LEN           4U       /**< NAK 帧载荷长度   */
#define OTA_DATA_ABORT_PAYLOAD_LEN         4U       /**< ABORT 帧载荷长度 */

/* =========================================================================
 *  6. START 帧标志位
 * ======================================================================= */

#define OTA_DATA_START_FLAG_RESUME         0x0001U  /**< 续传请求标志     */

/* =========================================================================
 *  7. NAK 拒绝原因码
 * ======================================================================= */

#define OTA_DATA_NAK_REASON_RETRY          1U       /**< 通用重试         */
#define OTA_DATA_NAK_REASON_SESSION        2U       /**< 会话 ID 不匹配   */
#define OTA_DATA_NAK_REASON_OFFSET         3U       /**< 偏移量不连续     */
#define OTA_DATA_NAK_REASON_PAYLOAD_CRC    4U       /**< 载荷 CRC 校验失败 */
#define OTA_DATA_NAK_REASON_LENGTH         5U       /**< 载荷长度错误     */
#define OTA_DATA_NAK_REASON_FLASH          6U       /**< Flash 写入失败   */
#define OTA_DATA_NAK_REASON_STATE          7U       /**< 状态错误         */
#define OTA_DATA_NAK_REASON_PROTOCOL       8U       /**< 协议错误         */

/* =========================================================================
 *  8. ABORT 错误类别
 * ======================================================================= */

#define OTA_DATA_ABORT_CLASS_RETRYABLE     1U       /**< 可重试错误       */
#define OTA_DATA_ABORT_CLASS_TERMINAL      2U       /**< 终端错误         */

#ifdef __cplusplus
}
#endif

#endif /* OTA_DATA_PROTOCOL_H */
