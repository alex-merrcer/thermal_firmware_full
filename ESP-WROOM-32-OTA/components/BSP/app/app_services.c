#include "app_services.h"

#include "app_service_bus.h"
#include "ble_provision_service.h"
#include "cloud_mqtt_service.h"
#include "diagnostics_service.h"
#include "esp_err.h"
#include "esp_log.h"
#include "host_service.h"
#include "host_uart_rx.h"
#include "ota_service.h"
#include "power_service.h"
#include "weather_service.h"

static const char *TAG = "APP_SERVICES";

void app_services_init(void)
{
    ESP_ERROR_CHECK(app_service_bus_init());
    ESP_ERROR_CHECK(cloud_mqtt_service_init());
    ESP_ERROR_CHECK(host_service_init());
    ota_service_init();
    ble_provision_service_init();
    weather_service_init();
    power_service_init();
    diagnostics_service_init();
    ESP_LOGI(TAG, "Services init complete");
}

void app_services_start(void)
{
    cloud_mqtt_service_start();
    host_service_start();
    ota_service_start();
    ble_provision_service_start();
    weather_service_start();
    power_service_start();
    diagnostics_service_start();
    host_uart_rx_start();
    ESP_LOGI(TAG, "Services start complete");
}
