#ifndef __EXTI_KEY_H__
#define __EXTI_KEY_H__

#include "sys.h"
#include "key.h"

void KEY_EXTI_Init(void);
void KEY_EXTI_ReconfigureDebounceTimer(void);
uint8_t KEY_EXTI_IsHealthy(void);
uint8_t KEY_GetValue(void);
void KEY_PushEvent(uint8_t key_value);
uint8_t KEY_IsLogicalPressed(uint8_t key_value);
void KEY_EXTI_OnEventQueuedFromISR(void);

#endif
