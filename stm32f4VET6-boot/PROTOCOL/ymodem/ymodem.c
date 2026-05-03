/**
 * @file    ymodem.c
 * @brief   OTA 数据面接收器 —— 固件传输、解密、校验与断点续传
 * @note    本模块实现 OTA 数据平面的完整接收流程，包括：
 *          - SHA-256 哈希计算（FIPS 180-4）
 *          - CRC-32（IEEE 802.3）和 CRC-16/CCITT 校验
 *          - OTA 数据帧协议（START/CHUNK/FINISH/ACK/NAK/ABORT）
 *          - AES-128 CTR 模式解密
 *          - Flash 缓冲写入与扇区擦除
 *          - 检查点持久化（支持断点续传）
 *          - 固件体哈希计算（支持从 Flash 恢复）
 *          - 主接收循环（Ymodem_Receive）
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "flash_if.h"
#include "common.h"
#include "ymodem.h"
#include "iap.h"
#include "string.h"
#include "aes.h"
#include "delay.h"
#include "stm32f4xx_iwdg.h"

/* 外部函数声明 */
uint32_t Send_Byte(uint8_t c);

/* =========================================================================
 *  2. 接收状态码与超时参数
 * ======================================================================= */

/** 接收状态码 */
#define OTA_DATA_PACKET_OK         0    /**< 接收成功               */
#define OTA_DATA_PACKET_TIMEOUT   -1    /**< 接收超时               */
#define OTA_DATA_PACKET_CRC       -2    /**< CRC 校验失败           */
#define OTA_DATA_PACKET_UART      -3    /**< UART 通信错误          */

/** UART 接收超时计数上限 */
#define OTA_DATA_RX_TIMEOUT        (0x100000UL)

/** 轮询步进间隔（微秒） */
#define OTA_DATA_POLL_STEP_US      50U

/* =========================================================================
 *  3. 内部类型定义 —— OTA 数据帧
 * ======================================================================= */

/**
 * @brief  OTA 数据帧结构体
 */
typedef struct
{
    uint8_t type;                                   /**< 帧类型               */
    uint32_t session_id;                            /**< 会话 ID              */
    uint32_t offset;                                /**< 数据偏移量           */
    uint16_t payload_len;                           /**< 载荷长度             */
    uint8_t payload[OTA_DATA_MAX_PAYLOAD_LEN];      /**< 载荷数据             */
} OtaDataFrame;

/**
 * @brief  Ymodem 传输状态结构体
 */
typedef struct
{
    uint8_t *flash_buffer;                          /**< Flash 写入缓冲区     */
    uint32_t flash_buffer_len;                      /**< 缓冲区已填充长度     */
    uint32_t flash_destination;                     /**< 当前 Flash 写入目标地址 */
    uint32_t firmware_received;                     /**< 已接收固件字节数     */
    uint32_t durable_offset;                        /**< 已持久化的检查点偏移 */
    uint32_t start_offset;                          /**< 传输起始偏移（续传） */
    uint32_t checkpoint_size;                       /**< 检查点间隔           */
    uint32_t transfer_size;                         /**< 传输总大小           */
    uint32_t last_acked_offset;                     /**< 最后确认偏移量       */
    uint16_t chunk_size;                            /**< 单次传输块大小       */
    uint8_t flash_ready;                            /**< Flash 是否就绪       */
} YmodemTransferState;

/* =========================================================================
 *  4. 模块内部变量
 * ======================================================================= */

/** 错误码与诊断 */
static uint8_t ymodem_error_code = YMODEM_OK;
static uint8_t ymodem_error_stage = 0U;
static uint32_t ymodem_uart_error_flags = 0U;
static uint32_t ymodem_last_acked_offset = 0U;

/** 回调函数指针 */
static YmodemProgressCallback ymodem_progress_callback = 0;
static YmodemHeaderValidator ymodem_header_validator = 0;
static void *ymodem_header_validator_context = 0;
static YmodemCheckpointCallback ymodem_checkpoint_callback = 0;
static void *ymodem_checkpoint_context = 0;

/** 接收到的镜像头与有效性标志 */
static OtaImageHeaderBinary ymodem_received_header;
static uint8_t ymodem_received_header_valid = 0U;
static uint32_t ymodem_received_firmware_size = 0U;

/** 哈希诊断信息 */
static YmodemHashDiagnostics ymodem_hash_diagnostics;

/** 固件体哈希上下文 */
static uint8_t ymodem_body_hash_active = 0U;
static OtaSha256Context ymodem_body_hash_context;

/** 会话管理 */
static uint8_t ymodem_session_fingerprint[OTA_CTRL_FINGERPRINT_LEN];
static uint8_t ymodem_session_configured = 0U;
static uint32_t ymodem_session_id = 0U;
static uint32_t ymodem_session_start_offset = 0U;
static uint32_t ymodem_session_checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;

/** 检查点持久化策略参数 */
static const uint32_t s_ymodem_resume_first_persist_bytes = OTA_DATA_DEFAULT_CHUNK_SIZE;
static const uint32_t s_ymodem_resume_persist_stride_bytes = OTA_DATA_DEFAULT_CHECKPOINT_SIZE * 4U;

/* =========================================================================
 *  5. SHA-256 常量表（FIPS 180-4，前 64 个素数的立方根小数部分）
 * ======================================================================= */

static const uint32_t s_ota_sha256_k[64] =
{
    0x428A2F98UL, 0x71374491UL, 0xB5C0FBCFUL, 0xE9B5DBA5UL,
    0x3956C25BUL, 0x59F111F1UL, 0x923F82A4UL, 0xAB1C5ED5UL,
    0xD807AA98UL, 0x12835B01UL, 0x243185BEUL, 0x550C7DC3UL,
    0x72BE5D74UL, 0x80DEB1FEUL, 0x9BDC06A7UL, 0xC19BF174UL,
    0xE49B69C1UL, 0xEFBE4786UL, 0x0FC19DC6UL, 0x240CA1CCUL,
    0x2DE92C6FUL, 0x4A7484AAUL, 0x5CB0A9DCUL, 0x76F988DAUL,
    0x983E5152UL, 0xA831C66DUL, 0xB00327C8UL, 0xBF597FC7UL,
    0xC6E00BF3UL, 0xD5A79147UL, 0x06CA6351UL, 0x14292967UL,
    0x27B70A85UL, 0x2E1B2138UL, 0x4D2C6DFCUL, 0x53380D13UL,
    0x650A7354UL, 0x766A0ABBUL, 0x81C2C92EUL, 0x92722C85UL,
    0xA2BFE8A1UL, 0xA81A664BUL, 0xC24B8B70UL, 0xC76C51A3UL,
    0xD192E819UL, 0xD6990624UL, 0xF40E3585UL, 0x106AA070UL,
    0x19A4C116UL, 0x1E376C08UL, 0x2748774CUL, 0x34B0BCB5UL,
    0x391C0CB3UL, 0x4ED8AA4AUL, 0x5B9CCA4FUL, 0x682E6FF3UL,
    0x748F82EEUL, 0x78A5636FUL, 0x84C87814UL, 0x8CC70208UL,
    0x90BEFFFAUL, 0xA4506CEBUL, 0xBEF9A3F7UL, 0xC67178F2UL
};

/* =========================================================================
 *  6. Flash 扇区边界表
 * ======================================================================= */

/** STM32F405/F407 Flash 扇区起始地址表（12 个扇区 + 末尾哨兵） */
static const uint32_t s_ota_flash_sector_boundaries[] =
{
    ADDR_FLASH_SECTOR_0,
    ADDR_FLASH_SECTOR_1,
    ADDR_FLASH_SECTOR_2,
    ADDR_FLASH_SECTOR_3,
    ADDR_FLASH_SECTOR_4,
    ADDR_FLASH_SECTOR_5,
    ADDR_FLASH_SECTOR_6,
    ADDR_FLASH_SECTOR_7,
    ADDR_FLASH_SECTOR_8,
    ADDR_FLASH_SECTOR_9,
    ADDR_FLASH_SECTOR_10,
    ADDR_FLASH_SECTOR_11,
    0x08100000U                                     /* Flash 末尾哨兵地址 */
};

/* =========================================================================
 *  7. 内部函数前向声明
 * ======================================================================= */

static void Ymodem_ResetHashDiagnostics(void);
static void Ymodem_BodyHashBegin(void);
static uint8_t Ymodem_BodyHashResumeFromFlash(uint32_t address, uint32_t length);
static void Ymodem_BodyHashUpdate(const uint8_t *data, uint32_t data_len);
static void Ymodem_BodyHashFinish(void);
static void Ymodem_ResetSessionState(void);
static uint8_t Ymodem_WriteFlashBuffered(YmodemTransferState *state,
                                         const uint8_t *data,
                                         uint32_t data_len);
static uint8_t Ymodem_FlushFlashBuffered(YmodemTransferState *state);
static int32_t Receive_Byte(uint8_t *c, uint32_t timeout);
static void Ymodem_FeedWatchdog(void);
static void Ymodem_NotifyProgress(uint32_t current, uint32_t total);
static uint16_t Ymodem_Crc16(const uint8_t *data, uint32_t count);
static uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length);
static uint8_t ota_data_send_frame(uint8_t type,
                                   uint32_t session_id,
                                   uint32_t offset,
                                   const uint8_t *payload,
                                   uint16_t payload_len);
static int32_t ota_data_receive_frame(OtaDataFrame *frame, uint32_t timeout);
static void ota_data_send_ack(uint32_t session_id, uint32_t next_offset);
static void ota_data_send_nak(uint32_t session_id,
                              uint32_t retry_offset,
                              uint16_t reason_code,
                              uint16_t detail_code);
static void ota_data_send_abort(uint32_t session_id,
                                uint32_t final_offset,
                                uint8_t stage,
                                uint8_t error_class,
                                uint16_t error_code);
static uint32_t ota_data_session_id_from_fingerprint(const uint8_t fingerprint[OTA_CTRL_FINGERPRINT_LEN]);
static void ota_ctr_build_counter(uint8_t counter[BLOCKSIZE], const uint8_t iv[BLOCKSIZE], uint32_t block_index);
static void ota_ctr_crypt(uint8_t *buffer,
                          uint32_t length,
                          const uint8_t iv[BLOCKSIZE],
                          uint32_t offset);
static uint8_t ota_data_process_start_frame(const OtaDataFrame *frame, YmodemTransferState *state);
static uint8_t ota_data_process_chunk_frame(const OtaDataFrame *frame, YmodemTransferState *state);
static uint8_t ota_data_finalize_transfer(YmodemTransferState *state);
static uint8_t ota_data_maybe_persist_progress(YmodemTransferState *state);
static uint32_t ota_data_slot_end_exclusive(uint32_t address);
static uint32_t ota_data_sector_base(uint32_t address);
static uint32_t ota_data_next_sector_base(uint32_t address);
static uint8_t ota_data_error_is_terminal(uint8_t err_code);
static uint32_t ota_sha256_load_be32(const uint8_t *data);
static void ota_sha256_store_be32(uint32_t value, uint8_t *data);
static void ota_sha256_transform(OtaSha256Context *context, const uint8_t block[64]);

/* =========================================================================
 *  8. 公共接口 —— 错误码与状态查询
 * ======================================================================= */

/**
 * @brief  获取最后错误码
 * @return 错误码（YMODEM_OK / YMODEM_ERR_xxx）
 */
uint8_t Ymodem_GetErrorCode(void)
{
    return ymodem_error_code;
}

/**
 * @brief  获取最后错误发生的阶段
 * @return 阶段码
 */
uint8_t Ymodem_GetErrorStage(void)
{
    return ymodem_error_stage;
}

/**
 * @brief  获取 UART 错误标志
 * @return UART 错误标志位（OR 累积）
 */
uint32_t Ymodem_GetUartErrorFlags(void)
{
    return ymodem_uart_error_flags;
}

/**
 * @brief  获取最后确认的偏移量
 * @return 最后 ACK 的偏移量
 */
uint32_t Ymodem_GetLastAckedOffset(void)
{
    return ymodem_last_acked_offset;
}

/**
 * @brief  设置错误码和阶段
 * @param  code  — 错误码
 * @param  stage — 阶段码
 */
void Ymodem_SetError(uint8_t code, uint8_t stage)
{
    ymodem_error_code = code;
    ymodem_error_stage = stage;
}

/**
 * @brief  重置错误状态
 */
void Ymodem_ResetError(void)
{
    ymodem_error_code = YMODEM_OK;
    ymodem_error_stage = 0U;
    ymodem_uart_error_flags = 0U;
    ymodem_last_acked_offset = 0U;
}

/* =========================================================================
 *  9. 公共接口 —— 回调注册与信息查询
 * ======================================================================= */

/**
 * @brief  注册进度回调函数
 * @param  callback — 进度回调指针
 */
void Ymodem_SetProgressCallback(YmodemProgressCallback callback)
{
    ymodem_progress_callback = callback;
}

/**
 * @brief  注册镜像头校验回调
 * @param  validator — 校验函数指针
 * @param  context   — 校验上下文
 */
void Ymodem_SetHeaderValidator(YmodemHeaderValidator validator, void *context)
{
    ymodem_header_validator = validator;
    ymodem_header_validator_context = context;
}

/**
 * @brief  获取接收到的镜像头指针
 * @return 镜像头指针（若有效）；NULL（若未接收）
 */
const OtaImageHeaderBinary *Ymodem_GetReceivedHeader(void)
{
    return (ymodem_received_header_valid != 0U) ? &ymodem_received_header : 0;
}

/**
 * @brief  获取接收到的固件大小
 * @return 固件大小（字节）
 */
uint32_t Ymodem_GetReceivedFirmwareSize(void)
{
    return ymodem_received_firmware_size;
}

/**
 * @brief  获取哈希诊断信息
 * @return 哈希诊断结构体指针
 */
const YmodemHashDiagnostics *Ymodem_GetHashDiagnostics(void)
{
    return &ymodem_hash_diagnostics;
}

/* =========================================================================
 *  10. 内部函数 —— 进度通知
 * ======================================================================= */

/**
 * @brief  调用进度回调（若已注册）
 * @param  current — 当前已接收字节数
 * @param  total   — 总字节数
 */
static void Ymodem_NotifyProgress(uint32_t current, uint32_t total)
{
    if (ymodem_progress_callback != 0)
    {
        ymodem_progress_callback(current, total);
    }
}

/* =========================================================================
 *  11. SHA-256 实现（FIPS 180-4）
 * ======================================================================= */

/**
 * @brief  SHA-256 循环右移 32 位
 * @param  value — 待移位的值
 * @param  shift — 移位位数（0~31）
 * @return 移位结果
 */
static uint32_t ota_sha256_rotr32(uint32_t value, uint32_t shift)
{
    return (value >> shift) | (value << (32U - shift));
}

/**
 * @brief  从字节数组加载大端序 32 位整数
 * @param  data — 字节数组（4 字节）
 * @return 大端序 32 位整数
 */
static uint32_t ota_sha256_load_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

/**
 * @brief  将 32 位整数以大端序写入字节数组
 * @param  value — 待写入的值
 * @param  data  — 目标字节数组（4 字节）
 */
static void ota_sha256_store_be32(uint32_t value, uint8_t *data)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

/**
 * @brief  SHA-256 压缩函数（处理单个 512 位块）
 * @note   FIPS 180-4 Section 6.2.2：
 *         1. 消息调度：将 16 个 32 位字扩展为 64 个
 *         2. 64 轮压缩：使用 Ch、Maj、Sigma 函数更新工作变量
 *         3. 将压缩结果加到当前哈希值
 * @param  context — SHA-256 上下文
 * @param  block   — 64 字节输入块
 */
static void ota_sha256_transform(OtaSha256Context *context, const uint8_t block[64])
{
    uint32_t w[64];                                 /* 消息调度数组 */
    uint32_t a = context->state[0];                 /* 工作变量 a~h */
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];
    uint32_t index = 0U;

    /* 步骤 1：加载前 16 个字（大端序） */
    for (index = 0U; index < 16U; ++index)
    {
        w[index] = ota_sha256_load_be32(block + (index * 4U));
    }

    /* 步骤 2：扩展到 64 个字 */
    for (index = 16U; index < 64U; ++index)
    {
        /* sigma0(x) = ROTR^7(x) XOR ROTR^18(x) XOR SHR^3(x) */
        uint32_t s0 = ota_sha256_rotr32(w[index - 15U], 7U) ^
                      ota_sha256_rotr32(w[index - 15U], 18U) ^
                      (w[index - 15U] >> 3);
        /* sigma1(x) = ROTR^17(x) XOR ROTR^19(x) XOR SHR^10(x) */
        uint32_t s1 = ota_sha256_rotr32(w[index - 2U], 17U) ^
                      ota_sha256_rotr32(w[index - 2U], 19U) ^
                      (w[index - 2U] >> 10);
        w[index] = w[index - 16U] + s0 + w[index - 7U] + s1;
    }

    /* 步骤 3：64 轮压缩 */
    for (index = 0U; index < 64U; ++index)
    {
        /* Sigma1(e) = ROTR^6(e) XOR ROTR^11(e) XOR ROTR^25(e) */
        uint32_t s1 = ota_sha256_rotr32(e, 6U) ^
                      ota_sha256_rotr32(e, 11U) ^
                      ota_sha256_rotr32(e, 25U);
        /* Ch(e,f,g) = (e AND f) XOR (NOT e AND g) */
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + s_ota_sha256_k[index] + w[index];
        /* Sigma0(a) = ROTR^2(a) XOR ROTR^13(a) XOR ROTR^22(a) */
        uint32_t s0 = ota_sha256_rotr32(a, 2U) ^
                      ota_sha256_rotr32(a, 13U) ^
                      ota_sha256_rotr32(a, 22U);
        /* Maj(a,b,c) = (a AND b) XOR (a AND c) XOR (b AND c) */
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        /* 更新工作变量 */
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    /* 步骤 4：累加到当前哈希值 */
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

/* =========================================================================
 *  12. 公共接口 —— SHA-256 初始化/更新/终结
 * ======================================================================= */

/**
 * @brief  初始化 SHA-256 上下文
 * @note   设置初始哈希值（FIPS 180-4 Section 5.3.3）：
 *         H0 = 0x6A09E667, H1 = 0xBB67AE85, ...
 * @param  context — SHA-256 上下文指针
 */
void OtaSha256_Init(OtaSha256Context *context)
{
    if (context == 0)
    {
        return;
    }

    memset(context, 0, sizeof(*context));
    context->state[0] = 0x6A09E667UL;
    context->state[1] = 0xBB67AE85UL;
    context->state[2] = 0x3C6EF372UL;
    context->state[3] = 0xA54FF53AUL;
    context->state[4] = 0x510E527FUL;
    context->state[5] = 0x9B05688CUL;
    context->state[6] = 0x1F83D9ABUL;
    context->state[7] = 0x5BE0CD19UL;
}

/**
 * @brief  向 SHA-256 上下文追加数据
 * @note   将数据逐块（64 字节）填入缓冲区，满时触发压缩。
 * @param  context  — SHA-256 上下文
 * @param  data     — 输入数据
 * @param  data_len — 数据长度
 */
void OtaSha256_Update(OtaSha256Context *context, const uint8_t *data, uint32_t data_len)
{
    uint32_t offset = 0U;

    if (context == 0 || data == 0 || data_len == 0U)
    {
        return;
    }

    while (offset < data_len)
    {
        /* 计算本次可填充的字节数 */
        uint32_t copy_len = 64U - (uint32_t)context->buffer_len;
        if (copy_len > (data_len - offset))
        {
            copy_len = data_len - offset;
        }

        /* 追加到缓冲区 */
        memcpy(context->buffer + context->buffer_len, data + offset, copy_len);
        context->buffer_len = (uint8_t)(context->buffer_len + copy_len);
        context->total_len += copy_len;
        offset += copy_len;

        /* 缓冲区满 64 字节时触发压缩 */
        if (context->buffer_len == 64U)
        {
            ota_sha256_transform(context, context->buffer);
            context->buffer_len = 0U;
        }
    }
}

/**
 * @brief  终结 SHA-256 计算，输出 32 字节哈希值
 * @note   FIPS 180-4 填充规则：
 *         1. 追加 0x80 字节
 *         2. 填充 0x00 直至剩余 8 字节
 *         3. 追加原始消息长度（大端序 64 位）
 *         4. 最终压缩，输出 8 个 32 位状态字（大端序）
 * @param  context — SHA-256 上下文
 * @param  output  — 输出缓冲区（32 字节）
 */
void OtaSha256_Final(OtaSha256Context *context, uint8_t output[32])
{
    uint64_t bit_len = 0ULL;
    uint32_t index = 0U;

    if (context == 0 || output == 0)
    {
        return;
    }

    /* 计算原始消息的比特长度 */
    bit_len = ((uint64_t)context->total_len) * 8ULL;

    /* 追加 0x80 填充字节 */
    context->buffer[context->buffer_len++] = 0x80U;

    /* 若剩余空间不足 8 字节，先压缩一次 */
    if (context->buffer_len > 56U)
    {
        while (context->buffer_len < 64U)
        {
            context->buffer[context->buffer_len++] = 0U;
        }
        ota_sha256_transform(context, context->buffer);
        context->buffer_len = 0U;
    }

    /* 填充 0x00 直至第 56 字节 */
    while (context->buffer_len < 56U)
    {
        context->buffer[context->buffer_len++] = 0U;
    }

    /* 追加消息长度（大端序 64 位） */
    for (index = 0U; index < 8U; ++index)
    {
        context->buffer[56U + index] = (uint8_t)(bit_len >> ((7U - index) * 8U));
    }

    /* 最终压缩 */
    ota_sha256_transform(context, context->buffer);

    /* 输出哈希值（大端序） */
    for (index = 0U; index < 8U; ++index)
    {
        ota_sha256_store_be32(context->state[index], output + (index * 4U));
    }
}

/**
 * @brief  一次性计算 SHA-256 哈希（便捷函数）
 * @param  data   — 输入数据
 * @param  length — 数据长度
 * @param  output — 输出缓冲区（32 字节）
 * @retval 1 — 成功；0 — 参数错误
 */
uint8_t OtaSha256_Compute(const uint8_t *data, uint32_t length, uint8_t output[32])
{
    OtaSha256Context context;

    if (data == 0 || output == 0)
    {
        return 0U;
    }

    OtaSha256_Init(&context);
    OtaSha256_Update(&context, data, length);
    OtaSha256_Final(&context, output);
    return 1U;
}

/* =========================================================================
 *  13. 内部函数 —— 固件体哈希管理
 * ======================================================================= */

/**
 * @brief  重置哈希诊断信息
 */
static void Ymodem_ResetHashDiagnostics(void)
{
    memset(&ymodem_hash_diagnostics, 0, sizeof(ymodem_hash_diagnostics));
}

/**
 * @brief  开始固件体哈希计算
 */
static void Ymodem_BodyHashBegin(void)
{
    Ymodem_ResetHashDiagnostics();
    OtaSha256_Init(&ymodem_body_hash_context);
    ymodem_body_hash_active = 1U;
}

/**
 * @brief  从 Flash 已有数据恢复体哈希计算（续传场景）
 * @note   将 Flash 中已写入的数据纳入哈希计算，
 *         确保续传后的体哈希与完整传输一致。
 * @param  address — Flash 起始地址
 * @param  length  — 数据长度
 * @retval 1 — 成功；0 — 地址越界
 */
static uint8_t Ymodem_BodyHashResumeFromFlash(uint32_t address, uint32_t length)
{
    if (length == 0U)
    {
        return 1U;
    }

    /* 地址合法性检查 */
    if (address < FLASH_APP1_ADDR || length > FLASH_APP_MAX_SIZE)
    {
        return 0U;
    }

    /* 直接从 Flash 读取数据更新哈希 */
    OtaSha256_Update(&ymodem_body_hash_context, (const uint8_t *)address, length);
    return 1U;
}

/**
 * @brief  更新固件体哈希（接收新数据时调用）
 * @param  data     — 新接收的明文数据
 * @param  data_len — 数据长度
 */
static void Ymodem_BodyHashUpdate(const uint8_t *data, uint32_t data_len)
{
    if (ymodem_body_hash_active == 0U || data == 0 || data_len == 0U)
    {
        return;
    }

    OtaSha256_Update(&ymodem_body_hash_context, data, data_len);
}

/**
 * @brief  完成固件体哈希计算，输出最终哈希值
 */
static void Ymodem_BodyHashFinish(void)
{
    if (ymodem_body_hash_active == 0U)
    {
        return;
    }

    OtaSha256_Final(&ymodem_body_hash_context, ymodem_hash_diagnostics.body_hash);
    ymodem_hash_diagnostics.body_hash_state = YMODEM_BODY_HASH_OK;
    ymodem_body_hash_active = 0U;
}

/* =========================================================================
 *  14. 内部函数 —— CRC-32（IEEE 802.3）
 * ======================================================================= */

/**
 * @brief  CRC-32 增量更新（IEEE 802.3 多项式 0xEDB88320）
 * @note   逐字节处理，每字节逐位移位异或。
 *         初始值取反，最终结果取反。
 * @param  crc    — 前一次 CRC 值（初始为 0）
 * @param  data   — 数据指针
 * @param  length — 数据长度
 * @return 更新后的 CRC-32 值
 */
static uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    uint32_t value = ~crc;                          /* 取反初始化 */
    uint32_t index = 0U;

    for (index = 0U; index < length; ++index)
    {
        uint32_t bit = 0U;

        value ^= data[index];                       /* 异或当前字节 */
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((value & 1U) != 0U)
            {
                value = (value >> 1) ^ 0xEDB88320UL; /* LSB 优先，异或多项式 */
            }
            else
            {
                value >>= 1;
            }
        }
    }

    return ~value;                                  /* 最终取反 */
}

/* =========================================================================
 *  15. 内部函数 —— 会话 ID 生成
 * ======================================================================= */

/**
 * @brief  从会话指纹生成 32 位会话 ID
 * @note   对 32 字节指纹计算 CRC-32 作为会话标识。
 * @param  fingerprint — 会话指纹（32 字节）
 * @return 32 位会话 ID
 */
static uint32_t ota_data_session_id_from_fingerprint(const uint8_t fingerprint[OTA_CTRL_FINGERPRINT_LEN])
{
    return ota_crc32_update(0U, fingerprint, OTA_CTRL_FINGERPRINT_LEN);
}

/* =========================================================================
 *  16. 内部函数 —— 会话状态重置
 * ======================================================================= */

/**
 * @brief  重置 Ymodem 会话状态（清除镜像头、指纹、哈希上下文等）
 */
static void Ymodem_ResetSessionState(void)
{
    memset(&ymodem_received_header, 0, sizeof(ymodem_received_header));
    memset(ymodem_session_fingerprint, 0, sizeof(ymodem_session_fingerprint));
    ymodem_received_header_valid = 0U;
    ymodem_received_firmware_size = 0U;
    ymodem_body_hash_active = 0U;
    ymodem_session_configured = 0U;
    ymodem_session_id = 0U;
    ymodem_session_start_offset = 0U;
    ymodem_session_checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    ymodem_checkpoint_callback = 0;
    ymodem_checkpoint_context = 0;
    memset(&ymodem_body_hash_context, 0, sizeof(ymodem_body_hash_context));
    Ymodem_ResetHashDiagnostics();
}

/* =========================================================================
 *  17. 公共接口 —— 传输配置
 * ======================================================================= */

/**
 * @brief  配置 Ymodem 传输参数
 * @note   在调用 Ymodem_Receive 之前必须先调用本函数。
 *         验证内容：
 *         1. 镜像头指纹一致性（SHA-256）
 *         2. 固件大小合法性
 *         3. 续传偏移量对齐（BLOCKSIZE 对齐）
 *         4. 镜像头校验回调
 * @param  header              — 固件镜像头
 * @param  session_fingerprint — 会话指纹（32 字节）
 * @param  start_offset        — 起始偏移量（0=全新传输）
 * @param  checkpoint_size     — 检查点间隔
 * @param  checkpoint_callback — 检查点回调函数
 * @param  checkpoint_context  — 检查点回调上下文
 * @retval 1 — 配置成功；0 — 配置失败
 */
uint8_t Ymodem_ConfigureTransfer(const OtaImageHeaderBinary *header,
                                 const uint8_t session_fingerprint[32],
                                 uint32_t start_offset,
                                 uint32_t checkpoint_size,
                                 YmodemCheckpointCallback checkpoint_callback,
                                 void *checkpoint_context)
{
    uint8_t computed_fingerprint[OTA_CTRL_FINGERPRINT_LEN];

    /* 重置状态与错误 */
    Ymodem_ResetSessionState();
    Ymodem_ResetError();

    /* 参数合法性检查 */
    if (header == 0 || session_fingerprint == 0)
    {
        Ymodem_SetError(YMODEM_ERR_HEADER, 1U);
        return 0U;
    }

    /* 固件大小与偏移量校验 */
    if (header->payload.firmware_size == 0U ||
        header->payload.firmware_size > FLASH_APP_MAX_SIZE ||
        start_offset > header->payload.firmware_size ||
        (start_offset % BLOCKSIZE) != 0U ||
        (start_offset != 0U && checkpoint_size == 0U))
    {
        Ymodem_SetError(YMODEM_ERR_SIZE, 1U);
        return 0U;
    }

    /* 检查点间隔默认值 */
    if (checkpoint_size == 0U)
    {
        checkpoint_size = OTA_DATA_DEFAULT_CHECKPOINT_SIZE;
    }

    /* 验证镜像头指纹（防止篡改） */
    if (OtaSha256_Compute((const uint8_t *)header,
                          OTA_IMAGE_HEADER_TOTAL_SIZE,
                          computed_fingerprint) == 0U ||
        memcmp(computed_fingerprint, session_fingerprint, OTA_CTRL_FINGERPRINT_LEN) != 0)
    {
        Ymodem_SetError(YMODEM_ERR_HEADER, 1U);
        return 0U;
    }

    /* 保存镜像头与会话信息 */
    memcpy(&ymodem_received_header, header, sizeof(ymodem_received_header));
    memcpy(ymodem_session_fingerprint, session_fingerprint, sizeof(ymodem_session_fingerprint));
    ymodem_received_header_valid = 1U;
    ymodem_received_firmware_size = 0U;
    ymodem_session_id = ota_data_session_id_from_fingerprint(ymodem_session_fingerprint);
    ymodem_session_start_offset = start_offset;
    ymodem_session_checkpoint_size = checkpoint_size;
    ymodem_checkpoint_callback = checkpoint_callback;
    ymodem_checkpoint_context = checkpoint_context;

    /* 调用镜像头校验回调（若已注册） */
    if (ymodem_header_validator != 0 &&
        ymodem_header_validator(&ymodem_received_header,
                                ymodem_header_validator_context) == 0U)
    {
        if (Ymodem_GetErrorCode() == YMODEM_OK)
        {
            Ymodem_SetError(YMODEM_ERR_HEADER, 1U);
        }
        return 0U;
    }

    ymodem_last_acked_offset = start_offset;
    ymodem_session_configured = 1U;
    return 1U;
}

/* =========================================================================
 *  18. 内部函数 —— Flash 缓冲写入
 * ======================================================================= */

/**
 * @brief  将数据写入 Flash（带内部缓冲区）
 * @note   数据先填充到内部缓冲区（OTA_DATA_MAX_PAYLOAD_LEN 字节），
 *         缓冲区满时调用 FLASH_If_Write 写入 Flash。
 *         确保写入操作以 4 字节对齐。
 * @param  state    — 传输状态
 * @param  data     — 待写入数据
 * @param  data_len — 数据长度
 * @retval 1 — 成功；0 — 写入失败
 */
static uint8_t Ymodem_WriteFlashBuffered(YmodemTransferState *state,
                                         const uint8_t *data,
                                         uint32_t data_len)
{
    if (state == 0 || state->flash_buffer == 0 || data == 0)
    {
        return 0U;
    }

    while (data_len > 0U)
    {
        /* 计算缓冲区剩余空间 */
        uint32_t space = OTA_DATA_MAX_PAYLOAD_LEN - state->flash_buffer_len;
        uint32_t copy_len = (data_len < space) ? data_len : space;

        /* 追加到缓冲区 */
        memcpy(state->flash_buffer + state->flash_buffer_len, data, copy_len);
        state->flash_buffer_len += copy_len;
        data += copy_len;
        data_len -= copy_len;

        /* 缓冲区满时写入 Flash */
        if (state->flash_buffer_len == OTA_DATA_MAX_PAYLOAD_LEN)
        {
            if (FLASH_If_Write(&state->flash_destination,
                               (uint32_t *)state->flash_buffer,
                               OTA_DATA_MAX_PAYLOAD_LEN / 4U) != 0U)
            {
                return 0U;
            }
            state->flash_buffer_len = 0U;
        }
    }

    return 1U;
}

/**
 * @brief  刷新 Flash 写入缓冲区（将残余数据写入 Flash）
 * @note   若缓冲区有残余数据，补 0xFF 对齐到 4 字节后写入。
 * @param  state — 传输状态
 * @retval 1 — 成功；0 — 写入失败
 */
static uint8_t Ymodem_FlushFlashBuffered(YmodemTransferState *state)
{
    uint32_t padded_len = 0U;

    if (state == 0 || state->flash_buffer == 0)
    {
        return 0U;
    }

    /* 缓冲区为空则直接返回 */
    if (state->flash_buffer_len == 0U)
    {
        return 1U;
    }

    /* 计算 4 字节对齐的填充长度 */
    padded_len = (state->flash_buffer_len + 3U) & ~3U;
    if (padded_len > state->flash_buffer_len)
    {
        /* 用 0xFF 填充对齐部分（Flash 擦除后的默认值） */
        memset(state->flash_buffer + state->flash_buffer_len,
               0xFF,
               padded_len - state->flash_buffer_len);
    }

    /* 写入 Flash */
    if (FLASH_If_Write(&state->flash_destination,
                       (uint32_t *)state->flash_buffer,
                       padded_len / 4U) != 0U)
    {
        return 0U;
    }

    state->flash_buffer_len = 0U;
    return 1U;
}

/* =========================================================================
 *  19. 内部函数 —— 看门狗喂狗
 * ======================================================================= */

/**
 * @brief  喂独立看门狗（IWDG）
 */
static void Ymodem_FeedWatchdog(void)
{
    IWDG_ReloadCounter();
}

/* =========================================================================
 *  20. 内部函数 —— 带超时的字节接收
 * ======================================================================= */

/**
 * @brief  从串口接收单个字节（带超时与 UART 错误检测）
 * @note   以 OTA_DATA_POLL_STEP_US 步进轮询，每次轮询时检查 UART 错误标志。
 *         接收期间持续喂狗。
 * @param  c       — 接收字节输出指针
 * @param  timeout — 超时时间（毫秒）
 * @return 接收状态码（OTA_DATA_PACKET_OK / TIMEOUT / UART）
 */
static int32_t Receive_Byte(uint8_t *c, uint32_t timeout)
{
    uint32_t waited_us = 0U;
    uint32_t timeout_us = timeout * 1000U;

    if (c == 0)
    {
        return OTA_DATA_PACKET_TIMEOUT;
    }

    /* 轮询等待 */
    while (waited_us < timeout_us)
    {
        /* 检查是否有数据到达 */
        if (SerialKeyPressed(c) != 0U)
        {
            return OTA_DATA_PACKET_OK;
        }

        /* 检查 UART 错误标志 */
        ymodem_uart_error_flags |= SerialTakeRxErrorFlags();
        if (ymodem_uart_error_flags != 0U)
        {
            return OTA_DATA_PACKET_UART;
        }

        /* 喂狗并步进 */
        Ymodem_FeedWatchdog();
        delay_us(OTA_DATA_POLL_STEP_US);
        waited_us += OTA_DATA_POLL_STEP_US;
    }

    /* 超时前再次检查 UART 错误 */
    ymodem_uart_error_flags |= SerialTakeRxErrorFlags();
    if (ymodem_uart_error_flags != 0U)
    {
        return OTA_DATA_PACKET_UART;
    }

    return OTA_DATA_PACKET_TIMEOUT;
}

/* =========================================================================
 *  21. 内部函数 —— CRC-16/CCITT
 * ======================================================================= */

/**
 * @brief  计算 CRC-16/CCITT 校验值
 * @note   多项式 0x1021，初始值 0x0000，MSB 优先。
 * @param  data  — 数据指针
 * @param  count — 数据长度
 * @return CRC-16 校验值
 */
static uint16_t Ymodem_Crc16(const uint8_t *data, uint32_t count)
{
    uint16_t crc = 0U;
    uint32_t index = 0U;
    uint32_t bit = 0U;

    if (data == 0)
    {
        return 0U;
    }

    for (index = 0U; index < count; ++index)
    {
        crc ^= (uint16_t)data[index] << 8;
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((crc & 0x8000U) != 0U)
            {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

/* =========================================================================
 *  22. 内部函数 —— 小端序序列化/反序列化
 * ======================================================================= */

/**
 * @brief  将 16 位无符号整数写入缓冲区（小端序）
 */
static void ota_data_write_u16le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/**
 * @brief  将 32 位无符号整数写入缓冲区（小端序）
 */
static void ota_data_write_u32le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/**
 * @brief  从缓冲区读取 16 位无符号整数（小端序）
 */
static uint16_t ota_data_read_u16le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

/**
 * @brief  从缓冲区读取 32 位无符号整数（小端序）
 */
static uint32_t ota_data_read_u32le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

/* =========================================================================
 *  23. 内部函数 —— OTA 数据帧发送
 * ======================================================================= */

/**
 * @brief  组装并发送 OTA 数据帧
 * @note   帧格式：
 *         | SOF1 | SOF2 | VER | TYPE | SID(LE32) | OFFSET(LE32) | LEN(LE16) | HDR_CRC16 | PAYLOAD | PLD_CRC32 |
 *         - 头部 CRC 覆盖：VER 到 LEN（12 字节）
 *         - 载荷 CRC 覆盖：整个载荷
 * @param  type        — 帧类型
 * @param  session_id  — 会话 ID
 * @param  offset      — 数据偏移量
 * @param  payload     — 载荷数据
 * @param  payload_len — 载荷长度
 * @retval 1 — 发送成功；0 — 载荷超长
 */
static uint8_t ota_data_send_frame(uint8_t type,
                                   uint32_t session_id,
                                   uint32_t offset,
                                   const uint8_t *payload,
                                   uint16_t payload_len)
{
    uint8_t frame[OTA_DATA_MAX_FRAME_LEN];
    uint16_t header_crc = 0U;
    uint32_t payload_crc = 0U;
    uint16_t index = 0U;
    uint16_t total_len = 0U;

    if (payload_len > OTA_DATA_MAX_PAYLOAD_LEN)
    {
        return 0U;
    }

    /* 组装帧头 */
    frame[0] = OTA_DATA_SOF1;
    frame[1] = OTA_DATA_SOF2;
    frame[2] = OTA_DATA_PROTOCOL_VERSION;
    frame[3] = type;
    ota_data_write_u32le(&frame[4], session_id);
    ota_data_write_u32le(&frame[8], offset);
    ota_data_write_u16le(&frame[12], payload_len);

    /* 计算并写入头部 CRC */
    header_crc = Ymodem_Crc16(&frame[2], 12U);
    ota_data_write_u16le(&frame[14], header_crc);

    /* 复制载荷 */
    if (payload_len > 0U && payload != 0)
    {
        memcpy(&frame[OTA_DATA_FIXED_HEADER_LEN], payload, payload_len);
    }

    /* 计算并写入载荷 CRC */
    payload_crc = ota_crc32_update(0U, payload, payload_len);
    ota_data_write_u32le(&frame[OTA_DATA_FIXED_HEADER_LEN + payload_len], payload_crc);

    /* 逐字节发送整个帧 */
    total_len = (uint16_t)(OTA_DATA_FIXED_HEADER_LEN + payload_len + OTA_DATA_TRAILER_LEN);
    for (index = 0U; index < total_len; ++index)
    {
        Send_Byte(frame[index]);
    }

    return 1U;
}

/* =========================================================================
 *  24. 内部函数 —— ACK / NAK / ABORT 发送
 * ======================================================================= */

/**
 * @brief  发送 ACK 帧
 * @param  session_id  — 会话 ID
 * @param  next_offset — 期望的下一个偏移量
 */
static void ota_data_send_ack(uint32_t session_id, uint32_t next_offset)
{
    (void)ota_data_send_frame(OTA_DATA_TYPE_ACK, session_id, next_offset, 0, 0U);
}

/**
 * @brief  发送 NAK 帧（请求重传）
 * @param  session_id   — 会话 ID
 * @param  retry_offset — 期望重传的偏移量
 * @param  reason_code  — 拒绝原因码
 * @param  detail_code  — 详细错误码
 */
static void ota_data_send_nak(uint32_t session_id,
                              uint32_t retry_offset,
                              uint16_t reason_code,
                              uint16_t detail_code)
{
    uint8_t payload[OTA_DATA_NAK_PAYLOAD_LEN];

    ota_data_write_u16le(&payload[0], reason_code);
    ota_data_write_u16le(&payload[2], detail_code);
    (void)ota_data_send_frame(OTA_DATA_TYPE_NAK,
                              session_id,
                              retry_offset,
                              payload,
                              OTA_DATA_NAK_PAYLOAD_LEN);
}

/**
 * @brief  发送 ABORT 帧（终止传输）
 * @param  session_id  — 会话 ID
 * @param  final_offset — 最终偏移量
 * @param  stage        — 错误阶段
 * @param  error_class  — 错误类别（可重试/终端）
 * @param  error_code   — 错误码
 */
static void ota_data_send_abort(uint32_t session_id,
                                uint32_t final_offset,
                                uint8_t stage,
                                uint8_t error_class,
                                uint16_t error_code)
{
    uint8_t payload[OTA_DATA_ABORT_PAYLOAD_LEN];

    payload[0] = stage;
    payload[1] = error_class;
    ota_data_write_u16le(&payload[2], error_code);
    (void)ota_data_send_frame(OTA_DATA_TYPE_ABORT,
                              session_id,
                              final_offset,
                              payload,
                              OTA_DATA_ABORT_PAYLOAD_LEN);
}

/* =========================================================================
 *  25. 内部函数 —— OTA 数据帧接收
 * ======================================================================= */

/**
 * @brief  接收一帧 OTA 数据帧
 * @note   接收流程：
 *         1. 等待 SOF1 + SOF2（双字节帧起始标识）
 *         2. 读取 14 字节固定头部（含 2 字节头部 CRC）
 *         3. 验证协议版本与头部 CRC
 *         4. 读取载荷数据
 *         5. 读取 4 字节载荷 CRC 并校验
 * @param  frame   — 接收帧输出
 * @param  timeout — 超时时间（毫秒）
 * @return 接收状态码
 */
static int32_t ota_data_receive_frame(OtaDataFrame *frame, uint32_t timeout)
{
    uint8_t ch = 0U;
    uint8_t header[14];                             /* 固定头部缓存 */
    uint8_t trailer[4];                             /* 载荷 CRC 缓存 */
    uint16_t header_crc_recv = 0U;
    uint16_t header_crc_calc = 0U;
    uint32_t payload_crc_recv = 0U;
    uint32_t payload_crc_calc = 0U;
    uint16_t index = 0U;
    int32_t ret = OTA_DATA_PACKET_OK;

    if (frame == 0)
    {
        return OTA_DATA_PACKET_TIMEOUT;
    }

    memset(frame, 0, sizeof(*frame));

    /* 步骤 1：等待 SOF1 + SOF2 */
    while (1)
    {
        ret = Receive_Byte(&ch, timeout);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }

        if (ch != OTA_DATA_SOF1)
        {
            continue;
        }

        ret = Receive_Byte(&ch, 100U);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }

        if (ch == OTA_DATA_SOF2)
        {
            break;
        }
    }

    /* 步骤 2：读取 14 字节固定头部 */
    for (index = 0U; index < sizeof(header); ++index)
    {
        ret = Receive_Byte(&header[index], 100U);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }
    }

    /* 步骤 3：验证协议版本 */
    if (header[0] != OTA_DATA_PROTOCOL_VERSION)
    {
        return OTA_DATA_PACKET_CRC;
    }

    /* 验证头部 CRC */
    header_crc_recv = ota_data_read_u16le(&header[12]);
    header_crc_calc = Ymodem_Crc16(header, 12U);
    if (header_crc_recv != header_crc_calc)
    {
        return OTA_DATA_PACKET_CRC;
    }

    /* 解析帧头字段 */
    frame->type = header[1];
    frame->session_id = ota_data_read_u32le(&header[2]);
    frame->offset = ota_data_read_u32le(&header[6]);
    frame->payload_len = ota_data_read_u16le(&header[10]);

    /* 载荷长度合法性检查 */
    if (frame->payload_len > OTA_DATA_MAX_PAYLOAD_LEN)
    {
        return OTA_DATA_PACKET_CRC;
    }

    /* 步骤 4：读取载荷数据 */
    for (index = 0U; index < frame->payload_len; ++index)
    {
        ret = Receive_Byte(&frame->payload[index], 100U);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }
    }

    /* 步骤 5：读取并验证载荷 CRC（4 字节） */
    for (index = 0U; index < sizeof(trailer); ++index)
    {
        ret = Receive_Byte(&trailer[index], 100U);
        if (ret != OTA_DATA_PACKET_OK)
        {
            return ret;
        }
    }

    payload_crc_recv = ota_data_read_u32le(trailer);
    payload_crc_calc = ota_crc32_update(0U, frame->payload, frame->payload_len);
    if (payload_crc_recv != payload_crc_calc)
    {
        return OTA_DATA_PACKET_CRC;
    }

    return OTA_DATA_PACKET_OK;
}

/* =========================================================================
 *  26. 内部函数 —— AES-128 CTR 模式
 * ======================================================================= */

/**
 * @brief  构建 AES-128 CTR 计数器块
 * @note   计数器 = IV + block_index（大端序加法，从最低字节进位）
 * @param  counter    — 计数器输出（16 字节）
 * @param  iv         — 初始向量（16 字节）
 * @param  block_index — 块索引
 */
static void ota_ctr_build_counter(uint8_t counter[BLOCKSIZE], const uint8_t iv[BLOCKSIZE], uint32_t block_index)
{
    uint32_t carry = block_index;
    int32_t index = 0;

    /* 复制 IV 作为计数器基础 */
    memcpy(counter, iv, BLOCKSIZE);

    /* 从最低字节开始加法（大端序，最低字节在数组末尾） */
    for (index = BLOCKSIZE - 1; index >= 0; --index)
    {
        carry += (uint32_t)counter[index];
        counter[index] = (uint8_t)(carry & 0xFFU);
        carry >>= 8;
    }
}

/**
 * @brief  AES-128 CTR 模式加解密
 * @note   CTR 模式流程：
 *         1. 根据偏移量计算起始块索引
 *         2. 对每个块：构建计数器 → AES 加密 → 异或明文/密文
 *         3. 支持非对齐偏移（block_offset）
 * @param  buffer — 输入/输出缓冲区（原地修改）
 * @param  length — 数据长度
 * @param  iv     — 初始向量（16 字节）
 * @param  offset — 数据在固件中的偏移量
 */
static void ota_ctr_crypt(uint8_t *buffer,
                          uint32_t length,
                          const uint8_t iv[BLOCKSIZE],
                          uint32_t offset)
{
    uint8_t counter[BLOCKSIZE];
    uint8_t keystream[BLOCKSIZE];
    uint32_t processed = 0U;
    uint32_t block_index = offset / BLOCKSIZE;      /* 起始块索引 */
    uint32_t block_offset = offset % BLOCKSIZE;     /* 块内偏移 */
    uint32_t index = 0U;
    uint32_t chunk = 0U;

    if (buffer == 0 || length == 0U || iv == 0)
    {
        return;
    }

    while (processed < length)
    {
        /* 构建当前块的计数器 */
        ota_ctr_build_counter(counter, iv, block_index);

        /* AES 加密计数器生成密钥流 */
        memcpy(keystream, counter, sizeof(keystream));
        aesEncryptBlock(keystream);

        /* 计算本次可处理的字节数 */
        chunk = BLOCKSIZE - block_offset;
        if (chunk > (length - processed))
        {
            chunk = length - processed;
        }

        /* 异或密钥流与数据 */
        for (index = 0U; index < chunk; ++index)
        {
            buffer[processed + index] ^= keystream[block_offset + index];
        }

        processed += chunk;
        ++block_index;
        block_offset = 0U;  /* 后续块从头开始 */
    }
}

/* =========================================================================
 *  27. 内部函数 —— Flash 扇区工具
 * ======================================================================= */

/**
 * @brief  获取指定分区地址的末尾地址（不含）
 * @param  address — 分区起始地址
 * @return 分区末尾地址；0 — 地址不属于任何分区
 */
static uint32_t ota_data_slot_end_exclusive(uint32_t address)
{
    if (address == FLASH_APP1_ADDR)
    {
        return FLASH_APP1_ADDR + FLASH_APP_MAX_SIZE;
    }

    if (address == FLASH_APP2_ADDR)
    {
        return FLASH_APP2_ADDR + FLASH_APP_MAX_SIZE;
    }

    return 0U;
}

/**
 * @brief  获取地址所在扇区的起始地址
 * @param  address — Flash 地址
 * @return 扇区起始地址；0 — 地址超出范围
 */
static uint32_t ota_data_sector_base(uint32_t address)
{
    uint32_t index = 0U;

    for (index = 0U;
         index + 1U < (sizeof(s_ota_flash_sector_boundaries) / sizeof(s_ota_flash_sector_boundaries[0]));
         ++index)
    {
        if (address >= s_ota_flash_sector_boundaries[index] &&
            address < s_ota_flash_sector_boundaries[index + 1U])
        {
            return s_ota_flash_sector_boundaries[index];
        }
    }

    return 0U;
}

/**
 * @brief  获取地址所在扇区的下一个扇区起始地址
 * @param  address — Flash 地址
 * @return 下一个扇区起始地址；0 — 已是最后一个扇区
 */
static uint32_t ota_data_next_sector_base(uint32_t address)
{
    uint32_t index = 0U;

    for (index = 0U;
         index + 1U < (sizeof(s_ota_flash_sector_boundaries) / sizeof(s_ota_flash_sector_boundaries[0]));
         ++index)
    {
        if (address >= s_ota_flash_sector_boundaries[index] &&
            address < s_ota_flash_sector_boundaries[index + 1U])
        {
            return s_ota_flash_sector_boundaries[index + 1U];
        }
    }

    return 0U;
}

/* =========================================================================
 *  28. 内部函数 —— Flash 扇区擦除
 * ======================================================================= */

/**
 * @brief  擦除目标 Flash 区域
 * @note   逐扇区擦除，确保：
 *         1. 地址在合法分区内
 *         2. 起始地址对齐到扇区边界
 *         3. 不超出分区末尾
 * @param  address — 目标起始地址
 * @param  length  — 目标区域长度
 * @retval 1 — 擦除成功；0 — 参数错误或擦除失败
 */
static uint8_t ota_data_erase_target_area(uint32_t address, uint32_t length)
{
    uint32_t slot_end = 0U;
    uint32_t erase_end = 0U;
    uint32_t sector_addr = 0U;
    uint32_t next_sector = 0U;

    if (length == 0U)
    {
        return 0U;
    }

    /* 获取分区末尾地址 */
    slot_end = ota_data_slot_end_exclusive(address);
    if (slot_end == 0U)
    {
        return 0U;
    }

    /* 计算擦除结束地址并校验 */
    erase_end = address + length;
    if (erase_end < address || erase_end > slot_end)
    {
        return 0U;
    }

    /* 起始地址必须对齐到扇区边界 */
    sector_addr = ota_data_sector_base(address);
    if (sector_addr != address)
    {
        return 0U;
    }

    /* 解锁 Flash 并逐扇区擦除 */
    FLASH_Unlock();

    while (sector_addr < erase_end)
    {
        if (MY_FLASH_Erase(sector_addr) != 0U)
        {
            FLASH_Lock();
            return 0U;
        }

        next_sector = ota_data_next_sector_base(sector_addr);
        if (next_sector <= sector_addr)
        {
            FLASH_Lock();
            return 0U;
        }

        sector_addr = next_sector;
    }

    FLASH_Lock();
    return 1U;
}

/* =========================================================================
 *  29. 内部函数 —— 错误分类
 * ======================================================================= */

/**
 * @brief  判断错误码是否为终端错误（不可重试）
 * @param  err_code — 错误码
 * @retval 1 — 终端错误；0 — 可重试错误
 */
static uint8_t ota_data_error_is_terminal(uint8_t err_code)
{
    return (err_code == YMODEM_ERR_HEADER ||
            err_code == YMODEM_ERR_AUTH ||
            err_code == YMODEM_ERR_VERSION ||
            err_code == YMODEM_ERR_SLOT) ? 1U : 0U;
}

/* =========================================================================
 *  30. 内部函数 —— START 帧处理
 * ======================================================================= */

/**
 * @brief  处理 OTA 数据 START 帧（传输启动）
 * @note   START 帧载荷布局（OTA_DATA_START_PAYLOAD_LEN = 48 字节）：
 *         [0..1]   标志位（LE16，bit0=续传请求）
 *         [2..3]   块大小（LE16）
 *         [4..7]   检查点间隔（LE32）
 *         [8..11]  传输总大小（LE32）
 *         [12..15] 明文大小（LE32）
 *         [16..47] 会话指纹（32 字节）
 *         验证内容：会话 ID、指纹、参数一致性、续传状态。
 *         验证通过后：擦除 Flash 或恢复体哈希、发送 ACK。
 * @param  frame — START 帧
 * @param  state — 传输状态
 * @retval 1 — 处理成功；0 — 终端错误
 */
static uint8_t ota_data_process_start_frame(const OtaDataFrame *frame, YmodemTransferState *state)
{
    uint16_t flags = 0U;
    uint16_t chunk_size = 0U;
    uint32_t checkpoint_size = 0U;
    uint32_t transfer_size = 0U;
    uint32_t plain_size = 0U;
    uint8_t resume_requested = 0U;

    if (frame == 0 || state == 0)
    {
        Ymodem_SetError(YMODEM_ERR_PROTOCOL, OTA_CTRL_STAGE_READY);
        return 0U;
    }

    /* 验证会话 ID */
    if (frame->session_id != ymodem_session_id)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_SESSION,
                          0U);
        return 1U;
    }

    /* 验证载荷长度 */
    if (frame->payload_len != OTA_DATA_START_PAYLOAD_LEN)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_LENGTH,
                          frame->payload_len);
        return 1U;
    }

    /* 解析 START 载荷字段 */
    flags = ota_data_read_u16le(&frame->payload[0]);
    chunk_size = ota_data_read_u16le(&frame->payload[2]);
    checkpoint_size = ota_data_read_u32le(&frame->payload[4]);
    transfer_size = ota_data_read_u32le(&frame->payload[8]);
    plain_size = ota_data_read_u32le(&frame->payload[12]);
    resume_requested = ((flags & OTA_DATA_START_FLAG_RESUME) != 0U) ? 1U : 0U;

    /* 验证会话指纹 */
    if (memcmp(&frame->payload[16], ymodem_session_fingerprint, OTA_CTRL_FINGERPRINT_LEN) != 0)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_SESSION,
                          1U);
        return 1U;
    }

    /* 验证参数一致性 */
    if (chunk_size == 0U ||
        chunk_size > OTA_DATA_MAX_PAYLOAD_LEN ||
        checkpoint_size == 0U ||
        transfer_size != ymodem_received_header.payload.firmware_size ||
        plain_size != ymodem_received_header.payload.firmware_size ||
        frame->offset != state->start_offset)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_PROTOCOL,
                          0U);
        return 1U;
    }

    /* 验证续传状态一致性 */
    if (((state->start_offset != 0U) && (resume_requested == 0U)) ||
        ((state->start_offset == 0U) && (resume_requested != 0U)))
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_STATE,
                          0U);
        return 1U;
    }

    /* 初始化传输状态 */
    state->transfer_size = transfer_size;
    state->checkpoint_size = checkpoint_size;
    state->chunk_size = chunk_size;
    state->firmware_received = state->start_offset;
    state->durable_offset = state->start_offset;
    state->last_acked_offset = state->start_offset;
    state->flash_destination = APPLICATION_ADDRESS + state->start_offset;

    /* 开始体哈希计算 */
    Ymodem_BodyHashBegin();

    if (state->start_offset == 0U)
    {
        /* 全新传输：擦除目标 Flash 区域 */
        if (ota_data_erase_target_area(APPLICATION_ADDRESS, state->transfer_size) == 0U)
        {
            Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_TRANSFER);
            return 0U;
        }
    }
    else if (Ymodem_BodyHashResumeFromFlash(APPLICATION_ADDRESS, state->start_offset) == 0U)
    {
        /* 续传：从 Flash 恢复体哈希 */
        Ymodem_SetError(YMODEM_ERR_AUTH, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    /* 标记 Flash 就绪并发送 ACK */
    state->flash_ready = 1U;
    ymodem_last_acked_offset = state->start_offset;
    ota_data_send_ack(ymodem_session_id, state->start_offset);
    Ymodem_NotifyProgress(state->start_offset, state->transfer_size);
    return 1U;
}

/* =========================================================================
 *  31. 内部函数 —— CHUNK 帧处理
 * ======================================================================= */

/**
 * @brief  处理 OTA 数据 CHUNK 帧（固件数据块）
 * @note   处理流程：
 *         1. 验证会话 ID、Flash 就绪状态、载荷长度
 *         2. 处理重复/乱序帧（发送 ACK 或 NAK）
 *         3. AES-128 CTR 解密
 *         4. 更新体哈希
 *         5. 写入 Flash（带缓冲）
 *         6. 检查点持久化
 *         7. 发送 ACK
 * @param  frame — CHUNK 帧
 * @param  state — 传输状态
 * @retval 1 — 处理成功；0 — 终端错误
 */
static uint8_t ota_data_process_chunk_frame(const OtaDataFrame *frame, YmodemTransferState *state)
{
    uint8_t plain[OTA_DATA_MAX_PAYLOAD_LEN];        /* 解密后的明文缓冲区 */

    if (frame == 0 || state == 0)
    {
        Ymodem_SetError(YMODEM_ERR_PROTOCOL, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    /* 验证会话 ID */
    if (frame->session_id != ymodem_session_id)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_SESSION,
                          0U);
        return 1U;
    }

    /* 验证 Flash 就绪状态 */
    if (state->flash_ready == 0U)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_STATE,
                          0U);
        return 1U;
    }

    /* 验证载荷长度与偏移量 */
    if (frame->payload_len == 0U ||
        frame->payload_len > state->chunk_size ||
        (frame->offset + frame->payload_len) > state->transfer_size)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_LENGTH,
                          frame->payload_len);
        return 1U;
    }

    /* 处理重复帧（已接收的数据） */
    if (frame->offset < state->firmware_received)
    {
        ota_data_send_ack(ymodem_session_id, state->firmware_received);
        return 1U;
    }

    /* 处理乱序帧（偏移量不连续） */
    if (frame->offset != state->firmware_received)
    {
        ota_data_send_nak(ymodem_session_id,
                          state->firmware_received,
                          OTA_DATA_NAK_REASON_OFFSET,
                          0U);
        return 1U;
    }

    /* AES-128 CTR 解密 */
    memcpy(plain, frame->payload, frame->payload_len);
    ota_ctr_crypt(plain,
                  frame->payload_len,
                  ymodem_received_header.payload.iv,
                  frame->offset);

    /* 更新体哈希并写入 Flash */
    Ymodem_BodyHashUpdate(plain, frame->payload_len);
    if (Ymodem_WriteFlashBuffered(state, plain, frame->payload_len) == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    /* 更新已接收计数 */
    state->firmware_received += frame->payload_len;
    state->last_acked_offset = state->firmware_received;
    ymodem_last_acked_offset = state->last_acked_offset;

    /* 检查点持久化 */
    if (ota_data_maybe_persist_progress(state) == 0U)
    {
        return 0U;
    }

    /* 发送 ACK 并通知进度 */
    ota_data_send_ack(ymodem_session_id, state->firmware_received);
    Ymodem_NotifyProgress(state->firmware_received, state->transfer_size);
    return 1U;
}

/* =========================================================================
 *  32. 内部函数 —— 检查点持久化
 * ======================================================================= */

/**
 * @brief  根据策略决定是否持久化传输进度
 * @note   持久化策略：
 *         - 首次持久化：接收超过 start_offset + first_persist_bytes 后
 *         - 后续持久化：每接收 persist_stride_bytes 后
 *         持久化操作：
 *         1. 刷新 Flash 缓冲区
 *         2. 调用检查点回调（保存进度到非易失存储）
 * @param  state — 传输状态
 * @retval 1 — 成功；0 — 写入失败
 */
static uint8_t ota_data_maybe_persist_progress(YmodemTransferState *state)
{
    uint32_t persist_offset = 0U;

    if (state == 0)
    {
        Ymodem_SetError(YMODEM_ERR_PROTOCOL, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    /* 已持久化的偏移量无需重复 */
    if (state->firmware_received <= state->durable_offset)
    {
        return 1U;
    }

    /* 判断是否满足持久化条件 */
    if (state->durable_offset == state->start_offset)
    {
        /* 首次持久化：接收超过 start + first_persist_bytes */
        uint32_t first_persist_target = state->start_offset + s_ymodem_resume_first_persist_bytes;

        if (state->firmware_received < first_persist_target)
        {
            return 1U;
        }

        persist_offset = first_persist_target;
    }
    else if ((state->firmware_received - state->durable_offset) >= s_ymodem_resume_persist_stride_bytes)
    {
        /* 后续持久化：接收超过 stride */
        persist_offset = state->firmware_received;
    }
    else
    {
        return 1U;
    }

    /* 确保持久化偏移不超过已接收量 */
    if (persist_offset > state->firmware_received)
    {
        persist_offset = state->firmware_received;
    }

    if (persist_offset <= state->durable_offset)
    {
        return 1U;
    }

    /* 刷新 Flash 缓冲区 */
    if (Ymodem_FlushFlashBuffered(state) == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    /* 调用检查点回调 */
    if (ymodem_checkpoint_callback != 0 &&
        ymodem_checkpoint_callback(persist_offset, ymodem_checkpoint_context) == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_DONE);
        return 0U;
    }

    state->durable_offset = persist_offset;
    return 1U;
}

/* =========================================================================
 *  33. 内部函数 —— 传输完成
 * ======================================================================= */

/**
 * @brief  完成固件传输（处理 FINISH 帧）
 * @note   完成流程：
 *         1. 验证已接收大小 = 传输总大小
 *         2. 刷新 Flash 缓冲区
 *         3. 最终检查点持久化
 *         4. 完成体哈希计算
 *         5. 通知进度完成
 * @param  state — 传输状态
 * @retval 1 — 成功；0 — 大小不匹配或写入失败
 */
static uint8_t ota_data_finalize_transfer(YmodemTransferState *state)
{
    if (state == 0)
    {
        Ymodem_SetError(YMODEM_ERR_PROTOCOL, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    /* 验证已接收大小 */
    if (state->firmware_received != state->transfer_size)
    {
        Ymodem_SetError(YMODEM_ERR_SIZE, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    /* 刷新 Flash 缓冲区 */
    if (Ymodem_FlushFlashBuffered(state) == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_TRANSFER);
        return 0U;
    }

    /* 最终检查点持久化 */
    if (state->durable_offset != state->transfer_size)
    {
        if (ymodem_checkpoint_callback != 0 &&
            ymodem_checkpoint_callback(state->transfer_size, ymodem_checkpoint_context) == 0U)
        {
            Ymodem_SetError(YMODEM_ERR_FLASH, OTA_CTRL_STAGE_DONE);
            return 0U;
        }

        state->durable_offset = state->transfer_size;
    }

    /* 完成体哈希计算 */
    Ymodem_BodyHashFinish();
    ymodem_received_firmware_size = state->transfer_size;
    ymodem_last_acked_offset = state->transfer_size;
    Ymodem_NotifyProgress(state->transfer_size, state->transfer_size);
    return 1U;
}

/* =========================================================================
 *  34. 公共接口 —— 主接收循环（Ymodem_Receive）
 * ======================================================================= */

/**
 * @brief  OTA 数据接收主循环
 * @note   主循环流程：
 *         1. 初始化传输状态与 AES 引擎
 *         2. 循环接收数据帧：
 *            - START → 初始化传输并 ACK
 *            - CHUNK → 解密、校验、写入 Flash 并 ACK
 *            - FINISH → 完成传输并返回
 *            - ABORT → 返回错误
 *            - 超时/UART 错误 → 发送 ABORT 并返回
 *         3. CRC 错误时发送 NAK 请求重传
 * @param  buf — Flash 写入缓冲区（OTA_DATA_MAX_PAYLOAD_LEN 字节）
 * @return 成功时返回传输大小（字节）；失败返回 -1
 */
int32_t Ymodem_Receive(uint8_t *buf)
{
    OtaDataFrame frame;
    YmodemTransferState state;
    uint8_t session_started = 0U;
    int32_t ret = OTA_DATA_PACKET_OK;

    /* 前置条件检查 */
    if (buf == 0 || ymodem_session_configured == 0U || ymodem_received_header_valid == 0U)
    {
        Ymodem_SetError(YMODEM_ERR_HEADER, OTA_CTRL_STAGE_READY);
        return -1;
    }

    /* 初始化传输状态 */
    memset(&state, 0, sizeof(state));
    state.flash_buffer = buf;
    state.start_offset = ymodem_session_start_offset;
    state.checkpoint_size = ymodem_session_checkpoint_size;
    state.transfer_size = ymodem_received_header.payload.firmware_size;
    state.durable_offset = ymodem_session_start_offset;
    state.firmware_received = ymodem_session_start_offset;
    state.last_acked_offset = ymodem_session_start_offset;

    /* 重置错误状态并初始化 AES 引擎 */
    Ymodem_ResetError();
    aesEncInit();

    /* ====== 主接收循环 ====== */
    while (1)
    {
        /* 尝试接收一帧（5 秒超时） */
        ret = ota_data_receive_frame(&frame, 5000U);

        /* 处理接收超时 */
        if (ret == OTA_DATA_PACKET_TIMEOUT)
        {
            Ymodem_SetError(YMODEM_ERR_TIMEOUT, OTA_CTRL_STAGE_TRANSFER);
            ota_data_send_abort(ymodem_session_id,
                                state.last_acked_offset,
                                OTA_CTRL_STAGE_TRANSFER,
                                OTA_DATA_ABORT_CLASS_RETRYABLE,
                                YMODEM_ERR_TIMEOUT);
            return -1;
        }

        /* 处理 UART 通信错误 */
        if (ret == OTA_DATA_PACKET_UART)
        {
            Ymodem_SetError(YMODEM_ERR_UART, OTA_CTRL_STAGE_TRANSFER);
            ota_data_send_abort(ymodem_session_id,
                                state.last_acked_offset,
                                OTA_CTRL_STAGE_TRANSFER,
                                OTA_DATA_ABORT_CLASS_RETRYABLE,
                                YMODEM_ERR_UART);
            return -1;
        }

        /* 处理 CRC 错误（发送 NAK 请求重传） */
        if (ret == OTA_DATA_PACKET_CRC)
        {
            ota_data_send_nak(ymodem_session_id,
                              state.firmware_received,
                              OTA_DATA_NAK_REASON_PAYLOAD_CRC,
                              0U);
            continue;
        }

        /* 根据帧类型分发处理 */
        switch (frame.type)
        {
        case OTA_DATA_TYPE_START:
            /* START 帧：初始化传输 */
            if (session_started != 0U)
            {
                /* 重复 START：仅 ACK */
                ota_data_send_ack(ymodem_session_id, state.firmware_received);
                continue;
            }

            if (ota_data_process_start_frame(&frame, &state) == 0U)
            {
                /* 终端错误：发送 ABORT */
                ota_data_send_abort(ymodem_session_id,
                                    state.last_acked_offset,
                                    Ymodem_GetErrorStage(),
                                    OTA_DATA_ABORT_CLASS_TERMINAL,
                                    Ymodem_GetErrorCode());
                return -1;
            }
            session_started = 1U;
            break;

        case OTA_DATA_TYPE_CHUNK:
            /* CHUNK 帧：固件数据块 */
            if (session_started == 0U)
            {
                /* 未收到 START：发送 NAK */
                ota_data_send_nak(ymodem_session_id,
                                  state.firmware_received,
                                  OTA_DATA_NAK_REASON_STATE,
                                  0U);
                continue;
            }

            if (ota_data_process_chunk_frame(&frame, &state) == 0U)
            {
                /* 错误：根据错误类别决定 ABORT 类型 */
                ota_data_send_abort(ymodem_session_id,
                                    state.last_acked_offset,
                                    Ymodem_GetErrorStage(),
                                    (ota_data_error_is_terminal(Ymodem_GetErrorCode()) != 0U) ?
                                        OTA_DATA_ABORT_CLASS_TERMINAL :
                                        OTA_DATA_ABORT_CLASS_RETRYABLE,
                                    Ymodem_GetErrorCode());
                return -1;
            }
            break;

        case OTA_DATA_TYPE_FINISH:
            /* FINISH 帧：传输完成 */
            if (session_started == 0U)
            {
                ota_data_send_nak(ymodem_session_id,
                                  state.firmware_received,
                                  OTA_DATA_NAK_REASON_STATE,
                                  0U);
                continue;
            }

            /* 验证 FINISH 帧参数 */
            if (frame.session_id != ymodem_session_id ||
                frame.payload_len != 0U ||
                frame.offset != state.transfer_size)
            {
                ota_data_send_nak(ymodem_session_id,
                                  state.firmware_received,
                                  OTA_DATA_NAK_REASON_PROTOCOL,
                                  0U);
                continue;
            }

            /* 完成传输 */
            if (ota_data_finalize_transfer(&state) == 0U)
            {
                ota_data_send_abort(ymodem_session_id,
                                    state.last_acked_offset,
                                    Ymodem_GetErrorStage(),
                                    OTA_DATA_ABORT_CLASS_RETRYABLE,
                                    Ymodem_GetErrorCode());
                return -1;
            }

            /* 发送最终 ACK 并返回传输大小 */
            ota_data_send_ack(ymodem_session_id, state.transfer_size);
            return (int32_t)state.transfer_size;

        case OTA_DATA_TYPE_ABORT:
            /* ABORT 帧：对方终止传输 */
            Ymodem_SetError(YMODEM_ERR_ABORT, OTA_CTRL_STAGE_TRANSFER);
            return -1;

        default:
            /* 未知帧类型：发送 NAK */
            ota_data_send_nak(ymodem_session_id,
                              state.firmware_received,
                              OTA_DATA_NAK_REASON_PROTOCOL,
                              frame.type);
            break;
        }
    }
}
