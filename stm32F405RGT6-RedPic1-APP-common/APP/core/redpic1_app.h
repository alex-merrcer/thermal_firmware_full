#ifndef REDPIC1_APP_H
#define REDPIC1_APP_H

#include <stdint.h>

#include "app_service_bus.h"
#include "settings_service.h"

/*
 * redpic1_app.h
 * RTOS application runtime public header.
 */

typedef struct
{
    uint8_t key_value;
    uint32_t tick_ms;
} app_key_event_t;

void app_rtos_runtime_init(void);
void app_rtos_runtime_start(void);

void app_rtos_lcd_lock(void);
void app_rtos_lcd_unlock(void);

void app_rtos_settings_lock(void);
void app_rtos_settings_unlock(void);
void app_rtos_settings_copy(device_settings_t *out_settings);
uint8_t app_rtos_settings_update(const device_settings_t *settings);

void redpic1_app_main(void);

#endif
