#include "KEY.h"

#include "esp_log.h"

static const char *TAG = "KEY";

static uint8_t key_detect_pressed_now(void)
{
    if (gpio_get_level(KEY4_PORT) == 0)
    {
        return 1U;
    }
    if (gpio_get_level(KEY5_PORT) == 0)
    {
        return 2U;
    }
    if (gpio_get_level(KEY6_PORT) == 0)
    {
        return 3U;
    }

    return 0U;
}

void key_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KEY4_PORT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };

    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << KEY5_PORT);
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << KEY6_PORT);
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Keys ready on IO22/IO21/IO27");
}

uint8_t key_scan(void)
{
    static uint8_t key_flag = 0U;
    uint8_t key_value = 0U;

    if (key_detect_pressed_now() != 0U && key_flag == 0U)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
        key_value = key_detect_pressed_now();
        if (key_value != 0U)
        {
            key_flag = 1U;
            ESP_LOGI(TAG, "KEY%u pressed", (unsigned int)(key_value + 3U));
        }
    }

    if (key_detect_pressed_now() == 0U)
    {
        key_flag = 0U;
    }

    return key_value;
}

uint8_t key_peek_pressed(void)
{
    return key_detect_pressed_now();
}
