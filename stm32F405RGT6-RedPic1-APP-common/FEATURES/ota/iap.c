/**
 * @file    iap.c
 * @brief   IAP（In-Application Programming）OTA 查询模块
 * @note    本模块实现 STM32 与 ESP32 之间的 OTA 版本查询协议。
 *          通过 UART 发送 OTA 请求帧，接收并解析 ACK / READY / ERROR 响应，
 *          获取最新固件版本号。
 *
 * @par 通信流程
 *      1. 构造请求载荷（设备信息 + 版本 + UID + 请求标志）
 *      2. 获取 UART 互斥锁
 *      3. 发送请求帧，等待 ACK 响应（支持重试）
 *      4. ACK 后等待 READY 帧，提取最新版本号
 *      5. 释放 UART 互斥锁
 *
 * @par 帧格式
 *      SOF1 + SOF2 + 协议版本 + 消息类型 + 序列号 + 载荷长度
 *      + 载荷数据 + CRC16 校验（CCITT 多项式 0x1021）
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "iap.h"
#include "delay.h"
#include "sys.h"
#include "flash_if.h"
#include "common.h"
#include "ota_ctrl_protocol.h"
#include "esp_host_service_priv.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* =========================================================================
 *  2. 宏定义
 * ======================================================================= */

/** @defgroup IAP_CONST  IAP 模块内部常量
 *  @{ */
#define APP_OTA_REQ_FLAG_BASE           0x00000003UL    /**< 请求基础标志位              */
#define APP_OTA_DEVICE_PRODUCT_ID       "LCD"           /**< 设备产品 ID                 */
#define APP_OTA_DEVICE_HW_REV           "A1"            /**< 设备硬件版本                */
#define APP_OTA_DEFAULT_VERSION         "0.0.0"         /**< 默认版本号                  */
#define APP_OTA_REQ_RETRY_COUNT         2U              /**< 请求重试次数                */
#define APP_OTA_ACK_TIMEOUT_MS          12000U          /**< ACK 响应超时（ms）          */
#define APP_OTA_READY_TIMEOUT_MS        12000U          /**< READY 帧等待超时（ms）      */
#define APP_OTA_UART_LOCK_TIMEOUT_MS    60000U          /**< UART 互斥锁获取超时（ms）   */
#define APP_OTA_FRAME_WAIT_MS           500U            /**< 单帧等待超时（ms）          */
#define APP_OTA_POLL_STEP_US            50U             /**< 轮询步进间隔（us）          */
#define APP_OTA_STM32_UID_BASE_ADDR     0x1FFF7A10U     /**< STM32 UID 基地址            */
/** @} */

/* =========================================================================
 *  3. 内部数据类型定义
 * ======================================================================= */

/**
 * @brief OTA 控制帧结构体
 * @note  用于接收 ESP32 返回的完整帧数据。
 */
typedef struct
{
    uint8_t  msg_type;                          /**< 消息类型                      */
    uint8_t  seq;                               /**< 序列号                        */
    uint16_t payload_len;                       /**< 载荷长度                      */
    uint8_t  payload[OTA_CTRL_MAX_PAYLOAD_LEN]; /**< 载荷数据                      */
} app_ota_ctrl_frame_t;

/* =========================================================================
 *  4. 模块级静态变量
 * ======================================================================= */

static uint8_t s_app_ota_ctrl_seq = 1U;        /**< 通信序列号（从 1 开始）       */

/* =========================================================================
 *  5. 内部工具函数
 * ======================================================================= */

/**
 * @brief  判断 FreeRTOS 调度器是否正在运行
 * @retval 1 — 调度器运行中；0 — 尚未启动或已挂起
 */
static uint8_t app_ota_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

/**
 * @brief  轮询步进延迟
 * @note   调度器运行时使用 vTaskDelay（至少 1 tick），否则使用 delay_us。
 * @param  waited_us  — [in/out] 已等待时间（us），会被累加
 * @param  timeout_us — 超时阈值（us），用于钳位 waited_us
 */
static void app_ota_delay_poll_step(uint32_t *waited_us, uint32_t timeout_us)
{
    if (waited_us == 0)
    {
        return;
    }

    if (app_ota_scheduler_running() != 0U)
    {
        TickType_t delay_ticks = pdMS_TO_TICKS(1U);

        if (delay_ticks == 0U)
        {
            delay_ticks = 1U;
        }

        vTaskDelay(delay_ticks);
        *waited_us += 1000UL;

        if (*waited_us > timeout_us)
        {
            *waited_us = timeout_us;
        }
        return;
    }

    delay_us(APP_OTA_POLL_STEP_US);
    *waited_us += APP_OTA_POLL_STEP_US;
}

/* =========================================================================
 *  6. 版本号校验
 * ======================================================================= */

/**
 * @brief  校验版本号字符串是否合法
 * @note   合法格式：X.Y.Z（X/Y/Z 为非负整数，无前导零）
 * @param  version — 版本号字符串
 * @retval 1 — 合法；0 — 非法
 */
static uint8_t app_ota_version_is_valid(const char *version)
{
    uint32_t i         = 0U;
    uint8_t  dot_count = 0U;
    uint8_t  has_digit = 0U;

    if (version == 0 || version[0] == '\0')
    {
        return 0U;
    }

    for (i = 0U; version[i] != '\0'; ++i)
    {
        char ch = version[i];

        if (ch >= '0' && ch <= '9')
        {
            has_digit = 1U;
            continue;
        }

        if (ch == '.')
        {
            /* 点号前必须有数字，且最多 2 个点号 */
            if (has_digit == 0U || dot_count >= 2U)
            {
                return 0U;
            }

            ++dot_count;
            has_digit = 0U;
            continue;
        }

        return 0U;
    }

    /* 最后一段必须有数字，且总共 2 个点号（3 段） */
    return (has_digit != 0U && dot_count == 2U) ? 1U : 0U;
}

/* =========================================================================
 *  7. CRC16 校验与字节序工具
 * ======================================================================= */

/**
 * @brief  计算 CRC16 校验值（CCITT 多项式 0x1021）
 * @param  data   — 数据指针
 * @param  length — 数据长度
 * @return CRC16 校验值
 */
static uint16_t app_ota_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0U;
    uint16_t i   = 0U;

    while (length-- > 0U)
    {
        crc ^= (uint16_t)(*data++) << 8;

        for (i = 0U; i < 8U; ++i)
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

/**
 * @brief  将 16 位无符号整数写入缓冲区（小端序）
 * @param  buffer — 目标缓冲区
 * @param  value  — 待写入的值
 */
static void app_ota_write_u16le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/**
 * @brief  将 32 位无符号整数写入缓冲区（小端序）
 * @param  buffer — 目标缓冲区
 * @param  value  — 待写入的值
 */
static void app_ota_write_u32le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/**
 * @brief  从缓冲区读取 16 位无符号整数（小端序）
 * @param  buffer — 源缓冲区
 * @return 读取到的值
 */
static uint16_t app_ota_read_u16le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

/* =========================================================================
 *  8. 字符串填充与拷贝工具
 * ======================================================================= */

/**
 * @brief  将字符串填充到固定长度的缓冲区（不足部分补零）
 * @param  target     — 目标缓冲区
 * @param  target_len — 目标缓冲区长度
 * @param  value      — 源字符串（可为 NULL）
 */
static void app_ota_fill_string(uint8_t *target, uint16_t target_len, const char *value)
{
    uint16_t i = 0U;

    memset(target, 0, target_len);

    if (value == 0)
    {
        return;
    }

    for (i = 0U; i < target_len && value[i] != '\0'; ++i)
    {
        target[i] = (uint8_t)value[i];
    }
}

/**
 * @brief  从字节缓冲区拷贝 ASCII 字符串
 * @param  target     — 目标字符串缓冲区
 * @param  target_len — 目标缓冲区长度
 * @param  source     — 源字节缓冲区
 * @param  source_len — 源缓冲区长度
 */
static void app_ota_copy_ascii(char *target,
                               uint16_t target_len,
                               const uint8_t *source,
                               uint16_t source_len)
{
    uint16_t i = 0U;

    if (target == 0 || target_len == 0U)
    {
        return;
    }

    memset(target, 0, target_len);

    if (source == 0)
    {
        return;
    }

    for (i = 0U; i + 1U < target_len && i < source_len && source[i] != '\0'; ++i)
    {
        target[i] = (char)source[i];
    }
}

/* =========================================================================
 *  9. 序列号与 UART 管理
 * ======================================================================= */

/**
 * @brief  获取下一个通信序列号
 * @note   序列号从 1 开始递增，跳过 0（保留为无效值）。
 * @return 序列号
 */
static uint8_t app_ota_next_seq(void)
{
    if (s_app_ota_ctrl_seq == 0U)
    {
        s_app_ota_ctrl_seq = 1U;
    }

    return s_app_ota_ctrl_seq++;
}

/**
 * @brief  清空 UART 接收缓冲区
 */
static void app_ota_flush_uart(void)
{
    uint8_t ch = 0U;

    while (SerialKeyPressed(&ch) != 0U)
    {
    }
}

/**
 * @brief  从 UART 读取一个字节（带超时）
 * @param  byte       — [out] 读取到的字节
 * @param  timeout_ms — 超时时间（ms）
 * @retval 1 — 读取成功；0 — 超时
 */
static uint8_t app_ota_read_byte_timeout(uint8_t *byte, uint32_t timeout_ms)
{
    uint32_t waited_us  = 0U;
    uint32_t timeout_us = timeout_ms * 1000U;

    while (waited_us < timeout_us)
    {
        if (SerialKeyPressed(byte) != 0U)
        {
            return 1U;
        }

        app_ota_delay_poll_step(&waited_us, timeout_us);
    }

    return 0U;
}

/* =========================================================================
 *  10. 帧发送与接收
 * ======================================================================= */

/**
 * @brief  发送一帧数据到 ESP32
 * @param  msg_type    — 消息类型
 * @param  seq         — 序列号
 * @param  payload     — 载荷数据指针
 * @param  payload_len — 载荷长度
 * @retval 1 — 发送成功；0 — 载荷超长
 */
static uint8_t app_ota_send_frame(uint8_t msg_type,
                                  uint8_t seq,
                                  const uint8_t *payload,
                                  uint16_t payload_len)
{
    uint8_t  frame[OTA_CTRL_MAX_FRAME_LEN];
    uint16_t total_len = 0U;
    uint16_t crc       = 0U;
    uint16_t i         = 0U;

    if (payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
    {
        return 0U;
    }

    /* 组装帧头 */
    frame[0] = OTA_CTRL_SOF1;
    frame[1] = OTA_CTRL_SOF2;
    frame[2] = OTA_CTRL_PROTOCOL_VERSION;
    frame[3] = msg_type;
    frame[4] = seq;
    app_ota_write_u16le(&frame[5], payload_len);

    /* 拷贝载荷 */
    for (i = 0U; i < payload_len; ++i)
    {
        frame[OTA_CTRL_HEADER_LEN + i] = payload[i];
    }

    /* 计算并追加 CRC16 */
    crc = app_ota_crc16(&frame[2], (uint16_t)(5U + payload_len));
    app_ota_write_u16le(&frame[OTA_CTRL_HEADER_LEN + payload_len], crc);

    /* 逐字节发送完整帧 */
    total_len = (uint16_t)(OTA_CTRL_FRAME_OVERHEAD + payload_len);

    for (i = 0U; i < total_len; ++i)
    {
        SerialPutChar(frame[i]);
    }

    return 1U;
}

/**
 * @brief  从 ESP32 接收一帧数据
 * @note   在超时时间内持续监听 UART，检测帧起始标志（SOF1+SOF2），
 *         解析帧头、载荷和 CRC16 校验。
 * @param  frame      — [out] 接收帧结构体指针
 * @param  timeout_ms — 超时时间（ms）
 * @retval 1 — 接收成功；0 — 超时或校验失败
 */
static uint8_t app_ota_receive_frame(app_ota_ctrl_frame_t *frame, uint32_t timeout_ms)
{
    uint8_t  ch = 0U;
    uint8_t  header[5];
    uint8_t  crc_bytes[2];
    uint8_t  crc_buffer[5U + OTA_CTRL_MAX_PAYLOAD_LEN];
    uint16_t crc_calc = 0U;
    uint16_t crc_recv = 0U;
    uint16_t i        = 0U;
    uint32_t waited   = 0U;

    while (waited < timeout_ms)
    {
        /* 逐字节读取，每次超时 1ms */
        if (app_ota_read_byte_timeout(&ch, 1U) == 0U)
        {
            ++waited;
            continue;
        }

        /* 检测帧起始标志第一字节 */
        if (ch != OTA_CTRL_SOF1)
        {
            continue;
        }

        /* 检测帧起始标志第二字节 */
        if (app_ota_read_byte_timeout(&ch, 20U) == 0U)
        {
            return 0U;
        }

        if (ch != OTA_CTRL_SOF2)
        {
            continue;
        }

        /* 读取帧头 */
        for (i = 0U; i < sizeof(header); ++i)
        {
            if (app_ota_read_byte_timeout(&header[i], 20U) == 0U)
            {
                return 0U;
            }
        }

        /* 校验协议版本 */
        if (header[0] != OTA_CTRL_PROTOCOL_VERSION)
        {
            continue;
        }

        /* 解析帧头字段 */
        frame->msg_type    = header[1];
        frame->seq         = header[2];
        frame->payload_len = app_ota_read_u16le(&header[3]);

        if (frame->payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
        {
            app_ota_flush_uart();
            return 0U;
        }

        /* 读取载荷数据 */
        for (i = 0U; i < frame->payload_len; ++i)
        {
            if (app_ota_read_byte_timeout(&frame->payload[i], 20U) == 0U)
            {
                return 0U;
            }
        }

        /* 读取 CRC16 校验值 */
        if (app_ota_read_byte_timeout(&crc_bytes[0], 20U) == 0U ||
            app_ota_read_byte_timeout(&crc_bytes[1], 20U) == 0U)
        {
            return 0U;
        }

        /* 组装 CRC 校验缓冲区 */
        crc_recv = app_ota_read_u16le(crc_bytes);

        for (i = 0U; i < sizeof(header); ++i)
        {
            crc_buffer[i] = header[i];
        }

        for (i = 0U; i < frame->payload_len; ++i)
        {
            crc_buffer[sizeof(header) + i] = frame->payload[i];
        }

        /* 校验 CRC */
        crc_calc = app_ota_crc16(crc_buffer,
                                 (uint16_t)(sizeof(header) + frame->payload_len));

        if (crc_recv != crc_calc)
        {
            continue;
        }

        return 1U;
    }

    return 0U;
}

/* =========================================================================
 *  11. 请求载荷构造与响应解析
 * ======================================================================= */

/**
 * @brief  构造 OTA 请求载荷
 * @note   填充设备信息：请求类型、分区信息、版本号、产品 ID、
 *         硬件版本、STM32 UID、请求标志。
 * @param  boot_info   — 启动信息指针
 * @param  payload     — [out] 载荷缓冲区
 * @param  payload_len — [out] 载荷长度
 * @param  req_flags   — 请求标志位
 * @retval 1 — 构造成功；0 — 参数无效
 */
static uint8_t app_ota_prepare_request_payload(const BootInfoTypeDef *boot_info,
                                               uint8_t *payload,
                                               uint16_t *payload_len,
                                               uint32_t req_flags)
{
    const char    *version = APP_OTA_DEFAULT_VERSION;
    const uint8_t *uid     = (const uint8_t *)APP_OTA_STM32_UID_BASE_ADDR;
    uint16_t i = 0U;

    if (boot_info == 0 || payload == 0 || payload_len == 0)
    {
        return 0U;
    }

    memset(payload, 0, OTA_CTRL_REQ_PAYLOAD_LEN);

    /* 填充请求头字段 */
    payload[0] = OTA_CTRL_REQ_TYPE_UPGRADE;
    payload[1] = (uint8_t)boot_info->active_partition;
    payload[2] = (uint8_t)boot_info->target_partition;
    payload[3] = app_ota_version_is_valid(boot_info->current_version) ? 1U : 0U;

    if (payload[3] != 0U)
    {
        version = boot_info->current_version;
    }

    /* 填充版本号、产品 ID、硬件版本 */
    app_ota_fill_string(&payload[4],  OTA_CTRL_VERSION_LEN,    version);
    app_ota_fill_string(&payload[20], OTA_CTRL_PRODUCT_ID_LEN, APP_OTA_DEVICE_PRODUCT_ID);
    app_ota_fill_string(&payload[36], OTA_CTRL_HW_REV_LEN,     APP_OTA_DEVICE_HW_REV);

    /* 填充 STM32 UID（12 字节） */
    for (i = 0U; i < OTA_CTRL_UID_LEN; ++i)
    {
        payload[44 + i] = uid[i];
    }

    /* 填充请求标志 */
    app_ota_write_u32le(&payload[56], req_flags);
    *payload_len = OTA_CTRL_REQ_PAYLOAD_LEN;

    return 1U;
}

/**
 * @brief  从 READY 帧中提取最新版本号
 * @param  frame        — READY 帧指针
 * @param  version      — [out] 版本号缓冲区
 * @param  version_len  — 版本号缓冲区长度
 * @retval 1 — 提取成功且版本合法；0 — 失败
 */
static uint8_t app_ota_extract_ready_version(const app_ota_ctrl_frame_t *frame,
                                             char *version,
                                             uint16_t version_len)
{
    if (frame == 0 || version == 0 || version_len == 0U ||
        frame->payload_len < OTA_CTRL_READY_PAYLOAD_LEN)
    {
        return 0U;
    }

    app_ota_copy_ascii(version, version_len, &frame->payload[4], OTA_CTRL_VERSION_LEN);
    return app_ota_version_is_valid(version);
}

/* =========================================================================
 *  12. 公共接口实现 —— 版本查询
 * ======================================================================= */

/**
 * @brief  查询最新 OTA 固件版本
 * @note   完整的版本查询流程：
 *         1. 构造请求载荷（带 CHECK_ONLY 标志）
 *         2. 获取 UART 互斥锁
 *         3. 发送请求帧，等待 ACK（支持重试）
 *         4. ACK 后等待 READY 帧，提取版本号
 *         5. 释放 UART 互斥锁
 * @param  boot_info           — 当前启动信息
 * @param  latest_version      — [out] 最新版本号缓冲区
 * @param  latest_version_len  — 缓冲区长度
 * @param  reject_reason       — [out] 拒绝原因码（可为 NULL）
 * @retval 1 — 查询成功；0 — 失败
 */
uint8_t iap_query_latest_version(const BootInfoTypeDef *boot_info,
                                 char *latest_version,
                                 uint16_t latest_version_len,
                                 uint16_t *reject_reason)
{
    app_ota_ctrl_frame_t frame;
    uint8_t  payload[OTA_CTRL_REQ_PAYLOAD_LEN];
    uint16_t payload_len    = 0U;
    uint8_t  ack_received   = 0U;
    uint8_t  req_seq        = 0U;
    uint8_t  retry          = 0U;
    uint8_t  result         = 0U;
    uint32_t waited_ms      = 0U;

    /* 初始化输出参数 */
    if (reject_reason != 0)
    {
        *reject_reason = 0U;
    }

    if (latest_version != 0 && latest_version_len > 0U)
    {
        latest_version[0] = '\0';
    }

    /* 构造请求载荷 */
    if (!app_ota_prepare_request_payload(boot_info, payload, &payload_len,
                                         APP_OTA_REQ_FLAG_BASE | OTA_CTRL_REQ_FLAG_CHECK_ONLY))
    {
        return 0U;
    }

    /* 获取 UART 互斥锁 */
    if (esp_host_uart_guard_lock(APP_OTA_UART_LOCK_TIMEOUT_MS) == 0U)
    {
        if (reject_reason != 0)
        {
            *reject_reason = OTA_CTRL_ERR_BUSY;
        }
        return 0U;
    }

    /* 清空 UART 并获取序列号 */
    app_ota_flush_uart();
    delay_ms(12U);
    app_ota_flush_uart();
    req_seq = app_ota_next_seq();

    /* ---- 阶段 1：发送请求并等待 ACK ---- */
    while (retry < APP_OTA_REQ_RETRY_COUNT)
    {
        if (!app_ota_send_frame(OTA_CTRL_MSG_REQ, req_seq, payload, payload_len))
        {
            goto cleanup;
        }

        if (app_ota_receive_frame(&frame, APP_OTA_ACK_TIMEOUT_MS))
        {
            /* ACK 响应 */
            if (frame.msg_type == OTA_CTRL_MSG_ACK &&
                frame.payload_len >= OTA_CTRL_ACK_PAYLOAD_LEN)
            {
                if (frame.payload[0] == 1U)
                {
                    ack_received = 1U;
                    break;
                }

                /* ACK 拒绝 */
                if (reject_reason != 0)
                {
                    *reject_reason = app_ota_read_u16le(&frame.payload[4]);
                }
                goto cleanup;
            }

            /* 直接收到 READY 帧（跳过 ACK） */
            if (frame.msg_type == OTA_CTRL_MSG_READY &&
                frame.payload_len >= OTA_CTRL_READY_PAYLOAD_LEN)
            {
                if (frame.payload[0] != (uint8_t)boot_info->target_partition)
                {
                    if (reject_reason != 0)
                    {
                        *reject_reason = OTA_CTRL_ERR_PARTITION;
                    }
                    goto cleanup;
                }

                if (app_ota_extract_ready_version(&frame, latest_version, latest_version_len) == 0U)
                {
                    if (reject_reason != 0)
                    {
                        *reject_reason = OTA_CTRL_ERR_VERSION;
                    }
                    goto cleanup;
                }

                result = 1U;
                goto cleanup;
            }

            /* 错误响应 */
            if (frame.msg_type == OTA_CTRL_MSG_ERROR &&
                frame.payload_len >= OTA_CTRL_ERROR_PAYLOAD_LEN)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = app_ota_read_u16le(&frame.payload[2]);
                }
                goto cleanup;
            }

            /* STATUS 帧视为 ACK */
            if (frame.msg_type == OTA_CTRL_MSG_STATUS &&
                frame.payload_len >= OTA_CTRL_STATUS_PAYLOAD_LEN)
            {
                ack_received = 1U;
                break;
            }
        }

        /* 重试 */
        ++retry;

        if (retry < APP_OTA_REQ_RETRY_COUNT)
        {
            delay_ms(20U);
            app_ota_flush_uart();
        }
    }

    if (!ack_received)
    {
        goto cleanup;
    }

    /* ---- 阶段 2：等待 READY 帧 ---- */
    while (waited_ms < APP_OTA_READY_TIMEOUT_MS)
    {
        if (!app_ota_receive_frame(&frame, APP_OTA_FRAME_WAIT_MS))
        {
            waited_ms += APP_OTA_FRAME_WAIT_MS;
            continue;
        }

        /* STATUS 帧跳过 */
        if (frame.msg_type == OTA_CTRL_MSG_STATUS &&
            frame.payload_len >= OTA_CTRL_STATUS_PAYLOAD_LEN)
        {
            continue;
        }

        /* 错误响应 */
        if (frame.msg_type == OTA_CTRL_MSG_ERROR &&
            frame.payload_len >= OTA_CTRL_ERROR_PAYLOAD_LEN)
        {
            if (reject_reason != 0)
            {
                *reject_reason = app_ota_read_u16le(&frame.payload[2]);
            }
            goto cleanup;
        }

        /* READY 帧：提取版本号 */
        if (frame.msg_type == OTA_CTRL_MSG_READY &&
            frame.payload_len >= OTA_CTRL_READY_PAYLOAD_LEN)
        {
            if (frame.payload[0] != (uint8_t)boot_info->target_partition)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = OTA_CTRL_ERR_PARTITION;
                }
                goto cleanup;
            }

            if (app_ota_extract_ready_version(&frame, latest_version, latest_version_len) == 0U)
            {
                if (reject_reason != 0)
                {
                    *reject_reason = OTA_CTRL_ERR_VERSION;
                }
                goto cleanup;
            }

            result = 1U;
            goto cleanup;
        }
    }

cleanup:
    esp_host_uart_guard_unlock();
    return result;
}
