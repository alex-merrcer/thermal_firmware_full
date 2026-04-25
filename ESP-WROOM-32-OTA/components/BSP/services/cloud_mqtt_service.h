#ifndef CLOUD_MQTT_SERVICE_H
#define CLOUD_MQTT_SERVICE_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t cloud_mqtt_service_init(void);
void cloud_mqtt_service_start(void);
esp_err_t cloud_mqtt_service_submit_thermal_snapshot_x10(int16_t min_temp_x10,
                                                         int16_t max_temp_x10,
                                                         int16_t center_temp_x10);

#endif
