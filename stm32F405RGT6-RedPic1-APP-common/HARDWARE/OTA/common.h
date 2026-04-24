#ifndef __COMMON_H
#define __COMMON_H

#include "stm32f4xx.h"

uint32_t SerialKeyPressed(uint8_t *key);
void SerialPutChar(uint8_t c);

#endif
