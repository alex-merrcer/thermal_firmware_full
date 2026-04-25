#include "led.h"

static uint8_t s_led_inited = 0U;

void led_init(void)
{
    if (s_led_inited != 0U)
    {
        return;
    }

    gpio_reset_pin(LED_GPIO_PIN);
    gpio_set_direction(LED_GPIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO_PIN, 1);
    s_led_inited = 1U;
}

void LED(uint8_t level)
{
    if (s_led_inited == 0U)
    {
        led_init();
    }

    gpio_set_level(LED_GPIO_PIN, level ? 1 : 0);
}
