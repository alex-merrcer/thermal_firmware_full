/**
 * @file    iap_auth.c
 * @brief   OTA 镜像认证模块 —— RSA-2048 SHA-256 PKCS#1 v1.5 签名验证
 * @note    本模块实现 OTA 固件镜像的完整认证流程，包括：
 *          - SHA-256 自测验证
 *          - RSA-2048 大数运算（模加、模乘、模幂）
 *          - PKCS#1 v1.5 签名编码与验证
 *          - 头部字段合法性校验
 *          - 多重哈希交叉验证（body/flash/expected）
 *          - 诊断串口报告
 *
 * @par 认证流程
 *      1. 头部校验：magic、版本、大小、分区槽位、版本约束
 *      2. SHA-256 自测：验证哈希引擎正确性
 *      3. 体哈希验证：传输过程中计算的 body hash 与预期比对
 *      4. Flash 哈希验证：从 Flash 读取固件并计算哈希
 *      5. RSA-2048 签名验证：PKCS#1 v1.5 解码 + SHA-256 摘要比对
 *
 * @par RSA 大数运算
 *      使用 64 个 32 位字（2048 位）表示大数，实现：
 *      - 字级加法/减法/比较
 *      - 模加法/模倍乘（避免溢出的阈值判断）
 *      - 模乘法（二进制展开法）
 *      - 模幂运算（平方-乘法法）
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "iap_auth.h"
#include "uart_rx_ring.h"

/* =========================================================================
 *  2. 内部宏定义
 * ======================================================================= */

/** RSA 公钥字数（2048 位 ÷ 32 位 = 64 字） */
#define OTA_RSA_WORD_COUNT      OTA_RSA_PUBLIC_KEY_WORD_COUNT

/** RSA 公钥模数字节数（256 字节） */
#define OTA_RSA_MODULUS_BYTES   OTA_RSA_PUBLIC_KEY_MODULUS_BYTES

/* =========================================================================
 *  3. 常量数据 —— SHA-256 自测向量与 DigestInfo 前缀
 * ======================================================================= */

/** SHA-256 自测输入："abc" */
static const uint8_t s_sha256_self_test_input[3] = { 'a', 'b', 'c' };

/** SHA-256 自测期望输出（NIST 标准向量） */
static const uint8_t s_sha256_self_test_output[32] =
{
    0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
    0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
    0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
    0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD
};

/**
 * SHA-256 DigestInfo 前缀（PKCS#1 v1.5 编码）
 * ASN.1 结构：SEQUENCE { SEQUENCE { OID, NULL }, OCTET STRING (32 字节哈希) }
 */
static const uint8_t s_sha256_digestinfo_prefix[19] =
{
    0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

/* =========================================================================
 *  4. 内部函数实现 —— 头部校验辅助
 * ======================================================================= */

/**
 * @brief  设置头部校验错误码并返回失败
 * @param  context — 校验上下文（可为 NULL）
 * @param  code    — YMODEM 错误码
 * @retval 0 — 始终返回失败
 */
static uint8_t iap_set_header_error(iap_header_validation_context_t *context, uint8_t code)
{
    if (context != 0)
    {
        context->error_code = code;
    }

    Ymodem_SetError(code, 2U);
    return 0U;
}

/**
 * @brief  重置认证诊断结构体
 * @param  diag — 诊断结构体指针（可为 NULL）
 */
static void iap_auth_diag_reset(iap_auth_diag_t *diag)
{
    if (diag != 0)
    {
        memset(diag, 0, sizeof(*diag));
    }
}

/* =========================================================================
 *  5. 内部函数实现 —— SHA-256 自测与 Flash 哈希
 * ======================================================================= */

/**
 * @brief  SHA-256 引擎自测
 * @note   使用 NIST 标准 "abc" 向量验证 SHA-256 计算正确性。
 * @retval 1 — 自测通过；0 — 自测失败
 */
static uint8_t ota_sha256_self_test(void)
{
    uint8_t hash[32];

    if (OtaSha256_Compute(s_sha256_self_test_input,
                          sizeof(s_sha256_self_test_input),
                          hash) == 0U)
    {
        return 0U;
    }

    return (memcmp(hash, s_sha256_self_test_output, sizeof(hash)) == 0) ? 1U : 0U;
}

/**
 * @brief  计算 Flash 中固件的 SHA-256 哈希
 * @param  firmware_address — 固件起始地址
 * @param  firmware_size    — 固件大小（字节）
 * @param  output           — 输出：32 字节哈希值
 * @retval 1 — 计算成功；0 — 参数非法
 */
static uint8_t iap_compute_flash_hash(uint32_t firmware_address,
                                      uint32_t firmware_size,
                                      uint8_t output[32])
{
    if (firmware_address < FLASH_APP1_ADDR ||
        firmware_size == 0U ||
        firmware_size > FLASH_APP_MAX_SIZE ||
        output == 0)
    {
        return 0U;
    }

    return OtaSha256_Compute((const uint8_t *)firmware_address, firmware_size, output);
}

/* =========================================================================
 *  6. 内部函数实现 —— RSA 大数运算（字级操作）
 * ======================================================================= */

/**
 * @brief  大数清零
 * @param  words — 大数数组（OTA_RSA_WORD_COUNT 个字）
 */
static void ota_rsa_words_zero(uint32_t *words)
{
    memset(words, 0, OTA_RSA_WORD_COUNT * sizeof(uint32_t));
}

/**
 * @brief  大数复制
 * @param  target — 目标数组
 * @param  source — 源数组
 */
static void ota_rsa_words_copy(uint32_t *target, const uint32_t *source)
{
    memcpy(target, source, OTA_RSA_WORD_COUNT * sizeof(uint32_t));
}

/**
 * @brief  大数比较（无符号，小端序）
 * @note   从最高有效字开始比较。
 * @param  left  — 大数 A
 * @param  right — 大数 B
 * @retval  1 — A > B
 * @retval  0 — A == B
 * @retval -1 — A < B
 */
static int32_t ota_rsa_words_compare(const uint32_t *left, const uint32_t *right)
{
    int32_t index = 0;

    for (index = (int32_t)OTA_RSA_WORD_COUNT - 1; index >= 0; --index)
    {
        if (left[index] > right[index])
        {
            return 1;
        }
        if (left[index] < right[index])
        {
            return -1;
        }
    }

    return 0;
}

/**
 * @brief  大数加法（无进位输出）
 * @note   target = target + source，返回最终进位。
 * @param  target — 被加数（原地修改）
 * @param  source — 加数
 * @return 最终进位（0 或 1）
 */
static uint32_t ota_rsa_words_add(uint32_t *target, const uint32_t *source)
{
    uint64_t carry = 0ULL;
    uint32_t index = 0U;

    for (index = 0U; index < OTA_RSA_WORD_COUNT; ++index)
    {
        uint64_t value = (uint64_t)target[index] + (uint64_t)source[index] + carry;
        target[index] = (uint32_t)value;
        carry = value >> 32;
    }

    return (uint32_t)carry;
}

/**
 * @brief  大数减法（原地，带借位）
 * @note   target = target - source，返回最终借位。
 *         要求 target >= source，否则结果无意义。
 * @param  target — 被减数（原地修改）
 * @param  source — 减数
 * @return 最终借位（0 或 1）
 */
static uint32_t ota_rsa_words_sub_inplace(uint32_t *target, const uint32_t *source)
{
    uint64_t borrow = 0ULL;
    uint32_t index = 0U;

    for (index = 0U; index < OTA_RSA_WORD_COUNT; ++index)
    {
        uint64_t left = (uint64_t)target[index];
        uint64_t right = (uint64_t)source[index] + borrow;

        if (left >= right)
        {
            target[index] = (uint32_t)(left - right);
            borrow = 0ULL;
        }
        else
        {
            target[index] = (uint32_t)(((uint64_t)1ULL << 32) + left - right);
            borrow = 1ULL;
        }
    }

    return (uint32_t)borrow;
}

/* =========================================================================
 *  7. 内部函数实现 —— RSA 模运算
 * ======================================================================= */

/**
 * @brief  模加法（原地）
 * @note   accumulator = (accumulator + addend) mod modulus
 *         使用阈值判断避免溢出：若 accumulator >= modulus - addend，
 *         则结果 = accumulator - (modulus - addend)；否则直接相加。
 * @param  accumulator — 累加器（原地修改）
 * @param  addend      — 加数
 * @param  modulus     — 模数
 */
static void ota_rsa_mod_add_inplace(uint32_t *accumulator,
                                    const uint32_t *addend,
                                    const uint32_t *modulus)
{
    uint32_t threshold[OTA_RSA_WORD_COUNT];

    /* threshold = modulus - addend */
    ota_rsa_words_copy(threshold, modulus);
    ota_rsa_words_sub_inplace(threshold, addend);

    if (ota_rsa_words_compare(accumulator, threshold) >= 0)
    {
        /* accumulator >= modulus - addend → 需要减模 */
        ota_rsa_words_sub_inplace(accumulator, threshold);
    }
    else
    {
        /* accumulator < modulus - addend → 直接相加 */
        ota_rsa_words_add(accumulator, addend);
    }
}

/**
 * @brief  模倍乘（原地）
 * @note   value = (value * 2) mod modulus
 *         若 value >= modulus - value，则结果 = value - (modulus - value)；
 *         否则 value = value + value。
 * @param  value   — 操作数（原地修改）
 * @param  modulus — 模数
 */
static void ota_rsa_mod_double_inplace(uint32_t *value, const uint32_t *modulus)
{
    uint32_t threshold[OTA_RSA_WORD_COUNT];

    /* threshold = modulus - value */
    ota_rsa_words_copy(threshold, modulus);
    ota_rsa_words_sub_inplace(threshold, value);

    if (ota_rsa_words_compare(value, threshold) >= 0)
    {
        /* value >= modulus - value → 需要减模 */
        ota_rsa_words_sub_inplace(value, threshold);
    }
    else
    {
        /* value < modulus - value → 直接加倍 */
        ota_rsa_words_add(value, value);
    }
}

/**
 * @brief  模乘法（二进制展开法）
 * @note   result = (left * right) mod modulus
 *         逐位扫描 right 的每一位，若为 1 则将当前 base 累加到结果中，
 *         然后将 base 加倍。时间复杂度 O(n^2)。
 * @param  result  — 输出：乘法结果
 * @param  left    — 乘数 A
 * @param  right   — 乘数 B
 * @param  modulus — 模数
 */
static void ota_rsa_mod_mul(uint32_t *result,
                            const uint32_t *left,
                            const uint32_t *right,
                            const uint32_t *modulus)
{
    uint32_t base[OTA_RSA_WORD_COUNT];
    uint32_t output[OTA_RSA_WORD_COUNT];
    uint32_t word_index = 0U;

    ota_rsa_words_zero(output);
    ota_rsa_words_copy(base, left);

    /* 逐字逐位扫描 right */
    for (word_index = 0U; word_index < OTA_RSA_WORD_COUNT; ++word_index)
    {
        uint32_t bit_value = right[word_index];
        uint32_t bit_index = 0U;

        for (bit_index = 0U; bit_index < 32U; ++bit_index)
        {
            if ((bit_value & 1U) != 0U)
            {
                ota_rsa_mod_add_inplace(output, base, modulus);
            }

            bit_value >>= 1U;
            ota_rsa_mod_double_inplace(base, modulus);
        }
    }

    ota_rsa_words_copy(result, output);
}

/* =========================================================================
 *  8. 内部函数实现 —— 大数与字节数组转换
 * ======================================================================= */

/**
 * @brief  大端字节数组 → 小端字数组
 * @note   将大端序字节数组转换为小端序 32 位字数组（低位在前）。
 * @param  words    — 输出：字数组
 * @param  bytes    — 输入：大端字节数组
 * @param  byte_len — 字节长度
 */
static void ota_rsa_words_from_bytes_be(uint32_t *words, const uint8_t *bytes, uint32_t byte_len)
{
    uint32_t offset = 0U;

    ota_rsa_words_zero(words);
    if (bytes == 0)
    {
        return;
    }

    for (offset = 0U; offset < byte_len && offset < OTA_RSA_MODULUS_BYTES; ++offset)
    {
        uint32_t source_index = byte_len - 1U - offset;    /* 大端逆序 */
        uint32_t word_index = offset / 4U;
        uint32_t byte_index = offset % 4U;
        words[word_index] |= ((uint32_t)bytes[source_index]) << (byte_index * 8U);
    }
}

/**
 * @brief  小端字数组 → 大端字节数组
 * @param  words    — 输入：字数组
 * @param  bytes    — 输出：大端字节数组
 * @param  byte_len — 字节长度
 */
static void ota_rsa_words_to_bytes_be(const uint32_t *words, uint8_t *bytes, uint32_t byte_len)
{
    uint32_t offset = 0U;

    if (words == 0 || bytes == 0)
    {
        return;
    }

    for (offset = 0U; offset < byte_len; ++offset)
    {
        uint32_t source_index = byte_len - 1U - offset;    /* 大端逆序 */
        uint32_t word_index = offset / 4U;
        uint32_t byte_index = offset % 4U;
        bytes[source_index] = (uint8_t)(words[word_index] >> (byte_index * 8U));
    }
}

/* =========================================================================
 *  9. 核心算法实现 —— RSA-2048 签名验证
 * ======================================================================= */

/**
 * @brief  RSA-2048 PKCS#1 v1.5 签名验证
 * @note   验证流程：
 *         1. 将签名转换为大数，检查 < 模数
 *         2. 模幂运算：encoded = signature^e mod n
 *         3. PKCS#1 v1.5 解码：检查 0x00 0x01 填充 → 0x00 分隔符
 *         4. 比对 DigestInfo 前缀 + 32 字节 SHA-256 哈希
 * @param  signature     — 签名字节数组（大端序）
 * @param  signature_len — 签名长度（必须为 256 字节）
 * @param  hash          — 预期的 32 字节 SHA-256 哈希
 * @retval 1 — 签名有效；0 — 签名无效或参数错误
 */
static uint8_t ota_rsa_verify_signature(const uint8_t *signature,
                                        uint32_t signature_len,
                                        const uint8_t hash[32])
{
    uint32_t signature_words[OTA_RSA_WORD_COUNT];
    uint32_t result_words[OTA_RSA_WORD_COUNT];
    uint32_t base_words[OTA_RSA_WORD_COUNT];
    uint32_t temp_words[OTA_RSA_WORD_COUNT];
    uint8_t encoded[OTA_RSA_MODULUS_BYTES];
    uint32_t exponent = OTA_RSA_PUBLIC_KEY_PUBLIC_EXPONENT;
    uint32_t offset = 0U;

    /* 参数校验 */
    if (signature == 0 || hash == 0 || signature_len != OTA_RSA_MODULUS_BYTES)
    {
        return 0U;
    }

    /* 将签名字节转换为大数 */
    ota_rsa_words_from_bytes_be(signature_words, signature, signature_len);

    /* 签名必须小于模数 */
    if (ota_rsa_words_compare(signature_words, s_ota_rsa_public_key_modulus_words) >= 0)
    {
        return 0U;
    }

    /* 模幂运算：result = signature^exponent mod modulus（平方-乘法法） */
    ota_rsa_words_zero(result_words);
    result_words[0] = 1U;
    ota_rsa_words_copy(base_words, signature_words);

    while (exponent != 0U)
    {
        if ((exponent & 1U) != 0U)
        {
            ota_rsa_mod_mul(temp_words, result_words, base_words, s_ota_rsa_public_key_modulus_words);
            ota_rsa_words_copy(result_words, temp_words);
        }

        exponent >>= 1U;
        if (exponent != 0U)
        {
            ota_rsa_mod_mul(temp_words, base_words, base_words, s_ota_rsa_public_key_modulus_words);
            ota_rsa_words_copy(base_words, temp_words);
        }
    }

    /* 将模幂结果转回字节数组 */
    ota_rsa_words_to_bytes_be(result_words, encoded, sizeof(encoded));

    /* PKCS#1 v1.5 解码：检查 0x00 0x01 头部 */
    if (encoded[0] != 0x00U || encoded[1] != 0x01U)
    {
        return 0U;
    }

    /* 跳过 0xFF 填充 */
    offset = 2U;
    while (offset < sizeof(encoded) && encoded[offset] == 0xFFU)
    {
        ++offset;
    }

    /* 检查填充长度和 0x00 分隔符 */
    if (offset < 10U || offset >= sizeof(encoded) || encoded[offset] != 0x00U)
    {
        return 0U;
    }

    ++offset;

    /* 验证剩余长度 = DigestInfo 前缀 + 32 字节哈希 */
    if (offset + sizeof(s_sha256_digestinfo_prefix) + 32U != sizeof(encoded))
    {
        return 0U;
    }

    /* 比对 DigestInfo 前缀 */
    if (memcmp(&encoded[offset], s_sha256_digestinfo_prefix, sizeof(s_sha256_digestinfo_prefix)) != 0)
    {
        return 0U;
    }

    /* 比对 SHA-256 哈希 */
    offset += sizeof(s_sha256_digestinfo_prefix);
    return (memcmp(&encoded[offset], hash, 32U) == 0) ? 1U : 0U;
}

/* =========================================================================
 *  10. 内部函数实现 —— 串口诊断输出
 * ======================================================================= */

/**
 * @brief  输出单字节十六进制值（2 个 ASCII 字符）
 * @param  value — 字节值
 */
static void iap_serial_put_hex_byte(uint8_t value)
{
    static const char s_hex[] = "0123456789ABCDEF";

    Send_Byte((uint8_t)s_hex[(value >> 4) & 0x0FU]);
    Send_Byte((uint8_t)s_hex[value & 0x0FU]);
}

/**
 * @brief  输出哈希前缀（标签 + 前 4 字节十六进制）
 * @param  label   — 标签字符串（如 "E="、"B="、"F="）
 * @param  hash    — 32 字节哈希值
 * @param  present — 是否有效（0 输出 "--------"）
 */
static void iap_serial_put_hash_prefix(const char *label,
                                       const uint8_t hash[32],
                                       uint8_t present)
{
    uint32_t index = 0U;

    Serial_PutString((uint8_t *)label);
    if (present == 0U)
    {
        Serial_PutString((uint8_t *)"--------");
        return;
    }

    /* 输出哈希前 4 字节（8 个十六进制字符） */
    for (index = 0U; index < 4U; ++index)
    {
        iap_serial_put_hex_byte(hash[index]);
    }
}

/**
 * @brief  输出 32 位无符号整数（十进制）
 * @param  value — 整数值
 */
static void iap_serial_put_u32(uint32_t value)
{
    uint8_t digits[10];
    uint32_t count = 0U;

    do
    {
        digits[count++] = (uint8_t)('0' + (value % 10U));
        value /= 10U;
    } while (value > 0U && count < sizeof(digits));

    while (count > 0U)
    {
        Send_Byte(digits[--count]);
    }
}

/**
 * @brief  获取事务加载来源的文本描述
 * @param  source — 来源码（OTA_CTRL_TXN_LOAD_SRC_xxx）
 * @return 描述字符串
 */
static const char *iap_txn_load_source_text(uint8_t source)
{
    switch (source)
    {
    case OTA_CTRL_TXN_LOAD_SRC_VALID:
        return "VALID";
    case OTA_CTRL_TXN_LOAD_SRC_EMPTY:
        return "EMPTY";
    case OTA_CTRL_TXN_LOAD_SRC_INVALID:
        return "INVALID";
    default:
        return "NONE";
    }
}

/* =========================================================================
 *  11. 公共接口实现 —— 头部校验
 * ======================================================================= */

/**
 * @brief  校验 OTA 镜像头部字段合法性
 * @note   校验内容：
 *         1. magic / header_version / header_size / signature_len
 *         2. format_version / signature_algorithm
 *         3. firmware_size 范围
 *         4. firmware_version / min_allowed_version 格式
 *         5. target_slot 与当前活跃分区的一致性
 *         6. firmware_version >= min_allowed_ota_version
 * @param  header  — OTA 镜像头部结构体指针
 * @param  context — 校验上下文（含 boot_info 和错误码输出）
 * @retval 1 — 校验通过；0 — 校验失败（错误码已设置）
 */
uint8_t iap_validate_received_header(const OtaImageHeaderBinary *header, void *context)
{
    iap_header_validation_context_t *validation = (iap_header_validation_context_t *)context;
    const BootInfoTypeDef *boot_info = (validation != 0) ? validation->boot_info : 0;
    uint32_t inactive_slot = OTA_CTRL_PARTITION_APP2;

    if (header == 0 || boot_info == 0)
    {
        return iap_set_header_error(validation, YMODEM_ERR_HEADER);
    }

    /* 校验信封字段：magic、版本、大小、签名长度 */
    if (header->envelope.magic != OTA_IMAGE_HEADER_MAGIC ||
        header->envelope.header_version != OTA_IMAGE_HEADER_VERSION ||
        header->envelope.header_size != OTA_IMAGE_HEADER_TOTAL_SIZE ||
        header->envelope.signature_len != OTA_IMAGE_SIGNATURE_LEN)
    {
        return iap_set_header_error(validation, YMODEM_ERR_HEADER);
    }

    /* 校验格式版本和签名算法 */
    if (header->payload.format_version != OTA_IMAGE_FORMAT_VERSION ||
        header->payload.signature_algorithm != OTA_IMAGE_SIG_ALG_RSA2048_SHA256_PKCS1V15)
    {
        return iap_set_header_error(validation, YMODEM_ERR_HEADER);
    }

    /* 校验固件大小范围 */
    if (header->payload.firmware_size == 0U ||
        header->payload.firmware_size > FLASH_APP_MAX_SIZE)
    {
        return iap_set_header_error(validation, YMODEM_ERR_SIZE);
    }

    /* 校验版本字符串格式 */
    if (version_text_is_valid(header->payload.firmware_version) == 0U ||
        version_text_is_valid(header->payload.min_allowed_version) == 0U)
    {
        return iap_set_header_error(validation, YMODEM_ERR_VERSION);
    }

    /* 校验目标分区槽位 */
    inactive_slot = boot_info_inactive_partition(boot_info->active_partition);
    if (header->payload.target_slot != boot_info->target_partition ||
        header->payload.target_slot != inactive_slot)
    {
        return iap_set_header_error(validation, YMODEM_ERR_SLOT);
    }

    /* 校验固件版本不低于最低允许 OTA 版本 */
    if (version_text_compare(header->payload.firmware_version,
                             boot_info->min_allowed_ota_version) < 0)
    {
        return iap_set_header_error(validation, YMODEM_ERR_VERSION);
    }

    return 1U;
}

/* =========================================================================
 *  12. 公共接口实现 —— 头部签名验证
 * ======================================================================= */

/**
 * @brief  验证 OTA 镜像头部的 RSA 签名
 * @note   对头部 payload 部分计算 SHA-256 哈希，
 *         然后使用 RSA-2048 公钥验证签名。
 * @param  header — OTA 镜像头部结构体指针
 * @retval 1 — 签名有效；0 — 签名无效或参数错误
 */
uint8_t iap_verify_image_header_signature(const OtaImageHeaderBinary *header)
{
    uint8_t payload_hash[32];

    if (header == 0)
    {
        return 0U;
    }

    /* 二次校验信封字段 */
    if (header->envelope.magic != OTA_IMAGE_HEADER_MAGIC ||
        header->envelope.header_version != OTA_IMAGE_HEADER_VERSION ||
        header->envelope.header_size != OTA_IMAGE_HEADER_TOTAL_SIZE ||
        header->envelope.signature_len != OTA_IMAGE_SIGNATURE_LEN)
    {
        return 0U;
    }

    /* 计算 payload 的 SHA-256 哈希 */
    if (OtaSha256_Compute((const uint8_t *)&header->payload,
                          OTA_IMAGE_HEADER_PAYLOAD_SIZE,
                          payload_hash) == 0U)
    {
        return 0U;
    }

    /* RSA 签名验证 */
    return ota_rsa_verify_signature(header->signature,
                                    header->envelope.signature_len,
                                    payload_hash);
}

/* =========================================================================
 *  13. 公共接口实现 —— 完整镜像授权
 * ======================================================================= */

/**
 * @brief  对已接收的 OTA 镜像执行完整授权验证
 * @note   验证流程：
 *         1. 固件大小一致性检查
 *         2. SHA-256 引擎自测
 *         3. 体哈希验证（传输中计算的哈希 vs 头部预期哈希）
 *         4. Flash 哈希验证（Flash 读取的哈希 vs 头部预期哈希）
 *         5. 头部 payload 签名 RSA-2048 验证
 * @param  boot_info       — BootInfo 信息
 * @param  header          — OTA 镜像头部
 * @param  firmware_address — 固件在 Flash 中的起始地址
 * @param  diag            — 输出：诊断信息
 * @return 认证结果（iap_auth_result_t 枚举值）
 */
iap_auth_result_t iap_authorize_received_image(const BootInfoTypeDef *boot_info,
                                               const OtaImageHeaderBinary *header,
                                               uint32_t firmware_address,
                                               iap_auth_diag_t *diag)
{
    const YmodemHashDiagnostics *hash_diag = 0;
    uint8_t payload_hash[32];

    iap_auth_diag_reset(diag);

    if (boot_info == 0 || header == 0 || diag == 0)
    {
        return IAP_AUTH_RESULT_INTERNAL;
    }

    /* 保存预期哈希到诊断结构 */
    memcpy(diag->expected_hash, header->payload.firmware_sha256, sizeof(diag->expected_hash));
    diag->has_expected = 1U;

    /* 步骤 1：固件大小一致性检查 */
    if (header->payload.firmware_size == 0U ||
        header->payload.firmware_size > FLASH_APP_MAX_SIZE ||
        Ymodem_GetReceivedFirmwareSize() != header->payload.firmware_size)
    {
        return IAP_AUTH_RESULT_SIZE;
    }

    /* 步骤 2：SHA-256 引擎自测 */
    if (ota_sha256_self_test() == 0U)
    {
        diag->hash_diag = IAP_HASH_DIAG_SELF_TEST;
        return IAP_AUTH_RESULT_HASH_ENGINE;
    }

    /* 步骤 3：体哈希验证 */
    hash_diag = Ymodem_GetHashDiagnostics();
    if (hash_diag == 0 || hash_diag->body_hash_state != YMODEM_BODY_HASH_OK)
    {
        diag->hash_diag = IAP_HASH_DIAG_BODY_CALC;
        return IAP_AUTH_RESULT_HASH_ENGINE;
    }

    memcpy(diag->body_hash, hash_diag->body_hash, sizeof(diag->body_hash));
    diag->has_body = 1U;

    /* 步骤 4：Flash 哈希验证 */
    if (iap_compute_flash_hash(firmware_address,
                               header->payload.firmware_size,
                               diag->flash_hash) == 0U)
    {
        diag->hash_diag = IAP_HASH_DIAG_FLASH_CALC;
        return IAP_AUTH_RESULT_HASH_ENGINE;
    }
    diag->has_flash = 1U;

    /* 体哈希 vs 预期哈希 */
    if (memcmp(diag->body_hash, diag->expected_hash, sizeof(diag->body_hash)) != 0)
    {
        if (memcmp(diag->body_hash, diag->flash_hash, sizeof(diag->body_hash)) == 0)
        {
            diag->hash_diag = IAP_HASH_DIAG_EXPECTED_MISMATCH;  /* body==flash≠expected */
        }
        else
        {
            diag->hash_diag = IAP_HASH_DIAG_BODY_MISMATCH;      /* body≠flash≠expected */
        }
        return IAP_AUTH_RESULT_HASH_MISMATCH;
    }

    /* Flash 哈希 vs 预期哈希 */
    if (memcmp(diag->flash_hash, diag->expected_hash, sizeof(diag->flash_hash)) != 0)
    {
        diag->hash_diag = IAP_HASH_DIAG_FLASH_MISMATCH;
        return IAP_AUTH_RESULT_HASH_MISMATCH;
    }

    /* 步骤 5：RSA-2048 头部签名验证 */
    if (OtaSha256_Compute((const uint8_t *)&header->payload,
                          OTA_IMAGE_HEADER_PAYLOAD_SIZE,
                          payload_hash) == 0U)
    {
        diag->hash_diag = IAP_HASH_DIAG_SELF_TEST;
        return IAP_AUTH_RESULT_HASH_ENGINE;
    }

    if (ota_rsa_verify_signature(header->signature,
                                 header->envelope.signature_len,
                                 payload_hash) == 0U)
    {
        return IAP_AUTH_RESULT_RSA;
    }

    (void)boot_info;
    return IAP_AUTH_RESULT_OK;
}

/* =========================================================================
 *  14. 公共接口实现 —— 诊断串口报告
 * ======================================================================= */

/**
 * @brief  获取认证结果的文本描述码
 * @param  result — 认证结果枚举
 * @param  diag   — 诊断信息（用于细化错误码）
 * @return 错误描述字符串（如 "AUTH RSA"、"AUTH HEXP"）
 */
const char *iap_auth_result_text(iap_auth_result_t result, const iap_auth_diag_t *diag)
{
    switch (result)
    {
    case IAP_AUTH_RESULT_SIZE:
        return "AUTH SIZE";
    case IAP_AUTH_RESULT_HASH_ENGINE:
        if (diag != 0)
        {
            if (diag->hash_diag == IAP_HASH_DIAG_BODY_CALC)
            {
                return "AUTH HBDC";
            }
            if (diag->hash_diag == IAP_HASH_DIAG_FLASH_CALC)
            {
                return "AUTH HFLC";
            }
            if (diag->hash_diag == IAP_HASH_DIAG_SELF_TEST)
            {
                return "AUTH HSTF";
            }
        }
        return "AUTH HMIS";
    case IAP_AUTH_RESULT_HASH_MISMATCH:
        if (diag != 0)
        {
            if (diag->hash_diag == IAP_HASH_DIAG_BODY_MISMATCH)
            {
                return "AUTH HBDY";
            }
            if (diag->hash_diag == IAP_HASH_DIAG_FLASH_MISMATCH)
            {
                return "AUTH HFLS";
            }
            if (diag->hash_diag == IAP_HASH_DIAG_EXPECTED_MISMATCH)
            {
                return "AUTH HEXP";
            }
        }
        return "AUTH HMIS";
    case IAP_AUTH_RESULT_RSA:
        return "AUTH RSA";
    default:
        return "AUTH INT";
    }
}

/**
 * @brief  串口输出认证结果码
 * @param  code — 结果码字符串
 */
void iap_serial_report_code(const char *code)
{
    if (code == 0)
    {
        return;
    }

    Serial_PutString((uint8_t *)"[BOOT] RES ");
    Serial_PutString((uint8_t *)code);
    Serial_PutString((uint8_t *)"\r\n");
}

/**
 * @brief  串口输出认证失败的完整诊断报告
 * @note   包含结果码和三组哈希前缀（Expected/Body/Flash）。
 * @param  result — 认证结果
 * @param  diag   — 诊断信息
 */
void iap_serial_report_auth_failure(iap_auth_result_t result, const iap_auth_diag_t *diag)
{
    iap_serial_report_code(iap_auth_result_text(result, diag));

    if (diag == 0)
    {
        return;
    }

    /* 输出三组哈希前缀：E=Expected, B=Body, F=Flash */
    Serial_PutString((uint8_t *)"[BOOT] SHA ");
    iap_serial_put_hash_prefix("E=", diag->expected_hash, diag->has_expected);
    Send_Byte(' ');
    iap_serial_put_hash_prefix("B=", diag->body_hash, diag->has_body);
    Send_Byte(' ');
    iap_serial_put_hash_prefix("F=", diag->flash_hash, diag->has_flash);
    Serial_PutString((uint8_t *)"\r\n");
}

/**
 * @brief  串口输出事务加载诊断报告
 * @param  txn  — OTA 事务记录
 * @param  diag — 事务加载诊断信息
 */
void iap_serial_report_txn_load(const OtaTxnRecord *txn, const iap_txn_load_diag_t *diag)
{
    if (txn == 0 || diag == 0)
    {
        return;
    }

    /* 输出事务基本信息 */
    Serial_PutString((uint8_t *)"[BOOT] TXN src=");
    Serial_PutString((uint8_t *)iap_txn_load_source_text(diag->source));

    Serial_PutString((uint8_t *)" st=");
    iap_serial_put_u32(txn->state);

    Serial_PutString((uint8_t *)" off=");
    iap_serial_put_u32(txn->resume_offset);

    Serial_PutString((uint8_t *)" ack=");
    iap_serial_put_u32(txn->last_acked_offset);

    Serial_PutString((uint8_t *)" ck=");
    iap_serial_put_u32(txn->checkpoint_size);

    Serial_PutString((uint8_t *)" proto=");
    iap_serial_put_u32(txn->protocol_version);

    Serial_PutString((uint8_t *)" inv=");
    iap_serial_put_u32(diag->invalid_slots);

    Serial_PutString((uint8_t *)" seq=");
    iap_serial_put_u32(diag->latest_seq);
    Serial_PutString((uint8_t *)"\r\n");

    /* 输出无效槽位详情 */
    if (diag->invalid_slots == 0U)
    {
        return;
    }

    Serial_PutString((uint8_t *)"[BOOT] TXNINV slot hdr=");
    iap_serial_put_u32(diag->invalid_slot_header);
    Serial_PutString((uint8_t *)" commit=");
    iap_serial_put_u32(diag->invalid_slot_commit);
    Serial_PutString((uint8_t *)" pcrc=");
    iap_serial_put_u32(diag->invalid_slot_payload_crc);
    Serial_PutString((uint8_t *)" pval=");
    iap_serial_put_u32(diag->invalid_slot_payload_validate);
    Serial_PutString((uint8_t *)"\r\n");

    /* 输出无效事务字段详情 */
    Serial_PutString((uint8_t *)"[BOOT] TXNINV pay lay=");
    iap_serial_put_u32(diag->invalid_txn_layout);
    Serial_PutString((uint8_t *)" st=");
    iap_serial_put_u32(diag->invalid_txn_state);
    Serial_PutString((uint8_t *)" part=");
    iap_serial_put_u32(diag->invalid_txn_partition);
    Serial_PutString((uint8_t *)" fld=");
    iap_serial_put_u32(diag->invalid_txn_fields);
    Serial_PutString((uint8_t *)" req=");
    iap_serial_put_u32(diag->invalid_txn_request_type);
    Serial_PutString((uint8_t *)" ver=");
    iap_serial_put_u32(diag->invalid_txn_version_text);
    Serial_PutString((uint8_t *)" off=");
    iap_serial_put_u32(diag->invalid_txn_offset);
    Serial_PutString((uint8_t *)" crc=");
    iap_serial_put_u32(diag->invalid_txn_data_crc);
    Serial_PutString((uint8_t *)"\r\n");
}

/**
 * @brief  串口输出 UART 错误标志报告
 * @param  flags — UART 错误标志位（UART_RX_RING_FLAG_xxx）
 */
void iap_serial_report_uart_flags(uint32_t flags)
{
    if (flags == 0U)
    {
        return;
    }

    Serial_PutString((uint8_t *)"[BOOT] UART");
    if ((flags & UART_RX_RING_FLAG_OVERFLOW) != 0U)
    {
        Serial_PutString((uint8_t *)" OVF");
    }
    if ((flags & UART_RX_RING_FLAG_ORE) != 0U)
    {
        Serial_PutString((uint8_t *)" ORE");
    }
    if ((flags & UART_RX_RING_FLAG_FE) != 0U)
    {
        Serial_PutString((uint8_t *)" FE");
    }
    if ((flags & UART_RX_RING_FLAG_NE) != 0U)
    {
        Serial_PutString((uint8_t *)" NE");
    }
    Serial_PutString((uint8_t *)"\r\n");
}
