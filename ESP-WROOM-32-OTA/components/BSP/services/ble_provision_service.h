#ifndef BLE_PROVISION_SERVICE_H
#define BLE_PROVISION_SERVICE_H

#include <stdint.h>

#include "esp_err.h"

void ble_provision_service_init(void);
void ble_provision_service_start(void);
esp_err_t ble_provision_service_set_enabled(uint8_t enabled);
uint8_t ble_provision_service_should_force_ble(void);
uint8_t ble_provision_service_is_connected(void);
void ble_provision_service_on_wifi_connected(void);
void ble_provision_service_on_wifi_disconnected(uint16_t reason);

#endif
