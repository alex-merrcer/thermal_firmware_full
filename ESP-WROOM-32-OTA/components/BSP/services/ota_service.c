#include "ota_service.h"

#include "OTA_STM32.h"

void ota_service_init(void)
{
    OTA_STM32_Init();
}

void ota_service_start(void)
{
    OTA_STM32_Start();
}
