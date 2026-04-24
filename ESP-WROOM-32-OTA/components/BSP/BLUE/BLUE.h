/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#define BT
#ifdef BT

#ifndef BLUE_H_
#define BLUE_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
extern esp_err_t ret;
extern EventGroupHandle_t xCreatedEventGroup_BlueConnect;
/* Attributes State Machine */
enum
{
    IDX_SVC,

    IDX_CHAR_A,
    IDX_CHAR_VAL_A,
    IDX_CHAR_CFG_A,

    IDX_CHAR_LED,
    IDX_CHAR_VAL_LED,

    IDX_CHAR_WEATHER,
    IDX_CHAR_VAL_WEATHER,

    IDX_CHAR_TEMP,
    IDX_CHAR_VAL_TEMP,
    IDX_CHAR_CFG_TEMP,


    HRS_IDX_NB,
};

#define Blue_CONNECTED_BIT      BIT0
#define Blue_FAIL_BIT           BIT1

#define MAX_CITY_NAME_LEN 30
extern char received_city[MAX_CITY_NAME_LEN + 1];
extern EventGroupHandle_t xCreatedEventGroup_BlueConnect;
void BLUE_Init(void);
#endif
#endif