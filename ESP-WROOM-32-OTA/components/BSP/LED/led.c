#include "LED.h"


void led_init(void) {
    gpio_config_t gpio_init_struct = {0};
    gpio_init_struct.intr_type = GPIO_INTR_DISABLE; // 禁用中断
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT; // 设置为输出模式
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE; // 启用上拉电阻
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE; // 禁用下拉电阻
    gpio_init_struct.pin_bit_mask = 1ULL << LED_GPIO_PIN; // 设置要配置的引脚
    gpio_config(&gpio_init_struct); // 配置GPIO
    LED(1); // 初始化LED为低电平

}