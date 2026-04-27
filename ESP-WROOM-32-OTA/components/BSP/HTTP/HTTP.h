#ifndef _HTTP_H
#define _HTTP_H

#include <stddef.h>

#include "app_config.h"
#include "esp_err.h"
#include "esp_http_client.h"

#define WEB_SERVER "api.seniverse.com"
#define WEB_PORT "80"
#define MAX_REQUEST_LEN 2048

extern const char *TAG;
extern char dynamic_url[2048];
extern char *http_response_buffer;
extern size_t http_response_len;

esp_err_t http_event_handler(esp_http_client_event_t *evt);
void weather_parse(const char *response);

#endif
