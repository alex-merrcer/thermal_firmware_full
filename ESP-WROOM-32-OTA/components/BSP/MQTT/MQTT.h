#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mqtt_client.h"

/*
 * Fill the three fields below after you create the device in Alibaba Cloud IoT.
 * The default region is cn-shanghai.
 * For a new public instance or enterprise instance, also fill ALIYUN_IOT_MQTT_HOST.
 */
#define ALIYUN_IOT_PRODUCT_KEY           "k1jy7ZTTQ8V"
#define ALIYUN_IOT_DEVICE_NAME           "ESP32-WROOM-32"
#define ALIYUN_IOT_DEVICE_SECRET         "6d699c5733d561787e8d08acbc807e4c"

#define ALIYUN_IOT_REGION_ID             "cn-shanghai"
#define ALIYUN_IOT_MQTT_HOST             "iot-06z00fpcn2h806i.mqtt.iothub.aliyuncs.com"

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
#define ALIYUN_THERMAL_PROP_MIN_TEMP     "MinTemp"
#define ALIYUN_THERMAL_PROP_MAX_TEMP     "MaxTemp"
#define ALIYUN_THERMAL_PROP_CENTER_TEMP  "CenterTemp"

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

void mqtt_app_init(void);
void mqtt_app_init_1(void);
void publish_sensor_data(uint16_t ir, uint16_t als, uint16_t ps);

#endif
