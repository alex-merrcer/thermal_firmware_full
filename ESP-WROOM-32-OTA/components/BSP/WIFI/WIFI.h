#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <inttypes.h>
#include <netdb.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include <nvs.h>
#include <nvs_flash.h>

#define TCP_CLIENT_PORT 5000
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define WIFI_SMARTCONFIG_DONE   BIT3

extern int server_socket;
extern int server_connect_socket;
extern EventGroupHandle_t xCreatedEventGroup_WifiConnect;
extern EventGroupHandle_t Smart_config_WifiConnect;

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void wifi_init_mode(void);
void WIFI_init_smartconfig(void);
void wifi_init_sta(void);
void wifi_init_softap(void);
esp_err_t CreateTcpServer(bool isCreatServer, uint16_t port);

esp_err_t wifi_service_set_enabled(uint8_t enabled);
esp_err_t wifi_service_apply_host_power(uint8_t power_policy, uint8_t host_state);
uint8_t wifi_service_is_enabled(void);
uint8_t wifi_service_is_connected(void);
uint8_t wifi_service_has_credentials(void);

#endif
