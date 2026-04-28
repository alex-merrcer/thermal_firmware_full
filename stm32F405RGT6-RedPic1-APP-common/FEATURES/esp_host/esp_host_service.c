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

#define ESP_HOST_REQ_TIMEOUT_MS      400U
#define ESP_HOST_OFFLINE_GRACE_MS    5000UL
#define ESP_HOST_MAX_FAILURES        3U
#define ESP_HOST_POLL_STEP_US        50U
#define ESP_HOST_READY_POLL_STEP_MS  50U
#define ESP_HOST_WAKE_PREAMBLE_COUNT 2U
#define ESP_HOST_WAKE_PREAMBLE_DELAY_MS 4U
#define ESP_HOST_RETRY_COUNT         2U
#define ESP_HOST_RETRY_DELAY_MS      20U
#define ESP_HOST_UART_LOCK_TIMEOUT_MS 10U

typedef struct
{
    uint8_t msg_type;
    uint8_t seq;
    uint16_t payload_len;
    uint8_t payload[OTA_CTRL_MAX_PAYLOAD_LEN];
} esp_host_frame_t;

static esp_host_status_t s_status;
static uint8_t s_host_seq = 1U;
static uint8_t s_consecutive_failures = 0U;
static uint8_t s_forced_deep_sleep = 0U;
static SemaphoreHandle_t s_uart_guard_mutex = 0;

static uint8_t esp_host_exchange(uint8_t cmd,
                                 uint8_t arg0,
                                 uint8_t arg1,
                                 uint8_t *response_payload);
static uint8_t esp_host_exchange_payload(const uint8_t *request_payload,
                                         uint16_t request_len,
                                         uint8_t expected_cmd,
                                         uint8_t *response_payload);
static uint8_t esp_host_encode_power_policy(power_policy_t policy);
static uint8_t esp_host_encode_host_state(power_state_t state);
static uint8_t esp_host_set_raw_host_state_now(uint8_t host_state, uint8_t *response_payload);
static uint8_t esp_host_wait_ready_for_sleep(uint32_t timeout_ms);
static uint8_t esp_host_scheduler_running(void);
static void esp_host_delay_poll_step(uint32_t *waited_us, uint32_t timeout_us);

static uint8_t esp_host_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

uint8_t esp_host_uart_guard_lock(uint32_t timeout_ms)
{
    TickType_t wait_ticks = 0U;

    if (esp_host_scheduler_running() == 0U)
    {
        return 1U;
    }

    if (s_uart_guard_mutex == 0)
    {
        s_uart_guard_mutex = xSemaphoreCreateMutex();
        if (s_uart_guard_mutex == 0)
        {
            return 0U;
        }
    }

    wait_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms != 0U && wait_ticks == 0U)
    {
        wait_ticks = 1U;
    }

    return (xSemaphoreTake(s_uart_guard_mutex, wait_ticks) == pdPASS) ? 1U : 0U;
}

void esp_host_uart_guard_unlock(void)
{
    if (esp_host_scheduler_running() == 0U || s_uart_guard_mutex == 0)
    {
        return;
    }

    (void)xSemaphoreGive(s_uart_guard_mutex);
}

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

static uint16_t esp_host_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0U;
    uint16_t bit = 0U;

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

static void esp_host_write_u16le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static uint16_t esp_host_read_u16le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static uint32_t esp_host_read_u32le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

static uint8_t esp_host_next_seq(void)
{
    if (s_host_seq == 0U)
    {
        s_host_seq = 1U;
    }

    return s_host_seq++;
}

static void esp_host_flush_uart(void)
{
    uint8_t ch = 0U;

    while (SerialKeyPressed(&ch) != 0U)
    {
    }
}

static uint8_t esp_host_read_byte_timeout(uint8_t *byte, uint32_t timeout_ms)
{
    uint32_t waited_us = 0U;
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

static uint8_t esp_host_send_frame(uint8_t msg_type,
                                   uint8_t seq,
                                   const uint8_t *payload,
                                   uint16_t payload_len)
{
    uint8_t frame[OTA_CTRL_MAX_FRAME_LEN];
    uint16_t crc = 0U;
    uint16_t total_len = 0U;
    uint16_t index = 0U;

    if (payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
    {
        return 0U;
    }

    frame[0] = OTA_CTRL_SOF1;
    frame[1] = OTA_CTRL_SOF2;
    frame[2] = OTA_CTRL_PROTOCOL_VERSION;
    frame[3] = msg_type;
    frame[4] = seq;
    esp_host_write_u16le(&frame[5], payload_len);

    for (index = 0U; index < payload_len; ++index)
    {
        frame[OTA_CTRL_HEADER_LEN + index] = payload[index];
    }

    crc = esp_host_crc16(&frame[2], (uint16_t)(5U + payload_len));
    esp_host_write_u16le(&frame[OTA_CTRL_HEADER_LEN + payload_len], crc);

    total_len = (uint16_t)(OTA_CTRL_FRAME_OVERHEAD + payload_len);
    for (index = 0U; index < total_len; ++index)
    {
        SerialPutChar(frame[index]);
    }

    return 1U;
}

static void esp_host_send_wake_preamble(void)
{
    uint8_t index = 0U;

    for (index = 0U; index < ESP_HOST_WAKE_PREAMBLE_COUNT; ++index)
    {
        SerialPutChar(0x00U);
    }

    delay_ms(ESP_HOST_WAKE_PREAMBLE_DELAY_MS);
}

static uint8_t esp_host_receive_frame(esp_host_frame_t *frame, uint32_t timeout_ms)
{
    uint8_t ch = 0U;
    uint8_t header[5];
    uint8_t crc_bytes[2];
    uint8_t crc_buffer[5U + OTA_CTRL_MAX_PAYLOAD_LEN];
    uint16_t crc_recv = 0U;
    uint16_t crc_calc = 0U;
    uint16_t index = 0U;
    uint32_t waited_ms = 0U;

    if (frame == 0)
    {
        return 0U;
    }

    while (waited_ms < timeout_ms)
    {
        if (esp_host_read_byte_timeout(&ch, 1U) == 0U)
        {
            ++waited_ms;
            continue;
        }

        if (ch != OTA_CTRL_SOF1)
        {
            continue;
        }

        if (esp_host_read_byte_timeout(&ch, 20U) == 0U)
        {
            return 0U;
        }

        if (ch != OTA_CTRL_SOF2)
        {
            continue;
        }

        for (index = 0U; index < sizeof(header); ++index)
        {
            if (esp_host_read_byte_timeout(&header[index], 20U) == 0U)
            {
                return 0U;
            }
        }

        if (header[0] != OTA_CTRL_PROTOCOL_VERSION)
        {
            continue;
        }

        frame->msg_type = header[1];
        frame->seq = header[2];
        frame->payload_len = esp_host_read_u16le(&header[3]);
        if (frame->payload_len > OTA_CTRL_MAX_PAYLOAD_LEN)
        {
            esp_host_flush_uart();
            return 0U;
        }

        for (index = 0U; index < frame->payload_len; ++index)
        {
            if (esp_host_read_byte_timeout(&frame->payload[index], 20U) == 0U)
            {
                return 0U;
            }
        }

        if (esp_host_read_byte_timeout(&crc_bytes[0], 20U) == 0U ||
            esp_host_read_byte_timeout(&crc_bytes[1], 20U) == 0U)
        {
            return 0U;
        }

        crc_recv = esp_host_read_u16le(crc_bytes);
        for (index = 0U; index < sizeof(header); ++index)
        {
            crc_buffer[index] = header[index];
        }
        for (index = 0U; index < frame->payload_len; ++index)
        {
            crc_buffer[sizeof(header) + index] = frame->payload[index];
        }

        crc_calc = esp_host_crc16(crc_buffer, (uint16_t)(sizeof(header) + frame->payload_len));
        if (crc_recv != crc_calc)
        {
            continue;
        }

        return 1U;
    }

    return 0U;
}

static void esp_host_apply_status_bits(uint32_t status_bits)
{
    s_status.wifi_enabled = ((status_bits & OTA_HOST_STATUS_WIFI_ENABLED) != 0U) ? 1U : 0U;
    s_status.wifi_connected = ((status_bits & OTA_HOST_STATUS_WIFI_CONNECTED) != 0U) ? 1U : 0U;
    s_status.ble_enabled = ((status_bits & OTA_HOST_STATUS_BLE_ENABLED) != 0U) ? 1U : 0U;
    s_status.ble_connected = ((status_bits & OTA_HOST_STATUS_BLE_CONNECTED) != 0U) ? 1U : 0U;
    s_status.mqtt_enabled = ((status_bits & OTA_HOST_STATUS_MQTT_ENABLED) != 0U) ? 1U : 0U;
    s_status.debug_screen_enabled = ((status_bits & OTA_HOST_STATUS_DEBUG_SCREEN_ENABLED) != 0U) ? 1U : 0U;
    s_status.remote_keys_enabled = ((status_bits & OTA_HOST_STATUS_REMOTE_KEYS_ENABLED) != 0U) ? 1U : 0U;
    s_status.has_credentials = ((status_bits & OTA_HOST_STATUS_HAS_CREDENTIALS) != 0U) ? 1U : 0U;
    s_status.ready_for_sleep = ((status_bits & OTA_HOST_STATUS_READY_FOR_SLEEP) != 0U) ? 1U : 0U;
}

static void esp_host_note_success(void)
{
    s_status.online = 1U;
    s_status.last_seen_ms = power_manager_get_tick_ms();
    s_consecutive_failures = 0U;
    s_forced_deep_sleep = 0U;
}

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
        s_status.online = 0U;
        s_status.wifi_connected = 0U;
        s_status.ble_connected = 0U;
    }
}

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
        break;

    default:
        break;
    }
}

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

static uint8_t esp_host_encode_host_state(power_state_t state)
{
    return (state == POWER_STATE_SCREEN_OFF_IDLE) ? OTA_HOST_STATE_SCREEN_OFF : OTA_HOST_STATE_ACTIVE;
}

static uint8_t esp_host_set_raw_host_state_now(uint8_t host_state, uint8_t *response_payload)
{
    return esp_host_command(OTA_HOST_CMD_SET_HOST_STATE, host_state, response_payload);
}

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

static uint8_t esp_host_exchange_payload(const uint8_t *request_payload,
                                         uint16_t request_len,
                                         uint8_t expected_cmd,
                                         uint8_t *response_payload)
{
    esp_host_frame_t frame;
    uint8_t seq = 0U;
    uint32_t status_bits = 0U;
    uint8_t attempt = 0U;

    if (request_payload == 0 ||
        request_len == 0U ||
        request_len > OTA_CTRL_MAX_PAYLOAD_LEN)
    {
        return 0U;
    }

    if (esp_host_uart_guard_lock(ESP_HOST_UART_LOCK_TIMEOUT_MS) == 0U)
    {
        return 0U;
    }

    seq = esp_host_next_seq();
    power_manager_acquire_lock(POWER_LOCK_ESP_HOST);

    for (attempt = 0U; attempt < ESP_HOST_RETRY_COUNT; ++attempt)
    {
        esp_host_flush_uart();
        esp_host_send_wake_preamble();

        if (esp_host_send_frame(OTA_CTRL_MSG_HOST_REQ, seq, request_payload, request_len) == 0U)
        {
            power_manager_release_lock(POWER_LOCK_ESP_HOST);
            esp_host_uart_guard_unlock();
            return 0U;
        }

        while (esp_host_receive_frame(&frame, ESP_HOST_REQ_TIMEOUT_MS) != 0U)
        {
            if (frame.msg_type != OTA_CTRL_MSG_HOST_RSP ||
                frame.seq != seq ||
                frame.payload_len < OTA_CTRL_HOST_PAYLOAD_LEN ||
                frame.payload[0] != expected_cmd)
            {
                continue;
            }

            if (response_payload != 0)
            {
                memcpy(response_payload, frame.payload, OTA_CTRL_HOST_PAYLOAD_LEN);
            }

            status_bits = esp_host_read_u32le(&frame.payload[4]);
            esp_host_apply_status_bits(status_bits);
            esp_host_note_success();
            esp_host_inject_remote_key(frame.payload[2]);
            power_manager_release_lock(POWER_LOCK_ESP_HOST);
            esp_host_uart_guard_unlock();
            return 1U;
        }

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

void esp_host_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_host_seq = 1U;
    s_consecutive_failures = 0U;
    s_forced_deep_sleep = 0U;
}

void esp_host_step(void)
{
    /* Stable product mode: no background polling.
     * Host communication is triggered only by explicit user actions or OTA flows. */
}

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

const esp_host_status_t *esp_host_get_status(void)
{
    return &s_status;
}

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

uint8_t esp_host_refresh_status(void)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    return esp_host_command(OTA_HOST_CMD_GET_STATUS, 0U, response);
}

uint8_t esp_host_set_wifi_now(uint8_t enabled, uint32_t wait_connected_ms)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];
    uint32_t start_ms = 0U;

    if (esp_host_command(OTA_HOST_CMD_SET_WIFI, enabled, response) == 0U)
    {
        return 0U;
    }

    esp_host_update_cached_switch(OTA_HOST_CMD_SET_WIFI, enabled);

    if (enabled == 0U || wait_connected_ms == 0U)
    {
        return 1U;
    }

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

uint8_t esp_host_set_host_state_now(power_state_t state)
{
    uint8_t response[OTA_CTRL_HOST_PAYLOAD_LEN];

    if (s_forced_deep_sleep != 0U)
    {
        return 1U;
    }

    return esp_host_set_raw_host_state_now(esp_host_encode_host_state(state), response);
}

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

uint8_t esp_host_enter_forced_deep_sleep_now(uint32_t timeout_ms)
{
    if (esp_host_prepare_for_standby(timeout_ms) == 0U)
    {
        return 0U;
    }

    s_forced_deep_sleep = 1U;
    s_status.online = 0U;
    s_status.ready_for_sleep = 0U;
    s_status.wifi_connected = 0U;
    s_status.ble_connected = 0U;
    return 1U;
}

uint8_t esp_host_is_forced_deep_sleep(void)
{
    return s_forced_deep_sleep;
}

void esp_host_set_wifi(uint8_t enabled)
{
    (void)esp_host_set_wifi_now(enabled, 0U);
}

void esp_host_set_ble(uint8_t enabled)
{
    (void)esp_host_set_ble_now(enabled);
}

void esp_host_set_mqtt(uint8_t enabled)
{
    (void)esp_host_set_mqtt_now(enabled);
}

void esp_host_set_debug_screen(uint8_t enabled)
{
    (void)esp_host_set_debug_screen_now(enabled);
}

void esp_host_set_remote_keys(uint8_t enabled)
{
    (void)esp_host_set_remote_keys_now(enabled);
}
