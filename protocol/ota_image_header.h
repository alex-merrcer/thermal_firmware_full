#ifndef OTA_IMAGE_HEADER_H
#define OTA_IMAGE_HEADER_H

#include <stdint.h>
#include "boot_info_v3.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_IMAGE_HEADER_MAGIC                            0x4F544132UL
#define OTA_IMAGE_HEADER_VERSION                          1U
#define OTA_IMAGE_FORMAT_VERSION                          1U
#define OTA_IMAGE_SIGNATURE_LEN                           256U
#define OTA_IMAGE_SIG_ALG_RSA2048_SHA256_PKCS1V15         1U

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define OTA_IMAGE_PACKED
#elif defined(__GNUC__)
#define OTA_IMAGE_PACKED __attribute__((packed))
#else
#define OTA_IMAGE_PACKED
#endif

typedef struct OTA_IMAGE_PACKED
{
    uint32_t magic;
    uint16_t header_version;
    uint16_t reserved0;
    uint32_t header_size;
    uint32_t signature_len;
    uint32_t reserved1;
} OtaImageHeaderEnvelope;

typedef struct OTA_IMAGE_PACKED
{
    uint32_t format_version;
    uint32_t target_slot;
    char firmware_version[BOOT_INFO_VERSION_LEN];
    uint32_t firmware_size;
    uint8_t firmware_sha256[32];
    uint8_t iv[16];
    uint32_t signature_algorithm;
    char min_allowed_version[BOOT_INFO_VERSION_LEN];
} OtaImageHeaderPayload;

typedef struct OTA_IMAGE_PACKED
{
    OtaImageHeaderEnvelope envelope;
    OtaImageHeaderPayload payload;
    uint8_t signature[OTA_IMAGE_SIGNATURE_LEN];
} OtaImageHeaderBinary;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#define OTA_IMAGE_HEADER_ENVELOPE_SIZE  ((uint32_t)sizeof(OtaImageHeaderEnvelope))
#define OTA_IMAGE_HEADER_PAYLOAD_SIZE   ((uint32_t)sizeof(OtaImageHeaderPayload))
#define OTA_IMAGE_HEADER_TOTAL_SIZE     ((uint32_t)sizeof(OtaImageHeaderBinary))

#ifdef __cplusplus
}
#endif

#endif
