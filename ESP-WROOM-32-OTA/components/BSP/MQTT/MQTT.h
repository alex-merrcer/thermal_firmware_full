#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "mqtt_client.h"

#define ALIYUN_MQTT_ENABLE               1U
#define ALIYUN_MQTT_USE_TLS              1U
#define ALIYUN_MQTT_PORT_TCP             1883U
/* The bundled root.crt is the GlobalSign root CA, so the default TLS port is 1883. */
#define ALIYUN_MQTT_PORT_TLS             1883U
#define ALIYUN_MQTT_KEEPALIVE_SEC        300U
#define ALIYUN_MQTT_QOS                  1

/*
 * These identifiers must match the TSL property identifiers you create
 * on Alibaba Cloud IoT Platform.
 */
#define ALIYUN_THERMAL_PROP_MIN_TEMP     APP_THERMAL_PROP_MIN_TEMP
#define ALIYUN_THERMAL_PROP_MAX_TEMP     APP_THERMAL_PROP_MAX_TEMP
#define ALIYUN_THERMAL_PROP_CENTER_TEMP  APP_THERMAL_PROP_CENTER_TEMP
#define ALIYUN_OTA_PROP_STATE            APP_OTA_PROP_STATE

#define MQTT_TAG "ALIYUN_MQTT"

extern esp_mqtt_client_handle_t client;
extern bool mqtt_connected;
extern double temp;
extern double humi;
extern double gas;
extern int g_publish_flag;
extern bool MQTT_OTA;

esp_err_t mqtt_service_init(void);
void mqtt_service_step(void);
esp_err_t mqtt_service_submit_thermal_snapshot_x10(int16_t min_temp_x10,
                                                   int16_t max_temp_x10,
                                                   int16_t center_temp_x10);
esp_err_t mqtt_service_submit_ota_status(uint8_t stage,
                                         uint8_t percent,
                                         uint16_t detail_code,
                                         uint32_t current_value,
                                         uint32_t total_value);

void mqtt_app_init(void);
void mqtt_app_init_1(void);
void publish_sensor_data(uint16_t ir, uint16_t als, uint16_t ps);

#endif
