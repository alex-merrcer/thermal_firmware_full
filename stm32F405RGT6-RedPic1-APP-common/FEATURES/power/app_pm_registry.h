#ifndef APP_PM_REGISTRY_H
#define APP_PM_REGISTRY_H

#include <stdint.h>

typedef struct
{
    const char *name;
    uint8_t (*can_sleep)(void);
    void (*prepare_stop)(void);
    void (*restore_stop)(void);
    void (*prepare_standby)(void);
} app_pm_client_t;

uint8_t app_pm_register_client(const app_pm_client_t *client);
uint8_t app_pm_can_enter_stop(void);
void app_pm_prepare_stop(void);
void app_pm_restore_stop(void);
void app_pm_prepare_standby(void);

#endif
