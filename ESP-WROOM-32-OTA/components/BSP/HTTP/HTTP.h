#ifndef _HTTP_H
#define _HTTP_H

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "lwip/dns.h"
#include <netdb.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "MQTT.h"
#include "app_config.h"
#include "esp_crt_bundle.h"  // 添加证书包支持

#define WEB_SERVER "api.seniverse.com"
#define WEB_PORT "80"
#define MAX_REQUEST_LEN 2048  // 根据实际需求调整长度
//#define WEB_URL "/v3/weather/now.json?key=your_weather_api_key&location=maoming&language=zh-Hans&unit=c"

extern const char *TAG;
extern char *REQUEST;
extern char dynamic_url[2048];
extern char request_buffer[MAX_REQUEST_LEN];
extern char *http_response_buffer;
extern size_t http_response_len;

extern EventGroupHandle_t xCreatedEventGroup_WifiConnect;

esp_err_t http_event_handler(esp_http_client_event_t *evt);
void TaskHttpCreate(void);
void weather_get(int s, char *recv_buf);
void weather_parse(const char *response);
esp_err_t http_event_handler(esp_http_client_event_t *evt);

void call_deepseek_api(void)  ;

#endif
