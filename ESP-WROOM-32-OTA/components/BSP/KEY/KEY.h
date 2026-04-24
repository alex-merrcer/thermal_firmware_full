#ifndef KEY_H
#define KEY_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#define KEY4_PORT  GPIO_NUM_22
#define KEY5_PORT  GPIO_NUM_21
#define KEY6_PORT  GPIO_NUM_27

#define FunKeyDown 0
#define powKeyDowm 1

#define getPin(gpio_num) gpio_get_level(gpio_num)
#define setPin(gpio_num, level) gpio_set_level(gpio_num, level)

uint8_t key_scan(void);
uint8_t key_peek_pressed(void);
void key_init(void);
#endif
