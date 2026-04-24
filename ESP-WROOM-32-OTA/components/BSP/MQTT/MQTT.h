#ifndef MQTT_H
#define MQTT_H

#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_err.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "esp_event.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "WIFI.h"
#include "cJSON.h"
#include "mqtt_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "led.h"
#include <esp_heap_caps.h>

/* 用户需要根据设备信息完善以下宏定义中的三元组内容 */
#define PRODUCT_KEY "k1jy7ZTTQ8V"                        /* ProductKey->阿里云颁发的产品唯一标识，11位长度的英文数字随机组合 */
#define DEVICE_NAME "ESP32-WROOM-32"                         /* DeviceName->用户注册设备时生成的设备唯一编号，支持系统自动生成，也可支持用户添加自定义编号，产品维度内唯一  */
#define DEVICE_SECRET "6d699c5733d561787e8d08acbc807e4c" /* DeviceSecret->设备密钥，与DeviceName成对出现，可用于一机一密的认证方案  */
/* MQTT地址与端口 */
#define HOST_NAME "k1jy7ZTTQ8V.iot-as-mqtt.cn-shanghai.aliyuncs.com" /* 阿里云域名 */
#define HOST_PORT 1883                                               /* 阿里云域名端口，固定1883 */
/* 根据三元组内容计算得出的数值 */
#define CLIENT_ID "k1jy7ZTTQ8V.ESP32-WROOM-32|securemode=2,signmethod=hmacsha256,timestamp=1775913989662|" /* 客户端ID */
#define USER_NAME "ESP32-WROOM-32&k1jy7ZTTQ8V"                                                             /* 客户端用户名 */
#define PASSWORD "eb0e3294218b9f14f1c910c29a706ca82aaf305c846d56707a0ab55ea9a42714"                    /* 由MQTT_Password工具计算得出的连接密码 */
/* 发布与订阅 */
#define DEVICE_PUBLISH "/" PRODUCT_KEY "/" DEVICE_NAME "/user/update" /* 发布 */
#define DEVICE_SUBSCRIBE "/" PRODUCT_KEY "/" DEVICE_NAME "/user/get"  /* 订阅 */
/* 阿里云物模型 */

//#define TOPIC_PROP_POST "/sys/" PRODUCT_KEY "/" DEVICE_NAME "/thing/event/property/post"
#define TOPIC_PROP_POST "/k1jy7ZTTQ8V/ESP32-WROOM-32/user/update"
#define TOPIC_PROP_SET "/k1jy7ZTTQ8V/ESP32-WROOM-32/user/get"
//#define TOPIC_PROP_SET "/sys/" PRODUCT_KEY "/" DEVICE_NAME "/thing/service/property/set"

#define TOPIC_POST_REPLY "/sys/" PRODUCT_KEY "/" DEVICE_NAME "/thing/event/property/post_reply"
#define TOPIC_SET_REPLY "/sys/" PRODUCT_KEY "/" DEVICE_NAME "/thing/service/property/set_reply"
  

#define MQTT_TAG "MQTT-TAG"
extern esp_mqtt_client_handle_t client;
extern bool mqtt_connected;      /* MQTT连接状态标志 */
extern double temp; // 温度值
extern double humi; // 湿度值
extern double gas;  // 湿度值
//extern const char *MQTT_TAG;
extern int g_publish_flag; /* 发布成功标志位 */
extern bool MQTT_OTA;
void mqtt_app_init(void);
void publish_sensor_data(uint16_t ir, uint16_t als, uint16_t ps);
void mqtt_app_init_1(void);
#endif