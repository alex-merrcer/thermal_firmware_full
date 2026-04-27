#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include <stdint.h>

typedef enum
{
    WEATHER_SERVICE_MODE_CLOUD_ONLY = 0,
    WEATHER_SERVICE_MODE_DEVICE_FETCH = 1
} weather_service_mode_t;

void weather_service_init(void);
void weather_service_start(void);
weather_service_mode_t weather_service_get_mode(void);
uint8_t weather_service_is_cloud_only(void);

#endif
