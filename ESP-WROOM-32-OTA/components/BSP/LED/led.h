#ifndef LED_H
#define LED_H

#include "driver/gpio.h"
#include "driver/ledc.h"

#define LED_GPIO_PIN    GPIO_NUM_2  /* LED连接的GPIO端口 */


#define LED(x)  do { x ? gpio_set_level(LED_GPIO_PIN, 1) : gpio_set_level(LED_GPIO_PIN, 0); } while(0)

#define LED_TOGGLE()    do { gpio_set_level(LED_GPIO_PIN, !gpio_get_level(LED_GPIO_PIN)); } while(0)  /* LED翻转 */

void led_init(void); /* 初始化 LED */
void pwm_set_duty(uint32_t duty);
void pwm_init(uint8_t resolution, uint16_t freq);
#endif