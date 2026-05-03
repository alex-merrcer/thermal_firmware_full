/**
 * @file    iap_ctrl.c
 * @brief   OTA 控制协议实现 —— STM32 与 ESP32 之间的升级协商通道
 * @note    本模块实现 OTA 控制平面的完整协议栈，包括：
 *          - CRC-16/CCITT 校验（多项式 0x1021）
 *          - 小端序序列化/反序列化工具函数
 *          - 控制帧的组装、发送与接收（双字节 SOF + 协议头 + 载荷 + CRC）
 *          - 请求（REQ）、就绪（READY）、确认（ACK）、状态（STATUS）、
 *            错误（ERROR）、结果（RESULT）、元数据（META）等消息处理
 *          - 升级就绪等待状态机（发送 REQ → 等待 ACK → 等待 READY）
 *
 * @version 2.0
 * @date    2026-05-01
 */

#include "iap_ctrl.h"

/* =========================================================================
 *  1. 模块内部变量
 * ======================================================================= */

/** 控制帧序列号（自动递增，0 跳过） */
static uint8_t s_ota_ctrl_seq = 1U;

/* =========================================================================
 *  2. 内部函数 —— CRC-16/CCITT 校验
 * ======================================================================= */

/**
 * @brief  计算 CRC-16/CCITT 校验值
 * @note   多项式 0x1021，初始值 0x0000，MSB 优先。
 *         逐字节处理，每字节左移 8 位后与 CRC 异或，
 *         再逐位检查最高位并移位异或。
 * @param  data   — 数据指针
 * @param  length — 数据长度（字节）
 * @return CRC-16 校验值
 */
static uint16_t ota_ctrl_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0U;
    uint16_t i = 0U;

    /* 逐字节处理 */
    while (length-- > 0U)
    {
        /* 当前字节左移 8 位后与 CRC 异或 */
        crc ^= (uint16_t)(*data++) << 8;

        /* 逐位处理 8 个比特 */
        for (i = 0U; i < 8U; ++i)
        {
            if ((crc & 0x8000U) != 0U)
            {
                /* 最高位为 1：左移后异或多项式 0x1021 */
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            }
            else
            {
                /* 最高位为 0：仅左移 */
                crc <<= 1;
            }
        }
    }

    return crc;
}

/* =========================================================================
 *  3. 内部函数 —— 小端序序列化/反序列化
 * ======================================================================= */

/**
 * @brief  将 16 位无符号整数写入缓冲区（小端序）
 * @param  buffer — 目标缓冲区（至少 2 字节）
 * @param  value  — 待写入的值
 */
static void ota_ctrl_write_u16le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);           /* 低字节 */
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);    /* 高字节 */
}

/**
 * @brief  将 32 位无符号整数写入缓冲区（小端序）
 * @param  buffer — 目标缓冲区（至少 4 字节）
 * @param  value  — 待写入的值
 */
static void ota_ctrl_write_u32le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);            /* 最低字节 */
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);    /* 最高字节 */
}

/**
 * @brief  从缓冲区读取 16 位无符号整数（小端序）
 * @param  buffer — 源缓冲区（至少 2 字节）
 * @return 解析后的 16 位值
 */
static uint16_t ota_ctrl_read_u16le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

/**
 * @brief  从缓冲区读取 32 位无符号整数（小端序）
 * @param  buffer — 源缓冲区（至少 4 字节）
 * @return 解析后的 32 位值
 */
static uint32_t ota_ctrl_read_u32le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

/* =========================================================================
 *  4. 内部函数 —— 字符串填充与复制
 * ======================================================================= */

/**
 * @brief  将字符串填充到固定长度的缓冲区（零填充 + 拷贝）
 * @note   先将目标缓冲区全部清零，再逐字节复制源字符串。
 *         超出源字符串长度的部分保持为 0。
 * @param  target     — 目标缓冲区
 * @param  target_len — 目标缓冲区长度
 * @param  value      — 源字符串（可为 NULL）
 */
static void ota_ctrl_fill_string(uint8_t *target, uint16_t target_len, const char *value)
{
    uint16_t i = 0U;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    /* 先清零整个目标缓冲区 */
    for (i = 0U; i < target_len; ++i)
    {
        target[i] = 0U;
    }

    /* 源字符串为空则直接返回 */
    if (value == 0)
    {
        return;
    }

    /* 逐字节复制，遇到字符串结尾或缓冲区满时停止 */
    for (i = 0U; i < target_len && value[i] != '\0'; ++i)
    {
        target[i] = (uint8_t)value[i];
    }
}

/**
 * @brief  从二进制缓冲区复制 ASCII 字符串（安全截断）
 * @note   将二进制数据逐字节转换为 ASCII 字符，确保目标以 '\0' 结尾。
 * @param  target     — 目标字符串缓冲区
 * @param  target_len — 目标缓冲区长度
 * @param  source     — 源二进制缓冲区
 * @param  source_len — 源缓冲区长度
 */
static void ota_ctrl_copy_ascii(char *target,
                                uint16_t target_len,
                                const uint8_t *source,
                                uint16_t source_len)
{
    uint16_t i = 0U;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    /* 先清零目标缓冲区 */
    for (i = 0U; i < target_len; ++i)
    {
        target[i] = '\0';
    }

    if (source == 0)
    {
        return;
    }

    /* 逐字节复制，保留空间给终止符 '\0' */
    for (i = 0U; i + 1U < target_len && i < source_len && source[i] != '\0'; ++i)
    {
        target[i] = (char)source[i];
    }
}

/* =========================================================================
 *  5. 公共接口 —— UART 刷新
 * ======================================================================= */

/**
 * @brief  刷新 UART 接收状态机
 * @note   复位串口接收缓冲区和状态标志，清除残留数据。
 */
void ota_ctrl_flush_uart(void)
{
    SerialResetRxState();
}

/* =========================================================================
 *  6. 内部函数 —— 带超时的字节接收
 * ======================================================================= */

/**
 * @brief  从串口接收单个字节（带超时）
 * @note   以 OTA_CTRL_POLL_STEP_US 步进轮询串口，累计等待至超时。
 *         超时时间 = timeout_ms × 1000 微秒。
 * @param  byte      — 接收字节输出指针
 * @param  timeout_ms — 超时时间（毫秒）
 * @retval 1 — 接收成功；0 — 超时
 */
static uint8_t ota_ctrl_read_byte_timeout(uint8_t *byte, uint32_t timeout_ms)
{
    uint32_t waited_us = 0U;
    uint32_t timeout_us = timeout_ms * 1000U;

    /* 轮询等待，每次步进 OTA_CTRL_POLL_STEP_US 微秒 */
    while (waited_us < timeout_us)
    {
        if (SerialKeyPressed(byte) != 0U)
        {
            return 1U;  /* 接收成功 */
        }

        delay_us(OTA_CTRL_POLL_STEP_US);
        waited_us += OTA_CTRL_POLL_STEP_US;
    }

    return 0U;  /* 超时 */
}

/* =========================================================================
 *  7. 内部函数 —— 控制帧发送
 * ======================================================================= */

/**
 * @brief  组装并发送 OTA 控制帧
 * @note   帧格式：
 *         | SOF1 | SOF2 | PROTO_VER | MSG_TYPE | SEQ | LEN(LE16) | PAYLOAD | CRC16 |
 *         CRC 覆盖范围：从 PROTO_VER 到 PAYLOAD 末尾（不含 SOF）。
 * @param  msg_type   — 消息类型
 * @param  seq        — 序列号
 * @param  payload    — 载荷数据指针
 * @param  payload_len — 载荷长度
 * @retval 1 — 发送成功；0 — 载荷超长
 */
static uint8_t ota_ctrl_send_frame(uint8_t msg_type,
                                   uint8_t seq,
                                   const uint8_t *payload,
                                   uint16_t payload_len)
{
    uint8_t frame[OTA_CTRL_MAX_FRAME_LEN];
    uint16_t crc = 0U;
    uint16_t total_len = 0U;
    uint16_t i = 0U;

    /* 载荷长度检查 */
    if (payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
    {
        return 0U;
    }

    /* 组装帧头（SOF + 协议版本 + 消息类型 + 序列号 + 载荷长度） */
    frame[0] = OTA_CTRL_SOF1;
    frame[1] = OTA_CTRL_SOF2;
    frame[2] = OTA_CTRL_PROTOCOL_VERSION;
    frame[3] = msg_type;
    frame[4] = seq;
    ota_ctrl_write_u16le(&frame[5], payload_len);

    /* 复制载荷数据 */
    for (i = 0U; i < payload_len; ++i)
    {
        frame[OTA_CTRL_HEADER_LEN + i] = payload[i];
    }

    /* 计算 CRC（覆盖 PROTO_VER 到 PAYLOAD 末尾） */
    crc = ota_ctrl_crc16(&frame[2], (uint16_t)(5U + payload_len));
    ota_ctrl_write_u16le(&frame[OTA_CTRL_HEADER_LEN + payload_len], crc);

    /* 逐字节发送整个帧 */
    total_len = (uint16_t)(OTA_CTRL_FRAME_OVERHEAD + payload_len);
    for (i = 0U; i < total_len; ++i)
    {
        SerialPutChar(frame[i]);
    }

    return 1U;
}

/* =========================================================================
 *  8. 内部函数 —— 控制帧接收
 * ======================================================================= */

/**
 * @brief  接收一帧 OTA 控制帧（带超时）
 * @note   接收流程：
 *         1. 等待 SOF1（0xAA），超时则返回失败
 *         2. 等待 SOF2（0x55），超时则返回失败
 *         3. 读取 5 字节协议头（版本 + 类型 + 序列号 + 长度）
 *         4. 验证协议版本
 *         5. 读取载荷数据
 *         6. 读取 2 字节 CRC 并校验
 *         CRC 校验失败时静默丢弃帧，继续等待下一帧。
 * @param  frame      — 接收帧结构体输出
 * @param  timeout_ms — 总超时时间（毫秒）
 * @retval 1 — 接收成功；0 — 超时或协议错误
 */
static uint8_t ota_ctrl_receive_frame(ota_ctrl_frame_t *frame, uint32_t timeout_ms)
{
    uint8_t ch = 0U;
    uint8_t header[5];                              /* 协议头缓存 */
    uint8_t crc_bytes[2];                           /* 接收的 CRC 字节 */
    uint8_t crc_buffer[5U + OTA_CTRL_MAX_PAYLOAD_LEN]; /* CRC 计算缓冲区 */
    uint16_t crc_calc = 0U;
    uint16_t crc_recv = 0U;
    uint16_t i = 0U;
    uint32_t waited = 0U;

    /* 主循环：等待 SOF1 + SOF2 */
    while (waited < timeout_ms)
    {
        /* 尝试接收 1 字节（1ms 超时） */
        if (ota_ctrl_read_byte_timeout(&ch, 1U) == 0U)
        {
            ++waited;
            continue;
        }

        /* 等待帧起始标识 SOF1 */
        if (ch != OTA_CTRL_SOF1)
        {
            continue;
        }

        /* 等待 SOF2（20ms 超时） */
        if (ota_ctrl_read_byte_timeout(&ch, 20U) == 0U)
        {
            return 0U;
        }

        if (ch != OTA_CTRL_SOF2)
        {
            continue;   /* SOF2 不匹配，丢弃并重新等待 */
        }

        /* 读取 5 字节协议头 */
        for (i = 0U; i < sizeof(header); ++i)
        {
            if (ota_ctrl_read_byte_timeout(&header[i], 20U) == 0U)
            {
                return 0U;
            }
        }

        /* 验证协议版本号 */
        if (header[0] != OTA_CTRL_PROTOCOL_VERSION)
        {
            continue;   /* 版本不匹配，静默丢弃 */
        }

        /* 解析帧头字段 */
        frame->msg_type = header[1];
        frame->seq = header[2];
        frame->payload_len = ota_ctrl_read_u16le(&header[3]);

        /* 载荷长度合法性检查 */
        if (frame->payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
        {
            ota_ctrl_flush_uart();
            return 0U;
        }

        /* 读取载荷数据 */
        for (i = 0U; i < frame->payload_len; ++i)
        {
            if (ota_ctrl_read_byte_timeout(&frame->payload[i], 20U) == 0U)
            {
                return 0U;
            }
        }

        /* 读取 2 字节 CRC */
        if (ota_ctrl_read_byte_timeout(&crc_bytes[0], 20U) == 0U ||
            ota_ctrl_read_byte_timeout(&crc_bytes[1], 20U) == 0U)
        {
            return 0U;
        }

        /* CRC 校验：重组头 + 载荷进行计算 */
        crc_recv = ota_ctrl_read_u16le(crc_bytes);
        for (i = 0U; i < sizeof(header); ++i)
        {
            crc_buffer[i] = header[i];
        }
        for (i = 0U; i < frame->payload_len; ++i)
        {
            crc_buffer[sizeof(header) + i] = frame->payload[i];
        }

        crc_calc = ota_ctrl_crc16(crc_buffer, (uint16_t)(sizeof(header) + frame->payload_len));
        if (crc_recv != crc_calc)
        {
            continue;   /* CRC 不匹配，丢弃并继续等待 */
        }

        return 1U;      /* 帧接收成功 */
    }

    return 0U;          /* 总超时 */
}

/* =========================================================================
 *  9. 内部函数 —— 序列号管理
 * ======================================================================= */

/**
 * @brief  获取下一个控制帧序列号
 * @note   序列号自动递增，跳过 0（保留值）。
 * @return 序列号
 */
static uint8_t ota_ctrl_next_seq(void)
{
    if (s_ota_ctrl_seq == 0U)
    {
        s_ota_ctrl_seq = 1U;
    }

    return s_ota_ctrl_seq++;
}

/* =========================================================================
 *  10. 内部函数 —— 请求载荷组装
 * ======================================================================= */

/**
 * @brief  组装 OTA 升级请求（REQ）载荷
 * @note   载荷布局（OTA_CTRL_REQ_PAYLOAD_LEN = 60 字节）：
 *         [0]     请求类型（1=升级）
 *         [1]     当前活跃分区
 *         [2]     目标分区
 *         [3]     协议版本（固定 1）
 *         [4..19] 当前固件版本（16 字节，零填充）
 *         [20..35] 产品标识（16 字节，零填充）
 *         [36..43] 硬件版本（8 字节，零填充）
 *         [44..55] 设备唯一 ID（12 字节，STM32 UID）
 *         [56..59] 请求标志位（LE32）
 * @param  boot_info  — BootInfo 指针
 * @param  payload    — 载荷输出缓冲区
 * @param  payload_len — 载荷长度输出
 * @param  req_flags  — 请求标志位
 * @retval 1 — 成功；0 — 参数错误
 */
static uint8_t ota_ctrl_prepare_request_payload(const BootInfoTypeDef *boot_info,
                                                uint8_t *payload,
                                                uint16_t *payload_len,
                                                uint32_t req_flags)
{
    const uint8_t *uid = (const uint8_t *)STM32_UID_BASE_ADDR;
    uint16_t i = 0U;

    if (boot_info == 0 || payload == 0 || payload_len == 0)
    {
        return 0U;
    }

    /* 清零载荷缓冲区 */
    for (i = 0U; i < OTA_CTRL_REQ_PAYLOAD_LEN; ++i)
    {
        payload[i] = 0U;
    }

    /* 填充各字段 */
    payload[0] = OTA_CTRL_REQ_TYPE_UPGRADE;             /* 请求类型 */
    payload[1] = (uint8_t)boot_info->active_partition;  /* 当前活跃分区 */
    payload[2] = (uint8_t)boot_info->target_partition;  /* 目标分区 */
    payload[3] = 1U;                                    /* 协议版本 */
    ota_ctrl_fill_string(&payload[4], OTA_CTRL_VERSION_LEN, boot_info->current_version);
    ota_ctrl_fill_string(&payload[20], OTA_CTRL_PRODUCT_ID_LEN, IAP_DEVICE_PRODUCT_ID);
    ota_ctrl_fill_string(&payload[36], OTA_CTRL_HW_REV_LEN, IAP_DEVICE_HW_REV);

    /* 复制设备唯一 ID（12 字节） */
    for (i = 0U; i < OTA_CTRL_UID_LEN; ++i)
    {
        payload[44 + i] = uid[i];
    }

    /* 写入请求标志位 */
    ota_ctrl_write_u32le(&payload[56], req_flags);
    *payload_len = OTA_CTRL_REQ_PAYLOAD_LEN;
    return 1U;
}

/* =========================================================================
 *  11. 公共接口 —— GO / STATUS / RESULT 消息发送
 * ======================================================================= */

/**
 * @brief  发送 GO 消息（通知 ESP32 开始数据传输）
 * @note   载荷布局（8 字节）：
 *         [0]     目标分区
 *         [2..3]  GO 标志位（LE16）
 *         [4..7]  续传偏移量（LE32）
 * @param  target_partition — 目标分区
 * @param  go_flags         — GO 标志位
 * @param  resume_offset    — 续传偏移量
 * @retval 1 — 发送成功；0 — 发送失败
 */
uint8_t ota_ctrl_send_go(uint8_t target_partition, uint16_t go_flags, uint32_t resume_offset)
{
    uint8_t payload[OTA_CTRL_GO_PAYLOAD_LEN];
    uint16_t i = 0U;

    /* 清零载荷 */
    for (i = 0U; i < OTA_CTRL_GO_PAYLOAD_LEN; ++i)
    {
        payload[i] = 0U;
    }

    /* 填充字段 */
    payload[0] = target_partition;
    ota_ctrl_write_u16le(&payload[2], go_flags);
    ota_ctrl_write_u32le(&payload[4], resume_offset);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_GO, ota_ctrl_next_seq(), payload, OTA_CTRL_GO_PAYLOAD_LEN);
}

/**
 * @brief  发送 STATUS 消息（向 ESP32 报告升级进度）
 * @note   载荷布局（12 字节）：
 *         [0]     阶段码
 *         [1]     百分比（0~100）
 *         [2..3]  详细错误码（LE16）
 *         [4..7]  当前值（LE32）
 *         [8..11] 总值（LE32）
 * @param  stage        — 阶段码
 * @param  percent      — 进度百分比
 * @param  detail_code  — 详细错误码
 * @param  current_value — 当前已处理值
 * @param  total_value   — 总值
 * @retval 1 — 发送成功；0 — 发送失败
 */
uint8_t ota_ctrl_send_status(uint8_t stage,
                             uint8_t percent,
                             uint16_t detail_code,
                             uint32_t current_value,
                             uint32_t total_value)
{
    uint8_t payload[OTA_CTRL_STATUS_PAYLOAD_LEN];

    /* 清零并填充载荷 */
    memset(payload, 0, sizeof(payload));
    payload[0] = stage;
    payload[1] = percent;
    ota_ctrl_write_u16le(&payload[2], detail_code);
    ota_ctrl_write_u32le(&payload[4], current_value);
    ota_ctrl_write_u32le(&payload[8], total_value);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_STATUS,
                               ota_ctrl_next_seq(),
                               payload,
                               OTA_CTRL_STATUS_PAYLOAD_LEN);
}

/* =========================================================================
 *  12. 内部函数 —— READY 信息提取
 * ======================================================================= */

/**
 * @brief  从 READY 帧中提取升级就绪信息
 * @note   载荷布局（OTA_CTRL_READY_PAYLOAD_LEN = 48 字节）：
 *         [0]      目标分区
 *         [2..3]   就绪标志位（LE16）
 *         [4..19]  目标版本字符串（16 字节）
 *         [20..23] 明文固件大小（LE32）
 *         [24..27] 传输大小（LE32）
 *         [28..31] 检查点间隔（LE32）
 *         [32..63] 会话指纹（32 字节）
 * @param  frame      — 接收到的帧
 * @param  ready_info — 就绪信息输出
 * @retval 1 — 提取成功；0 — 帧无效或版本格式错误
 */
static uint8_t ota_ctrl_extract_ready_info(const ota_ctrl_frame_t *frame,
                                           ota_ctrl_ready_info_t *ready_info)
{
    if (frame == 0 || ready_info == 0 || frame->payload_len < OTA_CTRL_READY_PAYLOAD_LEN)
    {
        return 0U;
    }

    /* 解析各字段 */
    memset(ready_info, 0, sizeof(*ready_info));
    ready_info->target_partition = frame->payload[0];
    ready_info->ready_flags = ota_ctrl_read_u16le(&frame->payload[2]);
    ota_ctrl_copy_ascii(ready_info->version,
                        (uint16_t)sizeof(ready_info->version),
                        &frame->payload[4],
                        OTA_CTRL_VERSION_LEN);
    ready_info->plain_size = ota_ctrl_read_u32le(&frame->payload[20]);
    ready_info->transfer_size = ota_ctrl_read_u32le(&frame->payload[24]);
    ready_info->checkpoint_size = ota_ctrl_read_u32le(&frame->payload[28]);
    memcpy(ready_info->session_fingerprint, &frame->payload[32], OTA_CTRL_FINGERPRINT_LEN);

    /* 验证版本字符串格式 */
    if (version_text_is_valid(ready_info->version) == 0U)
    {
        return 0U;
    }

    return 1U;
}

/* =========================================================================
 *  13. 公共接口 —— META 图像头接收
 * ======================================================================= */

/**
 * @brief  等待并接收 ESP32 通过 META 消息发送的固件镜像头
 * @note   图像头可能被拆分为多个 META 帧分片发送。
 *         每个 META 帧包含：类型（IMAGE_HEADER）+ 分片偏移 + 分片长度 + 总长度。
 *         接收流程：
 *         1. 循环等待 META 帧
 *         2. 验证分片偏移连续性
 *         3. 将分片数据拼接到 header 结构体
 *         4. 所有分片接收完毕后返回成功
 * @param  header    — 图像头输出结构体
 * @param  timeout_ms — 总超时时间（毫秒）
 * @retval 1 — 接收成功；0 — 超时或协议错误
 */
uint8_t ota_ctrl_wait_for_meta_image_header(OtaImageHeaderBinary *header, uint32_t timeout_ms)
{
    ota_ctrl_frame_t frame;
    uint32_t waited_ms = 0U;
    uint16_t expected_offset = 0U;                  /* 下一个期望的分片偏移 */
    uint16_t total_length = 0U;                     /* 图像头总长度 */

    if (header == 0)
    {
        return 0U;
    }

    memset(header, 0, sizeof(*header));

    /* 循环接收 META 帧分片 */
    while (waited_ms < timeout_ms)
    {
        uint16_t chunk_offset = 0U;
        uint16_t chunk_len = 0U;
        uint16_t frame_total = 0U;

        /* 尝试接收一帧（OTA_CTRL_FRAME_WAIT_MS 超时） */
        if (ota_ctrl_receive_frame(&frame, OTA_CTRL_FRAME_WAIT_MS) == 0U)
        {
            waited_ms += OTA_CTRL_FRAME_WAIT_MS;
            continue;
        }

        /* 过滤非 META 帧或类型不匹配的帧 */
        if (frame.msg_type != OTA_CTRL_MSG_META ||
            frame.payload_len < OTA_CTRL_META_PAYLOAD_HDR_LEN ||
            frame.payload[0] != OTA_CTRL_META_KIND_IMAGE_HEADER)
        {
            continue;
        }

        /* 解析分片元数据 */
        chunk_offset = ota_ctrl_read_u16le(&frame.payload[2]);
        chunk_len = ota_ctrl_read_u16le(&frame.payload[4]);
        frame_total = ota_ctrl_read_u16le(&frame.payload[6]);

        /* 分片完整性校验 */
        if (frame_total != OTA_IMAGE_HEADER_TOTAL_SIZE ||
            chunk_offset != expected_offset ||
            (uint16_t)(chunk_offset + chunk_len) > frame_total ||
            (uint16_t)(OTA_CTRL_META_PAYLOAD_HDR_LEN + chunk_len) != frame.payload_len)
        {
            return 0U;
        }

        /* 将分片数据拼接到 header 结构体 */
        memcpy(((uint8_t *)header) + chunk_offset,
               &frame.payload[OTA_CTRL_META_PAYLOAD_HDR_LEN],
               chunk_len);
        expected_offset = (uint16_t)(expected_offset + chunk_len);
        total_length = frame_total;

        /* 所有分片接收完毕 */
        if (expected_offset == total_length)
        {
            return 1U;
        }
    }

    return 0U;  /* 超时 */
}

/* =========================================================================
 *  14. 公共接口 —— RESULT 消息发送
 * ======================================================================= */

/**
 * @brief  发送 RESULT 消息（通知 ESP32 升级最终结果）
 * @note   载荷布局（8 字节）：
 *         [0]     结果（成功/失败）
 *         [1]     阶段码
 *         [2..3]  错误码（LE16）
 *         [4..7]  最终偏移量（LE32）
 * @param  outcome      — 结果码
 * @param  stage        — 阶段码
 * @param  error_code   — 错误码
 * @param  final_offset — 最终偏移量
 * @retval 1 — 发送成功；0 — 发送失败
 */
uint8_t ota_ctrl_send_result(uint8_t outcome, uint8_t stage, uint16_t error_code, uint32_t final_offset)
{
    uint8_t payload[OTA_CTRL_RESULT_PAYLOAD_LEN];

    memset(payload, 0, sizeof(payload));
    payload[0] = outcome;
    payload[1] = stage;
    ota_ctrl_write_u16le(&payload[2], error_code);
    ota_ctrl_write_u32le(&payload[4], final_offset);
    return ota_ctrl_send_frame(OTA_CTRL_MSG_RESULT, ota_ctrl_next_seq(), payload, OTA_CTRL_RESULT_PAYLOAD_LEN);
}

/* =========================================================================
 *  15. 内部函数 —— 就绪状态帧判断
 * ======================================================================= */

/**
 * @brief  判断 STATUS 帧是否为就绪状态（stage=READY）
 * @param  frame — 接收到的帧
 * @retval 1 — 是就绪状态帧；0 — 不是
 */
static uint8_t ota_ctrl_is_ready_status_frame(const ota_ctrl_frame_t *frame)
{
    if (frame == 0)
    {
        return 0U;
    }

    if (frame->msg_type != OTA_CTRL_MSG_STATUS ||
        frame->payload_len < OTA_CTRL_STATUS_PAYLOAD_LEN)
    {
        return 0U;
    }

    return (frame->payload[0] == OTA_CTRL_STAGE_READY) ? 1U : 0U;
}

/* =========================================================================
 *  16. 公共接口 —— 升级就绪等待（核心状态机）
 * ======================================================================= */

/**
 * @brief  等待 ESP32 返回升级就绪信息（OTA 控制协商主流程）
 * @note   状态机流程：
 *         阶段 1 — 发送 REQ 并等待 ACK/READY/ERROR/STATUS：
 *           - 收到 ACK（accept=1）→ 进入阶段 2
 *           - 收到 READY → 直接提取就绪信息并返回
 *           - 收到 ERROR/ACK（accept=0）→ 返回失败
 *           - 收到 STATUS → 跳转阶段 2
 *           - 重试次数耗尽 → 返回超时
 *         阶段 2 — 等待 READY/ERROR/STATUS：
 *           - 收到 READY → 提取就绪信息并返回
 *           - 收到 ERROR → 返回失败
 *           - 收到 STATUS（非 READY）→ 显示进度并继续等待
 *           - 超时 → 返回失败
 *
 * @param  boot_info     — BootInfo 指针
 * @param  ready_info    — 就绪信息输出
 * @param  reject_reason — 拒绝原因输出（可为 NULL）
 * @param  req_flags     — 请求标志位
 * @retval 1 — 升级就绪；0 — 被拒绝或超时
 */
uint8_t ota_ctrl_wait_for_upgrade_ready(const BootInfoTypeDef *boot_info,
                                        ota_ctrl_ready_info_t *ready_info,
                                        uint16_t *reject_reason,
                                        uint32_t req_flags)
{
    ota_ctrl_frame_t frame;
    uint8_t payload[OTA_CTRL_REQ_PAYLOAD_LEN];
    uint16_t payload_len = 0U;
    uint8_t ack_received = 0U;
    uint8_t req_seq = 0U;
    uint8_t retry = 0U;
    uint32_t waited_ms = 0U;

    /* 初始化输出参数 */
    if (reject_reason != 0)
    {
        *reject_reason = 0U;
    }

    if (ready_info != 0)
    {
        memset(ready_info, 0, sizeof(*ready_info));
    }

    /* 组装请求载荷 */
    if (ota_ctrl_prepare_request_payload(boot_info, payload, &payload_len, req_flags) == 0U)
    {
        return 0U;
    }

    /* 刷新 UART 并准备发送 */
    ota_ctrl_flush_uart();
    req_seq = ota_ctrl_next_seq();
    ota_ctrl_show_status_text("Send request", "To ESP32");

    /* ====== 阶段 1：发送 REQ 并等待 ACK ====== */
    while (retry < OTA_CTRL_REQ_RETRY_COUNT)
    {
        /* 发送 REQ 帧 */
        if (ota_ctrl_send_frame(OTA_CTRL_MSG_REQ, req_seq, payload, payload_len) == 0U)
        {
            return 0U;
        }

        /* 等待应答帧 */
        if (ota_ctrl_receive_frame(&frame, OTA_CTRL_ACK_TIMEOUT_MS))
        {
            /* --- 处理 ACK --- */
            if (frame.msg_type == OTA_CTRL_MSG_ACK &&
                frame.payload_len >= OTA_CTRL_ACK_PAYLOAD_LEN)
            {
                if (frame.payload[0] == 1U)
                {
                    /* ACK 确认：进入阶段 2 */
                    ack_received = 1U;
                    break;
                }

                /* ACK 拒绝：提取拒绝原因并返回 */
                if (reject_reason != 0)
                {
                    *reject_reason = ota_ctrl_read_u16le(&frame.payload[4]);
                }
                ota_ctrl_show_ack_reject_reason(ota_ctrl_read_u16le(&frame.payload[4]));
                return 0U;
            }

            /* --- 处理 READY（直接就绪） --- */
            if (frame.msg_type == OTA_CTRL_MSG_READY &&
                frame.payload_len >= OTA_CTRL_READY_PAYLOAD_LEN)
            {
                /* 验证目标分区 */
                if (frame.payload[0] != (uint8_t)boot_info->target_partition)
                {
                    if (reject_reason != 0)
                    {
                        *reject_reason = OTA_CTRL_ERR_PARTITION;
                    }
                    ota_ctrl_show_error_code(OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PARTITION);
                    return 0U;
                }

                /* 提取就绪信息 */
                if (ota_ctrl_extract_ready_info(&frame, ready_info) == 0U)
                {
                    if (reject_reason != 0)
                    {
                        *reject_reason = OTA_CTRL_ERR_VERSION;
                    }
                    ota_ctrl_show_error_code(OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_VERSION);
                    return 0U;
                }

                ota_ctrl_show_ready_info(&frame);
                return 1U;
            }

            /* --- 处理 ERROR --- */
            if (frame.msg_type == OTA_CTRL_MSG_ERROR &&
                frame.payload_len >= OTA_CTRL_ERROR_PAYLOAD_LEN)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = ota_ctrl_read_u16le(&frame.payload[2]);
                }
                ota_ctrl_show_error_code(frame.payload[0], ota_ctrl_read_u16le(&frame.payload[2]));
                return 0U;
            }

            /* --- 处理 STATUS（就绪状态帧，跳转阶段 2） --- */
            if (frame.msg_type == OTA_CTRL_MSG_STATUS &&
                frame.payload_len >= OTA_CTRL_STATUS_PAYLOAD_LEN)
            {
                ack_received = 1U;
                ota_ctrl_show_stage(frame.payload[0],
                                    frame.payload[1],
                                    ota_ctrl_read_u16le(&frame.payload[2]),
                                    ota_ctrl_read_u32le(&frame.payload[4]),
                                    ota_ctrl_read_u32le(&frame.payload[8]));
                break;
            }
        }

        ++retry;
    }

    /* 阶段 1 结束：检查是否收到 ACK */
    if (ack_received == 0U)
    {
        ota_ctrl_show_status_text("ESP32 timeout", "No ACK");
        return 0U;
    }

    /* ====== 阶段 2：等待 READY ====== */
    ota_ctrl_show_status_text("ESP32 ACK", "Preparing");
    waited_ms = 0U;

    while (waited_ms < OTA_CTRL_READY_TIMEOUT_MS)
    {
        /* 尝试接收帧 */
        if (ota_ctrl_receive_frame(&frame, OTA_CTRL_FRAME_WAIT_MS) == 0U)
        {
            waited_ms += OTA_CTRL_FRAME_WAIT_MS;
            continue;
        }

        /* 处理 STATUS（非 READY 状态则显示进度） */
        if (frame.msg_type == OTA_CTRL_MSG_STATUS &&
            frame.payload_len >= OTA_CTRL_STATUS_PAYLOAD_LEN)
        {
            if (ota_ctrl_is_ready_status_frame(&frame) == 0U)
            {
                ota_ctrl_show_stage(frame.payload[0],
                                    frame.payload[1],
                                    ota_ctrl_read_u16le(&frame.payload[2]),
                                    ota_ctrl_read_u32le(&frame.payload[4]),
                                    ota_ctrl_read_u32le(&frame.payload[8]));
            }
            continue;
        }

        /* 处理 ERROR */
        if (frame.msg_type == OTA_CTRL_MSG_ERROR &&
            frame.payload_len >= OTA_CTRL_ERROR_PAYLOAD_LEN)
        {
            if (reject_reason != 0)
            {
                *reject_reason = ota_ctrl_read_u16le(&frame.payload[2]);
            }
            ota_ctrl_show_error_code(frame.payload[0], ota_ctrl_read_u16le(&frame.payload[2]));
            return 0U;
        }

        /* 处理 READY */
        if (frame.msg_type == OTA_CTRL_MSG_READY &&
            frame.payload_len >= OTA_CTRL_READY_PAYLOAD_LEN)
        {
            /* 验证目标分区 */
            if (frame.payload[0] != (uint8_t)boot_info->target_partition)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = OTA_CTRL_ERR_PARTITION;
                }
                ota_ctrl_show_error_code(OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_PARTITION);
                return 0U;
            }

            /* 提取就绪信息 */
            if (ota_ctrl_extract_ready_info(&frame, ready_info) == 0U)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = OTA_CTRL_ERR_VERSION;
                }
                ota_ctrl_show_error_code(OTA_CTRL_STAGE_READY, OTA_CTRL_ERR_VERSION);
                return 0U;
            }

            ota_ctrl_show_ready_info(&frame);
            return 1U;
        }
    }

    /* 阶段 2 超时 */
    ota_ctrl_show_status_text("ESP32 timeout", "No READY");
    return 0U;
}
