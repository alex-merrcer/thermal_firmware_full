/**
 * @file    ota_image_header.h
 * @brief   OTA 固件镜像头定义 —— 固件包的元数据结构
 * @note    本头文件定义 OTA 固件镜像头的二进制格式，包括：
 *          - 镜像头魔数与版本号
 *          - 签名算法标识（RSA-2048 + SHA-256 + PKCS#1 v1.5）
 *          - 信封结构体（OtaImageHeaderEnvelope）：魔数、版本、大小
 *          - 载荷结构体（OtaImageHeaderPayload）：格式版本、目标分区、
 *            固件版本、固件大小、SHA-256 哈希、AES-128 IV、签名算法、
 *            最低允许版本
 *          - 完整镜像头结构体（OtaImageHeaderBinary）：信封 + 载荷 + 签名
 *          - 结构体大小常量
 *
 * @par 镜像头布局（二进制）
 *      | 信封（20 字节） | 载荷（76 字节） | 签名（256 字节） |
 *      总大小 = 352 字节
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef OTA_IMAGE_HEADER_H
#define OTA_IMAGE_HEADER_H

#include <stdint.h>
#include "boot_info_v3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 *  1. 镜像头常量
 * ======================================================================= */

/** 镜像头魔数："OTA2"（0x4F544132） */
#define OTA_IMAGE_HEADER_MAGIC                            0x4F544132UL

/** 镜像头版本号 */
#define OTA_IMAGE_HEADER_VERSION                          1U

/** 镜像格式版本号 */
#define OTA_IMAGE_FORMAT_VERSION                          1U

/** 签名长度（字节）：RSA-2048 = 256 字节 */
#define OTA_IMAGE_SIGNATURE_LEN                           256U

/** 签名算法标识：RSA-2048 + SHA-256 + PKCS#1 v1.5 */
#define OTA_IMAGE_SIG_ALG_RSA2048_SHA256_PKCS1V15         1U

/* =========================================================================
 *  2. 跨平台字节对齐控制
 * ======================================================================= */

#if defined(_MSC_VER)
#pragma pack(push, 1)                                   /* MSVC：1 字节对齐 */
#define OTA_IMAGE_PACKED
#elif defined(__GNUC__)
#define OTA_IMAGE_PACKED __attribute__((packed))         /* GCC：紧凑对齐    */
#else
#define OTA_IMAGE_PACKED
#endif

/* =========================================================================
 *  3. 镜像头信封结构体（20 字节）
 * ======================================================================= */

/**
 * @brief  镜像头信封 —— 包含魔数、版本和元数据
 */
typedef struct OTA_IMAGE_PACKED
{
    uint32_t magic;                                     /**< 魔数（OTA_IMAGE_HEADER_MAGIC） */
    uint16_t header_version;                            /**< 镜像头版本号                   */
    uint16_t reserved0;                                 /**< 保留字段                       */
    uint32_t header_size;                               /**< 镜像头总大小                   */
    uint32_t signature_len;                             /**< 签名长度                       */
    uint32_t reserved1;                                 /**< 保留字段                       */
} OtaImageHeaderEnvelope;

/* =========================================================================
 *  4. 镜像头载荷结构体（76 字节）
 * ======================================================================= */

/**
 * @brief  镜像头载荷 —— 包含固件元数据和加密参数
 */
typedef struct OTA_IMAGE_PACKED
{
    uint32_t format_version;                            /**< 格式版本号                     */
    uint32_t target_slot;                               /**< 目标分区（APP1/APP2）          */
    char firmware_version[BOOT_INFO_VERSION_LEN];       /**< 固件版本字符串（16 字节）      */
    uint32_t firmware_size;                             /**< 固件明文大小（字节）           */
    uint8_t firmware_sha256[32];                        /**< 固件 SHA-256 哈希值           */
    uint8_t iv[16];                                     /**< AES-128 CBC/CTR 初始向量      */
    uint32_t signature_algorithm;                       /**< 签名算法标识                   */
    char min_allowed_version[BOOT_INFO_VERSION_LEN];    /**< 最低允许升级版本（16 字节）    */
} OtaImageHeaderPayload;

/* =========================================================================
 *  5. 完整镜像头结构体（352 字节）
 * ======================================================================= */

/**
 * @brief  完整 OTA 固件镜像头（信封 + 载荷 + 签名）
 */
typedef struct OTA_IMAGE_PACKED
{
    OtaImageHeaderEnvelope envelope;                    /**< 信封（20 字节）                */
    OtaImageHeaderPayload payload;                      /**< 载荷（76 字节）                */
    uint8_t signature[OTA_IMAGE_SIGNATURE_LEN];         /**< RSA-2048 签名（256 字节）      */
} OtaImageHeaderBinary;

/* =========================================================================
 *  6. 恢复默认对齐
 * ======================================================================= */

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

/* =========================================================================
 *  7. 结构体大小常量
 * ======================================================================= */

#define OTA_IMAGE_HEADER_ENVELOPE_SIZE  ((uint32_t)sizeof(OtaImageHeaderEnvelope))   /**< 信封大小   */
#define OTA_IMAGE_HEADER_PAYLOAD_SIZE   ((uint32_t)sizeof(OtaImageHeaderPayload))    /**< 载荷大小   */
#define OTA_IMAGE_HEADER_TOTAL_SIZE     ((uint32_t)sizeof(OtaImageHeaderBinary))     /**< 总大小     */

#ifdef __cplusplus
}
#endif

#endif /* OTA_IMAGE_HEADER_H */
