#include "OTA_STM32.h"

#include "ota_service.h"
#include "ota_stm32_internal.h"

#define TAG OTA_STM32_TAG

void OTA_STM32_Init(void)
{
    ESP_LOGI(TAG, "OTA STM32 init");
    ota_log_aes_security_profile();
    ESP_LOGI(TAG, "OTA metadata=%s/%s, product=%s",
             OTA_PACKAGE_BASE_URL,
             OTA_LATEST_JSON_NAME,
             OTA_SUPPORTED_PRODUCT_ID);
}

void OTA_STM32_Start(void)
{
    /* Compatibility wrapper: phase 4 moves task ownership into ota_service. */
    ota_service_start();
}
