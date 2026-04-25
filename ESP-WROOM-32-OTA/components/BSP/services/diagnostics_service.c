#include "diagnostics_service.h"

#include "app_config.h"
#include "esp_log.h"

static const char *TAG = "DIAG_SVC";

void diagnostics_service_init(void)
{
    ESP_LOGI(TAG, "Diagnostics service registered");
}

void diagnostics_service_start(void)
{
    if (APP_FEATURE_ENABLE_DIAGNOSTICS_SERVICE_STARTUP == 0U)
    {
        ESP_LOGI(TAG, "Diagnostics service is disabled by default in phase 1");
    }
}
