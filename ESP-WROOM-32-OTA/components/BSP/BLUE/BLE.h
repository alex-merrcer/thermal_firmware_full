//#define BLE

#ifdef BLE
#ifndef BLE_H
#define BLE_H

#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"
/* Includes */
/* STD APIs */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ESP APIs */
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/* FreeRTOS APIs */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* NimBLE stack APIs */
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "HTTP.h"

#define TAG "NimBLE_GATT_Server"
#define DEVICE_NAME_BLE "NimBLE_GATT"

#define Blue_CONNECTED_BIT      BIT0
#define Blue_FAIL_BIT           BIT1

#define MAX_CITY_NAME_LEN 30
#define RESULT_MSG_LEN 20

/* Defines */
#define BLE_GAP_APPEARANCE_GENERIC_TAG 0x0200
#define BLE_GAP_URI_PREFIX_HTTPS 0x17
#define BLE_GAP_LE_ROLE_PERIPHERAL 0x00

extern esp_err_t ret;
extern EventGroupHandle_t xCreatedEventGroup_BlueConnect;

extern char city_name[MAX_CITY_NAME_LEN + 1];
/* Public function declarations */
void adv_init(void);
int gap_init(void);
void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);
void gatt_svr_subscribe_cb(struct ble_gap_event *event);
int gatt_svc_init(void);

/* Library function declarations */
void ble_store_config_init(void);

/* Private function declarations */
void on_stack_reset(int reason);
void on_stack_sync(void);
void nimble_host_config_init(void);
void BLE_Init(void);

#endif

#endif