/**
 * @file    ota_rsa_public_key_pem.h
 * @brief   OTA RSA 公钥（PEM 格式） —— 用于服务器端验证或工具链
 * @note    本头文件以 PEM 格式嵌入 RSA-2048 公钥，
 *          主要用于：
 *          - OTA 服务器端签名验证
 *          - 构建工具链中的密钥引用
 *          - 调试与密钥比对
 *
 * @par PEM 格式
 *      标准 PKCS#8 DER 编码的 Base64 文本，
 *      包含 RSA-2048 公钥（模数 + 指数 65537）。
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef OTA_RSA_PUBLIC_KEY_PEM_H
#define OTA_RSA_PUBLIC_KEY_PEM_H

/* =========================================================================
 *  1. PEM 格式公钥
 * ======================================================================= */

/**
 * @brief  RSA-2048 公钥（PEM 格式字符串）
 * @note   标准 PKCS#8 格式，包含 2048 位模数和公钥指数 65537。
 */
static const char *s_embedded_public_key_pem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAtpwWddB8MKtQPSGCvzhe\n"
    "/N39gCBXh57a7nUS4wqm7EmiPhqfROc/8f2TZNLTBpiAp11xBk8aaiLQQTjNd/Uo\n"
    "PYz5VmDuiN7dBuf7KjSu6ZfSCr5o5ZPlj2fwTGrTbnT84kpT+DgEff6Fr4a2k0PV\n"
    "O+J5vUPxfDrgyvFlnCgG9NAA/yGK/A93Bi0m2GoAVRAP0hNYGcnTFHJuVyXPT89D\n"
    "PIJXiVKcj2SHUai2Ux8F0zuleOvu3EBF5KzG/qpR5ovQEYzNUcqy+rj/3zF1bKih\n"
    "PK9+0XdESTe4YKOObvYg/I1O2nyrj1gJ7aKAdBSI9HRX2O9dqHuvmSSbFcLTsivd\n"
    "nwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
    ;

#endif /* OTA_RSA_PUBLIC_KEY_PEM_H */
