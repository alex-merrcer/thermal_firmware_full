/**
 * @file    ota_rsa_public_key.generated.h
 * @brief   OTA RSA 公钥（自动生成版本） —— 固件签名验证的可信公钥
 * @note    本头文件由 ota-release/tools/OtaPublicKeyExportTool 自动生成，
 *          包含 OTA 固件签名验证所使用的 RSA-2048 公钥。
 *          以 32 位字数组形式存储模数（Little-Endian Word Form），
 *          供 STM32 bootloader 的大数运算库直接使用。
 *
 * @warning 本文件为自动生成，请勿手动编辑。
 *          如需更新公钥，请使用 OtaPublicKeyExportTool 重新生成。
 *
 * @par 密钥参数
 *      - 算法：RSA-2048
 *      - 模数长度：256 字节（64 个 32 位字）
 *      - 公钥指数：65537（0x10001）
 *      - Montgomery 约化参数 N0'：0x95F401A1
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef OTA_RSA_PUBLIC_KEY_H
#define OTA_RSA_PUBLIC_KEY_H

#include <stdint.h>

/* =========================================================================
 *  1. RSA 公钥参数（自动生成）
 * ======================================================================= */

/** 模数的 32 位字数量（2048 / 32 = 64） */
#define OTA_RSA_PUBLIC_KEY_WORD_COUNT 64U

/** 模数的字节长度（2048 / 8 = 256） */
#define OTA_RSA_PUBLIC_KEY_MODULUS_BYTES 256U

/** 公钥指数（e = 65537 = 0x10001） */
#define OTA_RSA_PUBLIC_KEY_PUBLIC_EXPONENT 65537UL

/** Montgomery 约化参数 N0'（用于 Barrett/Montgomery 模乘加速） */
#define OTA_RSA_PUBLIC_KEY_N0_INV 0x95F401A1UL

/* =========================================================================
 *  2. RSA 模数（32 位字数组，Little-Endian Word Order）
 * ======================================================================= */

/**
 * @brief  RSA-2048 公钥模数（64 个 32 位字）
 * @note   由 OtaPublicKeyExportTool 自动生成，
 *         字节序为 Little-Endian Word Order（最低有效字在前）。
 */
static const uint32_t s_ota_rsa_public_key_modulus_words[OTA_RSA_PUBLIC_KEY_WORD_COUNT] =
{
    0xB22BDD9FUL, 0x9B15C2D3UL, 0x7BAF9924UL, 0xD8EF5DA8UL,
    0x88F47457UL, 0xA2807414UL, 0x8F5809EDUL, 0x4EDA7CABUL,
    0xF620FC8DUL, 0x60A38E6EUL, 0x444937B8UL, 0xAF7ED177UL,
    0x6CA8A13CUL, 0xFFDF3175UL, 0xCAB2FAB8UL, 0x118CCD51UL,
    0x51E68BD0UL, 0xACC6FEAAUL, 0xDC4045E4UL, 0xA578EBEEUL,
    0x1F05D33BUL, 0x51A8B653UL, 0x9C8F6487UL, 0x82578952UL,
    0x4FCF433CUL, 0x6E5725CFUL, 0xC9D31472UL, 0xD2135819UL,
    0x0055100FUL, 0x2D26D86AUL, 0xFC0F7706UL, 0x00FF218AUL,
    0x2806F4D0UL, 0xCAF1659CUL, 0xF17C3AE0UL, 0xE279BD43UL,
    0x9343D53BUL, 0x85AF86B6UL, 0x38047DFEUL, 0xE24A53F8UL,
    0xD36E74FCUL, 0x67F04C6AUL, 0xE593E58FUL, 0xD20ABE68UL,
    0x34AEE997UL, 0x06E7FB2AUL, 0xEE88DEDDUL, 0x8CF95660UL,
    0x77F5283DUL, 0xD04138CDUL, 0x4F1A6A22UL, 0xA75D7106UL,
    0xD3069880UL, 0xFD9364D2UL, 0x44E73FF1UL, 0xA23E1A9FUL,
    0x0AA6EC49UL, 0xEE7512E3UL, 0x57879EDAUL, 0xDDFD8020UL,
    0xBF385EFCUL, 0x503D2182UL, 0xD07C30ABUL, 0xB69C1675UL
};

#endif /* OTA_RSA_PUBLIC_KEY_H */
