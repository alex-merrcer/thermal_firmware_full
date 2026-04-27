#include "host_ctrl_service.h"

#include <inttypes.h>
#include <stdio.h>

#include "KEY.h"
#include "UART.h"
#include "WIFI.h"
#include "app_service_bus.h"
#include "ble_provision_service.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "freertos/task.h"
#include "lib_lcd7735.h"

static const char *TAG = "HOST_CTRL";

#define HOST_CTRL_LIGHT_WAKE_HOLD_KEY_MS    1500U
#define HOST_CTRL_LIGHT_WAKE_HOLD_LOCAL_KEY_MS 400U
#define HOST_CTRL_LIGHT_WAKE_HOLD_UART_MS    120U
#define HOST_CTRL_DEEP_SLEEP_WAKE_MS         (5ULL * 60ULL * 1000ULL)
#define HOST_CTRL_DEEP_CTX_MAGIC             0x48535443UL

typedef enum
{
    HOST_CTRL_SLEEP_NONE = 0U,
    HOST_CTRL_SLEEP_LIGHT,
    HOST_CTRL_SLEEP_DEEP
} host_ctrl_sleep_mode_t;

typedef struct
{
    uint32_t magic;
    uint8_t remote_keys_enabled;
    uint8_t reserved0;
    uint8_t reserved1;
    uint8_t reserved2;
} host_ctrl_rtc_ctx_t;

static RTC_DATA_ATTR host_ctrl_rtc_ctx_t s_rtc_ctx;

static uint8_t s_wifi_requested = 0U;
static uint8_t s_ble_requested = 0U;
static uint8_t s_ble_enabled = 0U;
static uint8_t s_debug_screen_requested = 0U;
static uint8_t s_debug_screen_visible = 0U;
static uint8_t s_remote_keys_enabled = 0U;
static uint8_t s_power_policy = OTA_HOST_POWER_POLICY_BALANCED;
static uint8_t s_host_state = OTA_HOST_STATE_ACTIVE;
static uint8_t s_pending_remote_key = OTA_HOST_REMOTE_KEY_NONE;
static uint8_t s_last_key = OTA_HOST_REMOTE_KEY_NONE;
static uint32_t s_last_logged_status_bits = 0xFFFFFFFFUL;
static uint8_t s_last_logged_result = 0xFFU;
static host_ctrl_sleep_mode_t s_pending_sleep = HOST_CTRL_SLEEP_NONE;
static TickType_t s_awake_hold_deadline = 0;
static uint8_t s_pending_runtime_apply = 0U;
static uint8_t s_manual_sleep_diag = 0U;
static uint8_t s_uart_wake_block_count = 0U;

static int16_t host_ctrl_read_i16le(const uint8_t *buffer)
{
    return (int16_t)ota_ctrl_read_u16le(buffer);
}

static void host_ctrl_log_thermal_snapshot(const uint8_t *payload, uint16_t payload_len)
{
    int16_t min_temp_x10 = 0;
    int16_t max_temp_x10 = 0;
    int16_t center_temp_x10 = 0;
    float min_temp = 0.0f;
    float max_temp = 0.0f;
    float center_temp = 0.0f;

    if (payload == NULL || payload_len < OTA_CTRL_HOST_PAYLOAD_LEN)
    {
        ESP_LOGW(TAG, "THERMAL snapshot payload too short: %u", (unsigned int)payload_len);
        return;
    }

    min_temp_x10 = host_ctrl_read_i16le(&payload[1]);
    max_temp_x10 = host_ctrl_read_i16le(&payload[3]);
    center_temp_x10 = host_ctrl_read_i16le(&payload[5]);
    min_temp = ((float)min_temp_x10) / 10.0f;
    max_temp = ((float)max_temp_x10) / 10.0f;
    center_temp = ((float)center_temp_x10) / 10.0f;

    ESP_LOGI(TAG,
             "THERMAL snapshot min=%.1fC max=%.1fC center=%.1fC",
             min_temp,
             max_temp,
             center_temp);
}

static void host_ctrl_forward_thermal_snapshot_to_cloud(const uint8_t *payload, uint16_t payload_len)
{
    int16_t min_temp_x10 = 0;
    int16_t max_temp_x10 = 0;
    int16_t center_temp_x10 = 0;
    esp_err_t err = ESP_OK;

    if (payload == NULL || payload_len < OTA_CTRL_HOST_PAYLOAD_LEN)
    {
        return;
    }

    min_temp_x10 = host_ctrl_read_i16le(&payload[1]);
    max_temp_x10 = host_ctrl_read_i16le(&payload[3]);
    center_temp_x10 = host_ctrl_read_i16le(&payload[5]);

    err = app_service_bus_submit_thermal_snapshot_x10(min_temp_x10, max_temp_x10, center_temp_x10);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Queue thermal snapshot to cloud service failed: 0x%04X", (unsigned int)err);
    }
}

static uint8_t host_ctrl_is_sleep_state(uint8_t host_state)
{
    return (host_state == OTA_HOST_STATE_STOP_IDLE || host_state == OTA_HOST_STATE_STANDBY_PREP) ? 1U : 0U;
}

static const char *host_ctrl_power_policy_text(uint8_t policy)
{
    switch (policy)
    {
    case OTA_HOST_POWER_POLICY_PERFORMANCE:
        return "PERF";

    case OTA_HOST_POWER_POLICY_ECO:
        return "ECO";

    case OTA_HOST_POWER_POLICY_BALANCED:
    default:
        return "BAL";
    }
}

static const char *host_ctrl_state_text(uint8_t state)
{
    switch (state)
    {
    case OTA_HOST_STATE_SCREEN_OFF:
        return "DIM";

    case OTA_HOST_STATE_STOP_IDLE:
        return "STOP";

    case OTA_HOST_STATE_STANDBY_PREP:
        return "STBY";

    case OTA_HOST_STATE_ACTIVE:
    default:
        return "ACT";
    }
}

static const char *host_ctrl_wakeup_cause_text(esp_sleep_wakeup_cause_t cause)
{
    switch (cause)
    {
    case ESP_SLEEP_WAKEUP_GPIO:
        return "GPIO";
    case ESP_SLEEP_WAKEUP_EXT0:
        return "EXT0";
    case ESP_SLEEP_WAKEUP_TIMER:
        return "TIMER";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        return "COLD";
    default:
        return "OTHER";
    }
}

static void host_ctrl_set_awake_hold_ms(uint32_t hold_ms)
{
    if (hold_ms == 0U)
    {
        s_awake_hold_deadline = 0;
        return;
    }

    s_awake_hold_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(hold_ms);
}

static uint8_t host_ctrl_is_awake_hold_active(void)
{
    if (s_awake_hold_deadline == 0)
    {
        return 0U;
    }

    return (((int32_t)(s_awake_hold_deadline - xTaskGetTickCount())) > 0) ? 1U : 0U;
}

static void host_ctrl_apply_display(uint8_t show_debug)
{
    if (show_debug != 0U)
    {
        s_debug_screen_visible = 1U;
        gpio_hold_dis(LCD_PIN_BL);
        gpio_deep_sleep_hold_dis();
        LCD_PanelWake();
        return;
    }

    if (s_debug_screen_visible != 0U)
    {
        LCD_Fill(0, 0, 159, 127, BLACK);
    }
    s_debug_screen_visible = 0U;
    LCD_PanelSleep();
}

static void host_ctrl_debug_render(void)
{
    char line[32];

    if (s_debug_screen_visible == 0U)
    {
        return;
    }

    LCD_Fill(0, 0, 159, 127, BLACK);
    LCD_ShowString(6, 6, 150, 16, 16, (unsigned char *)"ESP32 DEBUG", GREEN, BLACK);

    snprintf(line, sizeof(line), "WiFi   %s", (wifi_service_is_enabled() != 0U) ? "ON" : "OFF");
    LCD_ShowString(6, 28, 150, 16, 16, (unsigned char *)line, WHITE, BLACK);

    snprintf(line, sizeof(line), "Link   %s", (wifi_service_is_connected() != 0U) ? "OK" : "IDLE");
    LCD_ShowString(6, 46, 150, 16, 16, (unsigned char *)line, WHITE, BLACK);

    snprintf(line, sizeof(line), "BLE    %s", (s_ble_enabled != 0U) ? "ON" : "OFF");
    LCD_ShowString(6, 64, 150, 16, 16, (unsigned char *)line, WHITE, BLACK);

    snprintf(line, sizeof(line), "Pwr    %s", host_ctrl_power_policy_text(s_power_policy));
    LCD_ShowString(6, 82, 150, 16, 16, (unsigned char *)line, WHITE, BLACK);

    snprintf(line,
             sizeof(line),
             "Host   %s K%u",
             host_ctrl_state_text(s_host_state),
             (unsigned int)s_last_key);
    LCD_ShowString(6, 100, 150, 16, 16, (unsigned char *)line, YELLOW, BLACK);
}

static esp_err_t host_ctrl_ble_set_enabled(uint8_t enabled)
{
    esp_err_t err = ble_provision_service_set_enabled(enabled);

    if (err == ESP_OK)
    {
        s_ble_enabled = (enabled != 0U) ? 1U : 0U;
    }

    return err;
}

static void host_ctrl_apply_requested_radios(void)
{
    uint8_t wifi_target = s_wifi_requested;
    uint8_t ble_target = (uint8_t)((s_ble_requested != 0U) ||
                                   (wifi_service_needs_provisioning() != 0U) ||
                                   (ble_provision_service_should_force_ble() != 0U));

    if (wifi_service_is_enabled() != wifi_target)
    {
        if (wifi_service_set_enabled(wifi_target) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to apply WiFi request=%u", (unsigned int)wifi_target);
        }
    }

    if (s_ble_enabled != ble_target)
    {
        if (host_ctrl_ble_set_enabled(ble_target) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to apply BLE request=%u", (unsigned int)ble_target);
        }
    }

    if (wifi_service_apply_host_power(s_power_policy, s_host_state) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to apply WiFi power policy");
    }
}

static void host_ctrl_apply_runtime_state(void)
{
    uint8_t should_show_debug = (s_debug_screen_requested != 0U) &&
                                (s_host_state == OTA_HOST_STATE_ACTIVE);

    host_ctrl_apply_display(should_show_debug);
    if (host_ctrl_is_sleep_state(s_host_state) != 0U)
    {
        return;
    }

    host_ctrl_apply_requested_radios();
}

static void host_ctrl_prepare_sleep_runtime(void)
{
    if (wifi_service_is_enabled() != 0U)
    {
        if (wifi_service_set_enabled(0U) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to stop WiFi before sleep");
        }
    }

    if (s_ble_enabled != 0U)
    {
        if (host_ctrl_ble_set_enabled(0U) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to stop BLE before sleep");
        }
    }

    host_ctrl_apply_display(0U);
}

static uint32_t host_ctrl_status_bits(void)
{
    uint32_t bits = 0U;

    if (wifi_service_is_enabled() != 0U)
    {
        bits |= OTA_HOST_STATUS_WIFI_ENABLED;
    }
    if (wifi_service_is_connected() != 0U)
    {
        bits |= OTA_HOST_STATUS_WIFI_CONNECTED;
    }
    if (s_ble_enabled != 0U)
    {
        bits |= OTA_HOST_STATUS_BLE_ENABLED;
    }
    if (s_debug_screen_requested != 0U)
    {
        bits |= OTA_HOST_STATUS_DEBUG_SCREEN_ENABLED;
    }
    if (s_remote_keys_enabled != 0U)
    {
        bits |= OTA_HOST_STATUS_REMOTE_KEYS_ENABLED;
    }
    if (wifi_service_has_credentials() != 0U)
    {
        bits |= OTA_HOST_STATUS_HAS_CREDENTIALS;
    }
    if (host_ctrl_is_sleep_state(s_host_state) != 0U || s_pending_sleep != HOST_CTRL_SLEEP_NONE)
    {
        bits |= OTA_HOST_STATUS_READY_FOR_SLEEP;
    }

    return bits;
}

static uint8_t host_ctrl_consume_pending_key(void)
{
    uint8_t key = s_pending_remote_key;
    s_pending_remote_key = OTA_HOST_REMOTE_KEY_NONE;
    return key;
}

static void host_ctrl_restore_deep_ctx(esp_sleep_wakeup_cause_t wake_cause)
{
    if (s_rtc_ctx.magic != HOST_CTRL_DEEP_CTX_MAGIC)
    {
        return;
    }

    s_remote_keys_enabled = s_rtc_ctx.remote_keys_enabled;
    if (wake_cause == ESP_SLEEP_WAKEUP_EXT0 && s_remote_keys_enabled != 0U)
    {
        s_pending_remote_key = OTA_HOST_REMOTE_KEY_3;
        s_last_key = OTA_HOST_REMOTE_KEY_3;
    }

    s_rtc_ctx.magic = 0U;
    s_rtc_ctx.remote_keys_enabled = 0U;
}

static void host_ctrl_store_deep_ctx(void)
{
    s_rtc_ctx.magic = HOST_CTRL_DEEP_CTX_MAGIC;
    s_rtc_ctx.remote_keys_enabled = s_remote_keys_enabled;
}

static void host_ctrl_note_light_wakeup(void)
{
    uint8_t wake_key = key_peek_pressed();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (wake_key != OTA_HOST_REMOTE_KEY_NONE)
    {
        s_last_key = wake_key;
        if (s_remote_keys_enabled != 0U && s_pending_remote_key == OTA_HOST_REMOTE_KEY_NONE)
        {
            s_pending_remote_key = wake_key;
            host_ctrl_set_awake_hold_ms(HOST_CTRL_LIGHT_WAKE_HOLD_KEY_MS);
        }
        else
        {
            host_ctrl_set_awake_hold_ms(HOST_CTRL_LIGHT_WAKE_HOLD_LOCAL_KEY_MS);
        }
    }
    else
    {
        if (gpio_get_level(USART2_RX_GPIO_PIN) == 0)
        {
            s_uart_wake_block_count = 20U;
        }
        host_ctrl_set_awake_hold_ms(HOST_CTRL_LIGHT_WAKE_HOLD_UART_MS);
    }

    ESP_LOGI(TAG, "Light wake cause=%s key=%u", host_ctrl_wakeup_cause_text(cause), (unsigned int)wake_key);
}

static void host_ctrl_enter_light_sleep(void)
{
    esp_err_t err = ESP_OK;
    uint8_t manual_diag = s_manual_sleep_diag;
    uint8_t enable_uart_wake = 0U;

    s_pending_sleep = HOST_CTRL_SLEEP_NONE;
    s_manual_sleep_diag = 0U;
    host_ctrl_prepare_sleep_runtime();

    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));

    gpio_pullup_en(KEY6_PORT);
    gpio_pulldown_dis(KEY6_PORT);
    ESP_ERROR_CHECK(gpio_wakeup_enable(KEY6_PORT, GPIO_INTR_LOW_LEVEL));
    if (manual_diag == 0U)
    {
        if (s_uart_wake_block_count != 0U)
        {
            s_uart_wake_block_count--;
        }
        else if (gpio_get_level(USART2_RX_GPIO_PIN) != 0)
        {
            enable_uart_wake = 1U;
        }
        else
        {
            s_uart_wake_block_count = 20U;
        }

        if (enable_uart_wake != 0U)
        {
            gpio_pullup_en(USART2_RX_GPIO_PIN);
            gpio_pulldown_dis(USART2_RX_GPIO_PIN);
            ESP_ERROR_CHECK(gpio_wakeup_enable(USART2_RX_GPIO_PIN, GPIO_INTR_LOW_LEVEL));
        }
    }
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());

    if (manual_diag != 0U)
    {
        ESP_LOGI(TAG, "Manual light sleep test: wake by KEY6 only");
    }
    else
    {
        ESP_LOGI(TAG,
                 "Enter light sleep (KEY6%s)",
                 (enable_uart_wake != 0U) ? " + UART2 RX wake" : "");
    }
    err = esp_light_sleep_start();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_light_sleep_start failed: 0x%04X", (unsigned int)err);
    }

    host_ctrl_note_light_wakeup();
}

static void host_ctrl_enter_deep_sleep(void)
{
    esp_err_t err = ESP_OK;
    uint8_t manual_diag = s_manual_sleep_diag;

    s_pending_sleep = HOST_CTRL_SLEEP_NONE;
    s_manual_sleep_diag = 0U;
    host_ctrl_store_deep_ctx();
    host_ctrl_prepare_sleep_runtime();

    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));

    rtc_gpio_pullup_en(KEY6_PORT);
    rtc_gpio_pulldown_dis(KEY6_PORT);

    err = esp_sleep_enable_ext0_wakeup(KEY6_PORT, 0);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "ext0 wake setup failed: 0x%04X", (unsigned int)err);
    }

    err = esp_sleep_enable_timer_wakeup(HOST_CTRL_DEEP_SLEEP_WAKE_MS * 1000ULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "timer wake setup failed: 0x%04X", (unsigned int)err);
    }

    gpio_hold_en(LCD_PIN_BL);
    gpio_deep_sleep_hold_en();
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(20));
    uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(20));

    if (manual_diag != 0U)
    {
        ESP_LOGI(TAG,
                 "Manual deep sleep test: KEY6 wake, timer fallback %llu ms",
                 (unsigned long long)HOST_CTRL_DEEP_SLEEP_WAKE_MS);
    }
    else
    {
        ESP_LOGI(TAG,
                 "Enter deep sleep (KEY6 + %llu ms timer)",
                 (unsigned long long)HOST_CTRL_DEEP_SLEEP_WAKE_MS);
    }
    esp_deep_sleep_start();
}

static void host_ctrl_request_manual_sleep(host_ctrl_sleep_mode_t mode)
{
    s_pending_remote_key = OTA_HOST_REMOTE_KEY_NONE;
    s_pending_sleep = mode;
    s_manual_sleep_diag = 1U;
    host_ctrl_set_awake_hold_ms(0U);
    host_ctrl_apply_display(0U);

    if (mode == HOST_CTRL_SLEEP_DEEP)
    {
        ESP_LOGI(TAG, "Manual sleep test requested by KEY5 -> deep sleep");
    }
    else
    {
        ESP_LOGI(TAG, "Manual sleep test requested by KEY4 -> light sleep");
    }
}

static void host_ctrl_maybe_enter_sleep(void)
{
    if (host_ctrl_is_awake_hold_active() != 0U)
    {
        return;
    }

    if (s_pending_sleep == HOST_CTRL_SLEEP_DEEP ||
        (s_pending_sleep == HOST_CTRL_SLEEP_NONE && s_host_state == OTA_HOST_STATE_STANDBY_PREP))
    {
        host_ctrl_enter_deep_sleep();
        return;
    }

    if (s_pending_sleep == HOST_CTRL_SLEEP_LIGHT ||
        (s_pending_sleep == HOST_CTRL_SLEEP_NONE && s_host_state == OTA_HOST_STATE_STOP_IDLE))
    {
        host_ctrl_enter_light_sleep();
    }
}

static uint8_t host_ctrl_handle_command(uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t arg0 = (payload_len > 1U && payload != NULL) ? payload[1] : 0U;

    switch (cmd)
    {
    case OTA_HOST_CMD_GET_STATUS:
    case OTA_HOST_CMD_PING:
        return OTA_HOST_RESULT_OK;

    case OTA_HOST_CMD_SEND_THERMAL_SNAPSHOT:
        host_ctrl_log_thermal_snapshot(payload, payload_len);
        host_ctrl_forward_thermal_snapshot_to_cloud(payload, payload_len);
        return OTA_HOST_RESULT_OK;

    case OTA_HOST_CMD_SET_WIFI:
        s_wifi_requested = (arg0 != 0U) ? 1U : 0U;
        ESP_LOGI(TAG, "SET_WIFI request=%u", (unsigned int)s_wifi_requested);
        if (wifi_service_set_enabled(s_wifi_requested) != ESP_OK)
        {
            return OTA_HOST_RESULT_FAILED;
        }
        if (wifi_service_apply_host_power(s_power_policy, s_host_state) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to refresh WiFi power after SET_WIFI");
        }
        return OTA_HOST_RESULT_OK;

    case OTA_HOST_CMD_SET_BLE:
        s_ble_requested = (arg0 != 0U) ? 1U : 0U;
        return (host_ctrl_ble_set_enabled(s_ble_requested) == ESP_OK) ? OTA_HOST_RESULT_OK : OTA_HOST_RESULT_FAILED;

    case OTA_HOST_CMD_SET_DEBUG_SCREEN:
        s_debug_screen_requested = (arg0 != 0U) ? 1U : 0U;
        host_ctrl_apply_runtime_state();
        return OTA_HOST_RESULT_OK;

    case OTA_HOST_CMD_SET_REMOTE_KEYS:
        s_remote_keys_enabled = (arg0 != 0U) ? 1U : 0U;
        return OTA_HOST_RESULT_OK;

    case OTA_HOST_CMD_SET_POWER_POLICY:
        if (arg0 > OTA_HOST_POWER_POLICY_ECO)
        {
            return OTA_HOST_RESULT_UNSUPPORTED;
        }
        s_power_policy = arg0;
        host_ctrl_apply_runtime_state();
        return OTA_HOST_RESULT_OK;

    case OTA_HOST_CMD_SET_HOST_STATE:
        if (arg0 > OTA_HOST_STATE_STANDBY_PREP)
        {
            return OTA_HOST_RESULT_UNSUPPORTED;
        }

        s_host_state = arg0;
        if (s_host_state == OTA_HOST_STATE_STOP_IDLE)
        {
            s_pending_sleep = HOST_CTRL_SLEEP_LIGHT;
            s_pending_runtime_apply = 0U;
            s_pending_remote_key = OTA_HOST_REMOTE_KEY_NONE;
            host_ctrl_apply_display(0U);
        }
        else if (s_host_state == OTA_HOST_STATE_STANDBY_PREP)
        {
            s_pending_sleep = HOST_CTRL_SLEEP_DEEP;
            s_pending_runtime_apply = 0U;
            s_pending_remote_key = OTA_HOST_REMOTE_KEY_NONE;
            host_ctrl_apply_display(0U);
        }
        else
        {
            s_pending_sleep = HOST_CTRL_SLEEP_NONE;
            host_ctrl_set_awake_hold_ms(0U);
            s_pending_runtime_apply = 1U;
        }
        return OTA_HOST_RESULT_OK;

    default:
        return OTA_HOST_RESULT_UNSUPPORTED;
    }
}

void host_ctrl_service_init(void)
{
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();

    s_wifi_requested = 0U;
    s_ble_requested = 0U;
    s_ble_enabled = 0U;
    s_debug_screen_requested = 0U;
    s_debug_screen_visible = 0U;
    s_remote_keys_enabled = 0U;
    s_power_policy = OTA_HOST_POWER_POLICY_BALANCED;
    s_host_state = OTA_HOST_STATE_ACTIVE;
    s_pending_remote_key = OTA_HOST_REMOTE_KEY_NONE;
    s_last_key = OTA_HOST_REMOTE_KEY_NONE;
    s_last_logged_status_bits = 0xFFFFFFFFUL;
    s_last_logged_result = 0xFFU;
    s_pending_sleep = HOST_CTRL_SLEEP_NONE;
    s_awake_hold_deadline = 0;
    s_pending_runtime_apply = 1U;
    s_manual_sleep_diag = 0U;
    s_uart_wake_block_count = 0U;

    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(LCD_PIN_BL);
    LCD_PanelSleep();
    host_ctrl_restore_deep_ctx(wake_cause);

    ESP_LOGI(TAG, "Wake cause=%s", host_ctrl_wakeup_cause_text(wake_cause));
}

void host_ctrl_service_request_wifi(uint8_t enabled)
{
    s_wifi_requested = (enabled != 0U) ? 1U : 0U;
    s_pending_runtime_apply = 1U;
}

void host_ctrl_service_request_runtime_apply(void)
{
    s_pending_runtime_apply = 1U;
}

void host_ctrl_service_step(void)
{
    uint8_t key = OTA_HOST_REMOTE_KEY_NONE;

    if (s_pending_runtime_apply != 0U && host_ctrl_is_sleep_state(s_host_state) == 0U)
    {
        s_pending_runtime_apply = 0U;
        host_ctrl_apply_runtime_state();
    }

    host_ctrl_maybe_enter_sleep();

    key = key_scan();
    if (key == OTA_HOST_REMOTE_KEY_NONE)
    {
        return;
    }

    if (s_remote_keys_enabled == 0U &&
        s_host_state == OTA_HOST_STATE_ACTIVE &&
        s_pending_sleep == HOST_CTRL_SLEEP_NONE)
    {
        if (key == OTA_HOST_REMOTE_KEY_1)
        {
            host_ctrl_request_manual_sleep(HOST_CTRL_SLEEP_LIGHT);
            return;
        }
        if (key == OTA_HOST_REMOTE_KEY_2)
        {
            host_ctrl_request_manual_sleep(HOST_CTRL_SLEEP_DEEP);
            return;
        }
    }

    s_last_key = key;
    if (s_remote_keys_enabled != 0U && s_pending_remote_key == OTA_HOST_REMOTE_KEY_NONE)
    {
        s_pending_remote_key = key;
    }

    if (s_debug_screen_visible != 0U)
    {
        host_ctrl_debug_render();
    }
}

bool host_ctrl_service_handle_frame(const ota_ctrl_frame_t *frame)
{
    uint8_t rsp[OTA_CTRL_HOST_PAYLOAD_LEN] = {0};
    uint8_t cmd = 0U;
    uint8_t result = OTA_HOST_RESULT_FAILED;
    uint32_t status_bits = 0U;

    if (frame == NULL || frame->msg_type != OTA_CTRL_MSG_HOST_REQ || frame->payload_len < OTA_CTRL_HOST_PAYLOAD_LEN)
    {
        return false;
    }

    cmd = frame->payload[0];
    result = host_ctrl_handle_command(cmd, frame->payload, frame->payload_len);
    status_bits = host_ctrl_status_bits();

    rsp[0] = cmd;
    rsp[1] = frame->payload[1];
    rsp[2] = host_ctrl_consume_pending_key();
    rsp[3] = result;
    ota_ctrl_write_u32le(&rsp[4], status_bits);

    if (s_debug_screen_visible != 0U)
    {
        host_ctrl_debug_render();
    }

    if (cmd != OTA_HOST_CMD_GET_STATUS ||
        result != s_last_logged_result ||
        status_bits != s_last_logged_status_bits)
    {
        ESP_LOGI(TAG, "HOST cmd=0x%02X result=%u status=0x%08" PRIX32, cmd, result, status_bits);
        s_last_logged_result = result;
        s_last_logged_status_bits = status_bits;
    }

    return ota_ctrl_send_frame(OTA_CTRL_MSG_HOST_RSP, frame->seq, rsp, OTA_CTRL_HOST_PAYLOAD_LEN);
}
