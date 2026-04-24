#include "UART.h"

void UART0_Init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_APB,
    };

    uart_param_config(UART_NUM_0, &uart_config);
    uart_set_pin(UART_NUM_0,
                 USART_TX_GPIO_PIN,
                 USART_RX_GPIO_PIN,
                 UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, RX_BUF_SIZE * 2, 20, NULL, 0);
}

void UART2_Init(void)
{
    uart_config_t uart_config = {
        .baud_rate = STM32_LINK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_APB,
    };

    uart_param_config(UART_NUM_2, &uart_config);
    uart_set_pin(UART_NUM_2,
                 USART2_TX_GPIO_PIN,
                 USART2_RX_GPIO_PIN,
                 UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_2, RX_BUF_SIZE * 2, RX_BUF_SIZE * 2, 20, NULL, 0);
}

typedef enum
{
    STATE_WAIT_HEADER1,
    STATE_WAIT_HEADER2,
    STATE_RECEIVE_FLOAT,
    STATE_WAIT_TAIL
} uart_state_t;

#pragma pack(push, 1)
typedef struct
{
    uint8_t header[2];
    float data;
    uint8_t tail;
} stm32_frame_t;
#pragma pack(pop)

static const char *TAG = "UART_RX";
static uart_state_t uart_state = STATE_WAIT_HEADER1;
static uint8_t float_bytes[4];
static uint8_t byte_index = 0;
static TickType_t last_rx_time;

void process_rx_byte(uint8_t byte)
{
    switch (uart_state)
    {
    case STATE_WAIT_HEADER1:
        if (byte == 0x0A)
        {
            uart_state = STATE_WAIT_HEADER2;
            last_rx_time = xTaskGetTickCount();
        }
        break;

    case STATE_WAIT_HEADER2:
        if (byte == 0x0B)
        {
            uart_state = STATE_RECEIVE_FLOAT;
            byte_index = 0;
        }
        else
        {
            uart_state = STATE_WAIT_HEADER1;
            ESP_LOGI(TAG, "Invalid frame header: 0x%02X", byte);
        }
        break;

    case STATE_RECEIVE_FLOAT:
        float_bytes[byte_index++] = byte;
        if (byte_index >= 4)
        {
            uart_state = STATE_WAIT_TAIL;
        }
        last_rx_time = xTaskGetTickCount();
        break;

    case STATE_WAIT_TAIL:
        if (byte == 0x0C)
        {
            union
            {
                float float_val;
                uint8_t bytes[4];
            } converter;
            int i = 0;

            for (i = 0; i < 4; i++)
            {
                converter.bytes[i] = float_bytes[i];
            }

            ESP_LOGI(TAG, "Received float: %.4f", converter.float_val);
        }
        else
        {
            ESP_LOGW(TAG, "Invalid frame tail: 0x%02X", byte);
        }
        uart_state = STATE_WAIT_HEADER1;
        break;
    }
}

void check_timeout(void)
{
    const TickType_t timeout_ticks = pdMS_TO_TICKS(50);

    if (uart_state != STATE_WAIT_HEADER1 &&
        (xTaskGetTickCount() - last_rx_time) > timeout_ticks)
    {
        ESP_LOGW(TAG, "Frame timeout, reset state machine");
        uart_state = STATE_WAIT_HEADER1;
    }
}
