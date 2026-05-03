/**
 * @file    esp_host_service.c
 * @brief   ESP 主机通信服务模块
 * @note    本模块负责 STM32 与 ESP32 之间的 UART 通信，基于 OTA 控制协议
 *          实现主机状态查询、WiFi/BLE/MQTT 开关控制、电源策略设置、
 *          远程按键注入、热成像数据推送以及深度睡眠准备等功能。
 *
 * @par 通信协议
 *      采用帧格式：SOF1 + SOF2 + 协议版本 + 消息类型 + 序列号 + 载荷长度
 *      + 载荷数据 + CRC16 校验。支持自动重试和唤醒前导。
 *
 * @par 线程安全
 *      UART 访问通过互斥锁（s_uart_guard_mutex）保护，电源管理通过
 *      POWER_LOCK_ESP_HOST 锁保护，状态快照通过 PRIMASK 临界区保护。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "esp_host_service_priv.h"

#include <string.h>

#include "common.h"
#include "delay.h"
#include "exti_key.h"
#include "key.h"
#include "ota_ctrl_protocol.h"
#include "power_manager.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* =========================================================================
 *  2. 宏定义
 * ======================================================================= */

/** @defgroup ESP_HOST_CONST  ESP 主机通信内部常量
 *  @{ */
#define ESP_HOST_REQ_TIMEOUT_MS         400U    /**< 请求响应超时时间（ms）       */
#define ESP_HOST_OFFLINE_GRACE_MS       5000UL  /**< 离线判定宽限期（ms）         */
#define ESP_HOST_MAX_FAILURES           3U      /**< 连续失败次数阈值（标记离线） */
#define ESP_HOST_POLL_STEP_US           50U     /**< 轮询步进间隔（us）           */
#define ESP_HOST_READY_POLL_STEP_MS     50U     /**< 就绪状态轮询间隔（ms）       */
#define ESP_HOST_WAKE_PREAMBLE_COUNT    2U      /**< 唤醒前导字节发送次数         */
#define ESP_HOST_WAKE_PREAMBLE_DELAY_MS 4U      /**< 唤醒前导后延迟（ms）         */
#define ESP_HOST_RETRY_COUNT            2U      /**< 通信重试次数                 */
#define ESP_HOST_RETRY_DELAY_MS         20U     /**< 重试间隔（ms）               */
#define ESP_HOST_UART_LOCK_TIMEOUT_MS   10U     /**< UART 互斥锁获取超时（ms）    */
/** @} */

/* =========================================================================
 *  3. 内部数据类型定义
 * ======================================================================= */

/**
 * @brief ESP 主机通信帧结构体
 * @note  用于接收 ESP32 返回的完整帧数据。
 */
typedef struct
{
    uint8_t  msg_type;                          /**< 消息类型                      */
    uint8_t  seq;                               /**< 序列号                        */
    uint16_t payload_len;                       /**< 载荷长度                      */
    uint8_t  payload[OTA_CTRL_MAX_PAYLOAD_LEN]; /**< 载荷数据                      */
} esp_host_frame_t;

/* =========================================================================
 *  4. 模块级静态变量
 * ======================================================================= */

static esp_host_status_t s_status;              /**< ESP 主机状态缓存              */
static uint8_t  s_host_seq             = 1U;    /**< 通信序列号（从 1 开始）       */
static uint8_t  s_consecutive_failures = 0U;    /**< 连续通信失败计数              */
static uint8_t  s_forced_deep_sleep    = 0U;    /**< 强制深度睡眠标志              */
static SemaphoreHandle_t s_uart_guard_mutex = 0;/**< UART 访问互斥锁              */

/* =========================================================================
 *  5. 内部函数前向声明
 * ======================================================================= */

static uint8_t  esp_host_exchange(uint8_t cmd, uint8_t arg0, uint8_t arg1,
                                  uint8_t *response_payload);
static uint8_t  esp_host_exchange_payload(const uint8_t *request_payload,
                                          uint16_t request_len,
                                          uint8_t expected_cmd,
                                          uint8_t *response_payload);
static uint8_t  esp_host_encode_power_policy(power_policy_t policy);
static uint8_t  esp_host_encode_host_state(power_state_t state);
static uint8_t  esp_host_set_raw_host_state_now(uint8_t host_state,
                                                uint8_t *response_payload);
static uint8_t  esp_host_wait_ready_for_sleep(uint32_t timeout_ms);
static uint8_t  esp_host_scheduler_running(void);
static void     esp_host_delay_poll_step(uint32_t *waited_us, uint32_t timeout_us);

/* =========================================================================
 *  6. 内部工具函数实现
 * ======================================================================= */

/**
 * @brief  判断 FreeRTOS 调度器是否正在运行
 * @retval 1 — 调度器运行中；0 — 尚未启动或已挂起
 */
static uint8_t esp_host_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

/**
 * @brief  获取 UART 互斥锁
 * @note   首次调用时自动创建互斥锁。调度器未运行时直接返回成功。
 * @param  timeout_ms — 获取超时时间（ms）
 * @retval 1 — 获取成功；0 — 获取失败
 */
uint8_t esp_host_uart_guard_lock(uint32_t timeout_ms)
{
    TickType_t wait_ticks = 0U;

    if (esp_host_scheduler_running() == 0U)
    {
        return 1U;
    }

    if (s_uart_guard_mutex == 0)
    {
        return 0U;
    }

    wait_ticks = pdMS_TO_TICKS(timeout_ms);

    /* 防止非零超时被转换为零 tick（立即返回） */
    if (timeout_ms != 0U && wait_ticks == 0U)
    {
        wait_ticks = 1U;
    }

    return (xSemaphoreTake(s_uart_guard_mutex, wait_ticks) == pdPASS) ? 1U : 0U;
}

/**
 * @brief  释放 UART 互斥锁
 */
void esp_host_uart_guard_unlock(void)
{
    if (esp_host_scheduler_running() == 0U || s_uart_guard_mutex == 0)
    {
        return;
    }

    (void)xSemaphoreGive(s_uart_guard_mutex);
}

/**
 * @brief  轮询步进延迟
 * @note   调度器运行时使用 vTaskDelay（至少 1 tick），否则使用 delay_us。
 * @param  waited_us  — [in/out] 已等待时间（us），会被累加
 * @param  timeout_us — 超时阈值（us），用于钳位 waited_us
 */
static void esp_host_delay_poll_step(uint32_t *waited_us, uint32_t timeout_us)
{
    if (waited_us == 0)
    {
        return;
    }

    if (esp_host_scheduler_running() != 0U)
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

    delay_us(ESP_HOST_POLL_STEP_US);
    *waited_us += ESP_HOST_POLL_STEP_US;
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
static uint16_t esp_host_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc  = 0U;
    uint16_t bit  = 0U;

    while (length-- > 0U)
    {
        crc ^= (uint16_t)(*data++) << 8;

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

/**
 * @brief  将 16 位无符号整数写入缓冲区（小端序）
 * @param  buffer — 目标缓冲区
 * @param  value  — 待写入的值
 */
static void esp_host_write_u16le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/**
 * @brief  从缓冲区读取 16 位无符号整数（小端序）
 * @param  buffer — 源缓冲区
 * @return 读取到的值
 */
static uint16_t esp_host_read_u16le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

/**
 * @brief  从缓冲区读取 32 位无符号整数（小端序）
 * @param  buffer — 源缓冲区
 * @return 读取到的值
 */
static uint32_t esp_host_read_u32le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0]           |
           ((uint32_t)buffer[1] << 8)    |
           ((uint32_t)buffer[2] << 16)   |
           ((uint32_t)buffer[3] << 24);
}

/* =========================================================================
 *  8. 序列号与 UART 缓冲区管理
 * ======================================================================= */

/**
 * @brief  获取下一个通信序列号
 * @note   序列号从 1 开始递增，跳过 0（保留为无效值）。
 * @return 序列号
 */
static uint8_t esp_host_next_seq(void)
{
    if (s_host_seq == 0U)
    {
        s_host_seq = 1U;
    }

    return s_host_seq++;
}

/**
 * @brief  清空 UART 接收缓冲区
 * @note   读取并丢弃所有待处理的字节。
 */
static void esp_host_flush_uart(void)
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
static uint8_t esp_host_read_byte_timeout(uint8_t *byte, uint32_t timeout_ms)
{
    uint32_t waited_us  = 0U;
    uint32_t timeout_us = timeout_ms * 1000UL;

    while (waited_us < timeout_us)
    {
        if (SerialKeyPressed(byte) != 0U)
        {
            return 1U;
        }

        esp_host_delay_poll_step(&waited_us, timeout_us);
    }

    return 0U;
}

/* =========================================================================
 *  9. 帧发送与接收
 * ======================================================================= */

/**
 * @brief  发送一帧数据到 ESP32
 * @note   按协议格式组装帧头、载荷和 CRC16 校验，逐字节发送。
 * @param  msg_type    — 消息类型
 * @param  seq         — 序列号
 * @param  payload     — 载荷数据指针
 * @param  payload_len — 载荷长度
 * @retval 1 — 发送成功；0 — 载荷超长
 */
static uint8_t esp_host_send_frame(uint8_t msg_type,
                                   uint8_t seq,
                                   const uint8_t *payload,
                                   uint16_t payload_len)
{
    uint8_t  frame[OTA_CTRL_MAX_FRAME_LEN];
    uint16_t crc       = 0U;
    uint16_t total_len = 0U;
    uint16_t index     = 0U;

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
    esp_host_write_u16le(&frame[5], payload_len);

    /* 拷贝载荷 */
    for (index = 0U; index < payload_len; ++index)
    {
        frame[OTA_CTRL_HEADER_LEN + index] = payload[index];
    }

    /* 计算并追加 CRC16 */
    crc = esp_host_crc16(&frame[2], (uint16_t)(5U + payload_len));
    esp_host_write_u16le(&frame[OTA_CTRL_HEADER_LEN + payload_len], crc);

    /* 逐字节发送完整帧 */
    total_len = (uint16_t)(OTA_CTRL_FRAME_OVERHEAD + payload_len);

    for (index = 0U; index < total_len; ++index)
    {
        SerialPutChar(frame[index]);
    }

    return 1U;
}

/**
 * @brief  发送唤醒前导字节
 * @note   在正式通信前发送指定数量的 0x00 字节唤醒 ESP32，
 *         随后等待一段时间确保 ESP32 就绪。
 */
static void esp_host_send_wake_preamble(void)
{
    uint8_t index = 0U;

    for (index = 0U; index < ESP_HOST_WAKE_PREAMBLE_COUNT; ++index)
    {
        SerialPutChar(0x00U);
    }

    delay_ms(ESP_HOST_WAKE_PREAMBLE_DELAY_MS);
}

/**
 * @brief  从 ESP32 接收一帧数据
 * @note   在超时时间内持续监听 UART，检测帧起始标志（SOF1+SOF2），
 *         解析帧头、载荷和 CRC16 校验。
 * @param  frame      — [out] 接收帧结构体指针
 * @param  timeout_ms — 超时时间（ms）
 * @retval 1 — 接收成功；0 — 超时或校验失败
 */
static uint8_t esp_host_receive_frame(esp_host_frame_t *frame, uint32_t timeout_ms)
{
    uint8_t  ch = 0U;
    uint8_t  header[5];
    uint8_t  crc_bytes[2];
    uint8_t  crc_buffer[5U + OTA_CTRL_MAX_PAYLOAD_LEN];
    uint16_t crc_recv = 0U;
    uint16_t crc_calc = 0U;
    uint16_t index    = 0U;
    uint32_t waited_ms = 0U;

    if (frame == 0)
    {
        return 0U;
    }

    while (waited_ms < timeout_ms)
    {
        /* 逐字节读取，每次超时 1ms */
        if (esp_host_read_byte_timeout(&ch, 1U) == 0U)
        {
            ++waited_ms;
            continue;
        }

        /* 检测帧起始标志第一字节 */
        if (ch != OTA_CTRL_SOF1)
        {
            continue;
        }

        /* 检测帧起始标志第二字节 */
        if (esp_host_read_byte_timeout(&ch, 20U) == 0U)
        {
            return 0U;
        }

        if (ch != OTA_CTRL_SOF2)
        {
            continue;
        }

        /* 读取帧头（协议版本 + 消息类型 + 序列号 + 载荷长度） */
        for (index = 0U; index < sizeof(header); ++index)
        {
            if (esp_host_read_byte_timeout(&header[index], 20U) == 0U)
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
        frame->payload_len = esp_host_read_u16le(&header[3]);

        if (frame->payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
        {
            esp_host_flush_uart();
            return 0U;
        }

        /* 读取载荷数据 */
        for (index = 0U; index < frame->payload_len; ++index)
        {
            if (esp_host_read_byte_timeout(&frame->payload[index], 20U) == 0U)
            {
                return 0U;
            }
        }

        /* 读取 CRC16 校验值 */
        if (esp_host_read_byte_timeout(&crc_bytes[0], 20U) == 0U ||
            esp_host_read_byte_timeout(&crc_bytes[1], 20U) == 0U)
        {
            return 0U;
        }

        /* 组装 CRC 校验缓冲区（帧头 + 载荷） */
        crc_recv = esp_host_read_u16le(crc_bytes);

        for (index = 0U; index < sizeof(header); ++index)
        {
            crc_buffer[index] = header[index];
        }

        for (index = 0U; index < frame->payload_len; ++index)
        {
            crc_buffer[sizeof(header) + index] = frame->payload[index];
        }

        /* 校验 CRC */
        crc_calc = esp_host_crc16(crc_buffer,
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
 *  10. 状态管理与缓存更新
 * ======================================================================= */

/**
 * @brief  根据状态位更新 ESP 主机状态缓存
 * @param  status_bits — 32 位状态位掩码
 */
static void esp_host_apply_status_bits(uint32_t status_bits)
{
    s_status.wifi_enabled         = ((status_bits & OTA_HOST_STATUS_WIFI_ENABLED)         != 0U) ? 1U : 0U;
    s_status.wifi_connected       = ((status_bits & OTA_HOST_STATUS_WIFI_CONNECTED)       != 0U) ? 1U : 0U;
    s_status.ble_enabled          = ((status_bits & OTA_HOST_STATUS_BLE_ENABLED)          != 0U) ? 1U : 0U;
    s_status.ble_connected        = ((status_bits & OTA_HOST_STATUS_BLE_CONNECTED)        != 0U) ? 1U : 0U;
    s_status.mqtt_enabled         = ((status_bits & OTA_HOST_STATUS_MQTT_ENABLED)         != 0U) ? 1U : 0U;
    s_status.mqtt_connected       = ((status_bits & OTA_HOST_STATUS_MQTT_CONNECTED)       != 0U) ? 1U : 0U;
    s_status.debug_screen_enabled = ((status_bits & OTA_HOST_STATUS_DEBUG_SCREEN_ENABLED) != 0U) ? 1U : 0U;
    s_status.remote_keys_enabled  = ((status_bits & OTA_HOST_STATUS_REMOTE_KEYS_ENABLED)  != 0U) ? 1U : 0U;
    s_status.has_credentials      = ((status_bits & OTA_HOST_STATUS_HAS_CREDENTIALS)      != 0U) ? 1U : 0U;
    s_status.ready_for_sleep      = ((status_bits & OTA_HOST_STATUS_READY_FOR_SLEEP)      != 0U) ? 1U : 0U;
}

/**
 * @brief  记录一次通信成功
 * @note   标记 ESP 主机在线，更新最后可见时间，清零失败计数。
 */
static void esp_host_note_success(void)
{
    s_status.online         = 1U;
    s_status.last_seen_ms   = power_manager_get_tick_ms();
    s_consecutive_failures  = 0U;
    s_forced_deep_sleep     = 0U;
}

/**
 * @brief  记录一次通信失败
 * @note   累加连续失败计数，当满足以下任一条件时标记 ESP 主机离线：
 *         - 首次通信且连续失败 >= MAX_FAILURES
 *         - 曾通信过且（连续失败 >= MAX_FAILURES 或 距最后可见时间 >= 宽限期）
 */
static void esp_host_note_failure(void)
{
    uint32_t now_ms = power_manager_get_tick_ms();
    uint32_t age_ms = 0U;

    if (s_consecutive_failures < 0xFFU)
    {
        s_consecutive_failures++;
    }

    if (s_status.last_seen_ms != 0U)
    {
        age_ms = now_ms - s_status.last_seen_ms;
    }

    if ((s_status.last_seen_ms == 0U && s_consecutive_failures >= ESP_HOST_MAX_FAILURES) ||
        (s_status.last_seen_ms != 0U &&
         (s_consecutive_failures >= ESP_HOST_MAX_FAILURES || age_ms >= ESP_HOST_OFFLINE_GRACE_MS)))
    {
        s_status.online           = 0U;
        s_status.wifi_connected   = 0U;
        s_status.ble_connected    = 0U;
        s_status.mqtt_connected   = 0U;
    }
}

/* =========================================================================
 *  11. 命令封装与缓存同步
 * ======================================================================= */

/**
 * @brief  发送简单命令并检查结果
 * @param  cmd             — 命令 ID
 * @param  arg0            — 参数 0
 * @param  response_payload — 响应载荷缓冲区（可为 NULL）
 * @retval 1 — 命令执行成功；0 — 通信失败或结果非 OK
 */
static uint8_t esp_host_command(uint8_t cmd,
                                uint8_t arg0,
                                uint8_t *response_payload)
{
    uint8_t local_response[OTA_CTRL_HOST_PAYLOAD_LEN];
    uint8_t *response = response_payload;

    if (response == 0)
    {
        response = local_response;
    }

    if (esp_host_exchange(cmd, arg0, 0U, response) == 0U)
    {
        esp_host_note_failure();
        return 0U;
    }

    return (response[3] == OTA_HOST_RESULT_OK) ? 1U : 0U;
}

/**
 * @brief  根据命令 ID 更新本地状态缓存中的开关字段
 * @param  cmd     — 命令 ID
 * @param  enabled — 开关状态
 */
static void esp_host_update_cached_switch(uint8_t cmd, uint8_t enabled)
{
    switch (cmd)
    {
    case OTA_HOST_CMD_SET_WIFI:
        s_status.wifi_enabled = (enabled != 0U) ? 1U : 0U;
        if (enabled == 0U)
        {
            s_status.wifi_connected = 0U;
        }
        break;

    case OTA_HOST_CMD_SET_DEBUG_SCREEN:
        s_status.debug_screen_enabled = (enabled != 0U) ? 1U : 0U;
        break;

    case OTA_HOST_CMD_SET_REMOTE_KEYS:
        s_status.remote_keys_enabled = (enabled != 0U) ? 1U : 0U;
        break;

    case OTA_HOST_CMD_SET_BLE:
        s_status.ble_enabled = (enabled != 0U) ? 1U : 0U;
        if (enabled == 0U)
        {
            s_status.ble_connected = 0U;
        }
        break;

    case OTA_HOST_CMD_SET_MQTT:
        s_status.mqtt_enabled = (enabled != 0U) ? 1U : 0U;
        if (enabled == 0U)
        {
            s_status.mqtt_connected = 0U;
        }
        break;

    default:
        break;
    }
}

/* =========================================================================
 *  12. 电源策略与主机状态编码
 * ======================================================================= */

/**
 * @brief  将内部电源策略枚举转换为 OTA 协议定义的策略值
 * @param  policy — 内部电源策略
 * @return OTA 协议电源策略值
 */
static uint8_t esp_host_encode_power_policy(power_policy_t policy)
{
    switch (policy)
    {
    case POWER_POLICY_PERFORMANCE:
        return OTA_HOST_POWER_POLICY_PERFORMANCE;

    case POWER_POLICY_ECO:
        return OTA_HOST_POWER_POLICY_ECO;

    case POWER_POLICY_BALANCED:
    default:
        return OTA_HOST_POWER_POLICY_BALANCED;
    }
}

/**
 * @brief  将内部电源状态转换为 OTA 协议定义的主机状态
 * @param  state — 内部电源状态
 * @return OTA 协议主机状态值
 */
static uint8_t esp_host_encode_host_state(power_state_t state)
{
    return (state == POWER_STATE_SCREEN_OFF_IDLE)
               ? OTA_HOST_STATE_SCREEN_OFF
               : OTA_HOST_STATE_ACTIVE;
}

/**
 * @brief  直接发送主机状态设置命令
 * @param  host_state      — OTA 协议主机状态值
 * @param  response_payload — 响应载荷缓冲区
 * @retval 1 — 成功；0 — 失败
 */
static uint8_t esp_host_set_raw_host_state_now(uint8_t host_state,
                                               uint8_t *response_payload)
{
    return esp_host_command(OTA_HOST_CMD_SET_HOST_STATE, host_state, response_payload);
}

/**
 * @brief  等待 ESP32 就绪（ready_for_sleep 标志）
 * @note   周期性查询 ESP32 状态，直到 ready_for_sleep 置位或超时。
 * @param  timeout_ms — 超时时间（ms）
 * @retval 1 — 已就绪；0 — 超时
 */
static uint8_t esp_host_wait_ready_for_sleep(uint32_t timeout_ms)
{
    uint32_t start_ms = power_manager_get_tick_ms();

    if (s_status.ready_for_sleep != 0U)
    {
        return 1U;
    }

    while ((power_manager_get_tick_ms() - start_ms) < timeout_ms)
    {
        delay_ms(ESP_HOST_READY_POLL_STEP_MS);

        if (esp_host_refresh_status() != 0U && s_status.ready_for_sleep != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}

/* =========================================================================
 *  13. 远程按键注入
 * ======================================================================= */

/**
 * @brief  将 ESP32 返回的远程按键码转换为逻辑按键并注入按键队列
 * @param  pending_key — ESP32 远程按键码
 */
static void esp_host_inject_remote_key(uint8_t pending_key)
{
    uint8_t logical_key = 0U;

    switch (pending_key)
    {
    case OTA_HOST_REMOTE_KEY_1:
        logical_key = KEY1_PRES;
        break;

    case OTA_HOST_REMOTE_KEY_2:
        logical_key = KEY2_PRES;
        break;

    case OTA_HOST_REMOTE_KEY_3:
        logical_key = KEY3_PRES;
        break;

    default:
        break;
    }

    if (logical_key != 0U)
    {
        power_manager_notify_activity();
        KEY_PushEvent(logical_key);
    }
}

/* =========================================================================
 *  14. 核心通信交换函数
 * ======================================================================= */

/**
 * @brief  发送简单命令帧并接收响应
 * @note   将 cmd/arg0/arg1 封装为标准载荷，调用 esp_host_exchange_payload。
 * @param  cmd              — 命令 ID
 * @param  arg0             — 参数 0
 * @param  arg1             — 参数 1
 * @param  response_payload — 响应载荷缓冲区
 * @retval 1 — 通信成功；0 — 失败
 */
static uint8_t esp_host_exchange(uint8_t cmd,
                                 uint8_t arg0,
                                 uint8_t arg1,
                                 uint8_t *response_payload)
{
    uint8_t payload[OTA_CTRL_HOST_PAYLOAD_LEN];

    memset(payload, 0, sizeof(payload));
    payload[0] = cmd;
    payload[1] = arg0;
    payload[2] = arg1;

    return esp_host_exchange_payload(payload,
                                     OTA_CTRL_HOST_PAYLOAD_LEN,
                                     cmd,
                                     response_payload);
}

/**
 * @brief  发送自定义载荷帧并接收响应（核心交换逻辑）
 * @note   完整的请求-响应交换流程：
 *         1. 获取 UART 互斥锁和电源锁
 *         2. 清空接收缓冲区并发送唤醒前导
 *         3. 发送请求帧
 *         4. 等待并解析响应帧（匹配序列号和命令 ID）
 *         5. 更新状态缓存和远程按键
 *         6. 失败时自动重试（重新初始化 UART 后重试）
 * @param  request_payload  — 请求载荷指针
 * @param  request_len      — 请求载荷长度
 * @param  expected_cmd     — 期望的响应命令 ID
 * @param  response_payload — 响应载荷缓冲区（可为 NULL）
 * @retval 1 — 通信成功；0 — 失败
 */
static uint8_t esp_host_exchange_payload(const uint8_t *request_payload,
                                         uint16_t request_len,
                                         uint8_t expected_cmd,
                                         uint8_t *response_payload)
{
    esp_host_frame_t frame;
    uint8_t  seq         = 0U;
    uint32_t status_bits = 0U;
    uint8_t  attempt     = 0U;

    if (request_payload == 0 ||
        request_len == 0U ||
        request_len > OTA_CTRL_MAX_PAYLOAD_LEN)
    {
        return 0U;
    }

    /* 获取 UART 互斥锁 */
    if (esp_host_uart_guard_lock(ESP_HOST_UART_LOCK_TIMEOUT_MS) == 0U)
    {
        return 0U;
    }

    seq = esp_host_next_seq();
    power_manager_acquire_lock(POWER_LOCK_ESP_HOST);

    /* 重试循环 */
    for (attempt = 0U; attempt < ESP_HOST_RETRY_COUNT; ++attempt)
    {
        esp_host_flush_uart();
        esp_host_send_wake_preamble();

        /* 发送请求帧 */
        if (esp_host_send_frame(OTA_CTRL_MSG_HOST_REQ, seq,
                                request_payload, request_len) == 0U)
        {
            power_manager_release_lock(POWER_LOCK_ESP_HOST);
            esp_host_uart_guard_unlock();
            return 0U;
        }

        /* 等待并解析响应帧 */
        while (esp_host_receive_frame(&frame, ESP_HOST_REQ_TIMEOUT_MS) != 0U)
        {
            /* 过滤不匹配的帧 */
            if (frame.msg_type != OTA_CTRL_MSG_HOST_RSP ||
                frame.seq != seq ||
                frame.payload_len < OTA_CTRL_HOST_PAYLOAD_LEN ||
                frame.payload[0] != expected_cmd)
            {
                continue;
            }

            /* 拷贝响应载荷 */
            if (response_payload != 0)
            {
                memcpy(response_payload, frame.payload, OTA_CTRL_HOST_PAYLOAD_LEN);
            }

            /* 解析状态位并更新缓存 */
            status_bits = esp_host_read_u32le(&frame.payload[4]);
            esp_host_apply_status_bits(status_bits);
            esp_host_note_success();

            /* 注入远程按键 */
            esp_host_inject_remote_key(frame.payload[2]);

            power_manager_release_lock(POWER_LOCK_ESP_HOST);
            esp_host_uart_guard_unlock();
            return 1U;
        }

        /* 重试前重新初始化 UART */
        if ((attempt + 1U) < ESP_HOST_RETRY_COUNT)
        {
            uart_reinit_current_baud();
            delay_ms(ESP_HOST_RETRY_DELAY_MS);
        }
    }

    power_manager_release_lock(POWER_LOCK_ESP_HOST);
    esp_host_uart_guard_unlock();
    return 0U;
}

/* =========================================================================
 *  15. 公共接口实现 —— 初始化与周期处理
 * ======================================================================= */

/**
 * @brief  初始化 ESP 主机服务
 * @note   清零状态缓存、序列号、失败计数和强制深度睡眠标志。
 */
void esp_host_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_host_seq             = 1U;
    s_consecutive_failures = 0U;
    s_forced_deep_sleep    = 0U;

    if (s_uart_guard_mutex == 0)
    {
        s_uart_guard_mutex = xSemaphoreCreateMutex();
    }
}

/**
 * @brief  ESP 主机服务周期处理
 * @note   当前为稳定产品模式，不进行后台轮询。
 *         主机通信仅由用户操作或 OTA 流程触发。
 */
void esp_host_step(void)
{
    /* 稳定产品模式：无后台轮询 */
}

/* =========================================================================
 *  16. 公共接口实现 —— 热成像数据推送
 * ======================================================================= */

/**
 * @brief  发送热成像快照数据到 ESP32
 * @note   温度值为实际温度 x10 的有符号整数（如 36.5℃ = 365）。
 * @param  min_temp_x10    — 最低温度 x10
 * @param  max_temp_x10    — 最高温度 x10
 * @param  center_temp_x10 — 中心温度 x10
 * @retval 1 — 发送成功；0 — 失败
 */
uint8_t esp_host_send_thermal_snapshot_x10(int16_t min_temp_x10,
                                           int16_t max_temp_x10,
                                           int16_t center_temp_x10)
{
    uint8_t payload[OTA_CTRL_HOST_PAYLOAD_LEN];
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    memset(payload, 0, sizeof(payload));
    payload[0] = OTA_HOST_CMD_SEND_THERMAL_SNAPSHOT;
    esp_host_write_u16le(&payload[1], (uint16_t)min_temp_x10);
    esp_host_write_u16le(&payload[3], (uint16_t)max_temp_x10);
    esp_host_write_u16le(&payload[5], (uint16_t)center_temp_x10);

    if (esp_host_exchange_payload(payload,
                                  OTA_CTRL_HOST_PAYLOAD_LEN,
                                  OTA_HOST_CMD_SEND_THERMAL_SNAPSHOT,
                                  response) == 0U)
    {
        esp_host_note_failure();
        return 0U;
    }

    return (response[3] == OTA_HOST_RESULT_OK) ? 1U : 0U;
}

/* =========================================================================
 *  17. 公共接口实现 —— 状态查询
 * ======================================================================= */

/**
 * @brief  获取 ESP 主机状态指针
 * @note   返回内部缓存的指针，非线程安全（适用于单读者场景）。
 * @return 状态结构体指针
 */
const esp_host_status_t *esp_host_get_status(void)
{
    return &s_status;
}

/**
 * @brief  获取 ESP 主机状态的安全副本
 * @note   在 PRIMASK 临界区内拷贝，保证多任务/中断环境下的数据一致性。
 * @param  out_status — [out] 状态副本输出指针
 */
void esp_host_get_status_copy(esp_host_status_t *out_status)
{
    uint32_t primask = 0U;

    if (out_status == 0)
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    *out_status = s_status;

    if (primask == 0U)
    {
        __enable_irq();
    }
}

/**
 * @brief  刷新 ESP 主机状态
 * @note   发送 GET_STATUS 命令并更新本地缓存。
 * @retval 1 — 刷新成功；0 — 通信失败
 */
uint8_t esp_host_refresh_status(void)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    return esp_host_command(OTA_HOST_CMD_GET_STATUS, 0U, response);
}

/* =========================================================================
 *  18. 公共接口实现 —— WiFi / BLE / MQTT 开关控制
 * ======================================================================= */

/**
 * @brief  设置 WiFi 开关并可选等待连接
 * @param  enabled           — 1=开启，0=关闭
 * @param  wait_connected_ms — 开启时等待连接的超时时间（ms），0=不等待
 * @retval 1 — 命令执行成功；0 — 失败
 */
uint8_t esp_host_set_wifi_now(uint8_t enabled, uint32_t wait_connected_ms)
{
    uint8_t  response[OTA_CTRL_HOST_PAYLOAD_LEN];
    uint32_t start_ms = 0U;

    if (esp_host_command(OTA_HOST_CMD_SET_WIFI, enabled, response) == 0U)
    {
        return 0U;
    }

    esp_host_update_cached_switch(OTA_HOST_CMD_SET_WIFI, enabled);

    /* 关闭 WiFi 或不需要等待连接时直接返回 */
    if (enabled == 0U || wait_connected_ms == 0U)
    {
        return 1U;
    }

    /* 轮询等待 WiFi 连接 */
    start_ms = power_manager_get_tick_ms();

    while ((power_manager_get_tick_ms() - start_ms) < wait_connected_ms)
    {
        delay_ms(120U);

        if (esp_host_refresh_status() == 0U)
        {
            continue;
        }

        if (s_status.wifi_connected != 0U)
        {
            break;
        }
    }

    return 1U;
}

/**
 * @brief  设置 BLE 开关
 * @param  enabled — 1=开启，0=关闭
 * @retval 1 — 成功；0 — 失败
 */
uint8_t esp_host_set_ble_now(uint8_t enabled)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    if (esp_host_command(OTA_HOST_CMD_SET_BLE, enabled, response) == 0U)
    {
        return 0U;
    }

    esp_host_update_cached_switch(OTA_HOST_CMD_SET_BLE, enabled);
    return 1U;
}

/**
 * @brief  设置 MQTT 开关
 * @param  enabled — 1=开启，0=关闭
 * @retval 1 — 成功；0 — 失败
 */
uint8_t esp_host_set_mqtt_now(uint8_t enabled)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    if (esp_host_command(OTA_HOST_CMD_SET_MQTT, enabled, response) == 0U)
    {
        return 0U;
    }

    esp_host_update_cached_switch(OTA_HOST_CMD_SET_MQTT, enabled);
    return 1U;
}

/**
 * @brief  设置调试屏幕开关
 * @param  enabled — 1=开启，0=关闭
 * @retval 1 — 成功；0 — 失败
 */
uint8_t esp_host_set_debug_screen_now(uint8_t enabled)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    if (esp_host_command(OTA_HOST_CMD_SET_DEBUG_SCREEN, enabled, response) == 0U)
    {
        return 0U;
    }

    esp_host_update_cached_switch(OTA_HOST_CMD_SET_DEBUG_SCREEN, enabled);
    return 1U;
}

/**
 * @brief  设置远程按键开关
 * @param  enabled — 1=开启，0=关闭
 * @retval 1 — 成功；0 — 失败
 */
uint8_t esp_host_set_remote_keys_now(uint8_t enabled)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    if (esp_host_command(OTA_HOST_CMD_SET_REMOTE_KEYS, enabled, response) == 0U)
    {
        return 0U;
    }

    esp_host_update_cached_switch(OTA_HOST_CMD_SET_REMOTE_KEYS, enabled);
    return 1U;
}

/* =========================================================================
 *  19. 公共接口实现 —— 电源策略与主机状态控制
 * ======================================================================= */

/**
 * @brief  设置 ESP32 电源策略
 * @param  policy — 电源策略枚举
 * @retval 1 — 成功；0 — 失败
 * @note   强制深度睡眠模式下直接返回成功。
 */
uint8_t esp_host_set_power_policy_now(power_policy_t policy)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    if (s_forced_deep_sleep != 0U)
    {
        return 1U;
    }

    return esp_host_command(OTA_HOST_CMD_SET_POWER_POLICY,
                            esp_host_encode_power_policy(policy),
                            response);
}

/**
 * @brief  设置 ESP32 主机状态
 * @param  state — 内部电源状态枚举
 * @retval 1 — 成功；0 — 失败
 * @note   强制深度睡眠模式下直接返回成功。
 */
uint8_t esp_host_set_host_state_now(power_state_t state)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    if (s_forced_deep_sleep != 0U)
    {
        return 1U;
    }

    return esp_host_set_raw_host_state_now(esp_host_encode_host_state(state), response);
}

/* =========================================================================
 *  20. 公共接口实现 —— 深度睡眠准备
 * ======================================================================= */

/**
 * @brief  准备 ESP32 进入停机（Stop）模式
 * @note   发送停机空闲状态命令，然后等待 ESP32 就绪。
 * @param  timeout_ms — 等待就绪的超时时间（ms）
 * @retval 1 — 准备完成；0 — 失败或超时
 */
uint8_t esp_host_prepare_for_stop(uint32_t timeout_ms)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    if (s_forced_deep_sleep != 0U)
    {
        return 1U;
    }

    if (esp_host_set_raw_host_state_now(OTA_HOST_STATE_STOP_IDLE, response) == 0U)
    {
        return 0U;
    }

    if (s_status.ready_for_sleep != 0U)
    {
        return 1U;
    }

    return esp_host_wait_ready_for_sleep(timeout_ms);
}

/**
 * @brief  准备 ESP32 进入待机（Standby）模式
 * @note   发送待机准备状态命令，然后等待 ESP32 就绪。
 * @param  timeout_ms — 等待就绪的超时时间（ms）
 * @retval 1 — 准备完成；0 — 失败或超时
 */
uint8_t esp_host_prepare_for_standby(uint32_t timeout_ms)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    if (s_forced_deep_sleep != 0U)
    {
        return 1U;
    }

    if (esp_host_set_raw_host_state_now(OTA_HOST_STATE_STANDBY_PREP, response) == 0U)
    {
        return 0U;
    }

    if (s_status.ready_for_sleep != 0U)
    {
        return 1U;
    }

    return esp_host_wait_ready_for_sleep(timeout_ms);
}

/**
 * @brief  强制 ESP32 进入深度睡眠
 * @note   先调用 prepare_for_standby，成功后标记强制深度睡眠标志
 *         并清零所有连接状态。
 * @param  timeout_ms — 等待就绪的超时时间（ms）
 * @retval 1 — 进入成功；0 — 准备阶段失败
 */
uint8_t esp_host_enter_forced_deep_sleep_now(uint32_t timeout_ms)
{
    if (esp_host_prepare_for_standby(timeout_ms) == 0U)
    {
        return 0U;
    }

    s_forced_deep_sleep             = 1U;
    s_status.online                 = 0U;
    s_status.ready_for_sleep        = 0U;
    s_status.wifi_connected         = 0U;
    s_status.ble_connected          = 0U;
    s_status.mqtt_connected         = 0U;

    return 1U;
}

/**
 * @brief  查询 ESP32 是否处于强制深度睡眠状态
 * @retval 1 — 已进入强制深度睡眠；0 — 未进入
 */
uint8_t esp_host_is_forced_deep_sleep(void)
{
    return s_forced_deep_sleep;
}

/* =========================================================================
 *  21. 公共接口实现 —— 异步开关控制（不等待结果）
 * ======================================================================= */

/**
 * @brief  异步设置 WiFi 开关（不等待连接）
 * @param  enabled — 1=开启，0=关闭
 */
void esp_host_set_wifi(uint8_t enabled)
{
    (void)esp_host_set_wifi_now(enabled, 0U);
}

/**
 * @brief  异步设置 BLE 开关
 * @param  enabled — 1=开启，0=关闭
 */
void esp_host_set_ble(uint8_t enabled)
{
    (void)esp_host_set_ble_now(enabled);
}

/**
 * @brief  异步设置 MQTT 开关
 * @param  enabled — 1=开启，0=关闭
 */
void esp_host_set_mqtt(uint8_t enabled)
{
    (void)esp_host_set_mqtt_now(enabled);
}

/**
 * @brief  异步设置调试屏幕开关
 * @param  enabled — 1=开启，0=关闭
 */
void esp_host_set_debug_screen(uint8_t enabled)
{
    (void)esp_host_set_debug_screen_now(enabled);
}

/**
 * @brief  异步设置远程按键开关
 * @param  enabled — 1=开启，0=关闭
 */
void esp_host_set_remote_keys(uint8_t enabled)
{
    (void)esp_host_set_remote_keys_now(enabled);
}
