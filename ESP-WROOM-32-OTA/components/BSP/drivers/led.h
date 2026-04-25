#ifndef LED_H
#define LED_H

#include <stdint.h>

#include "driver/gpio.h"

#ifndef LED_GPIO_PIN
#define LED_GPIO_PIN GPIO_NUM_2
#endif

void led_init(void);
void LED(uint8_t level);

#endif
