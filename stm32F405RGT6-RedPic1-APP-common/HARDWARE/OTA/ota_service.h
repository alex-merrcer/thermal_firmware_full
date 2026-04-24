#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <stdint.h>

#include "boot_info_v3.h"

void ota_service_init(void);
void ota_service_poll(void);
void ota_service_refresh_info(void);
void ota_service_reconfigure_timebase(void);
void ota_service_load_boot_info_copy(BootInfoTypeDef *boot_info);
uint32_t ota_service_store_boot_info(const BootInfoTypeDef *boot_info);

const BootInfoTypeDef *ota_service_get_boot_info(void);
const char *ota_service_get_display_version(void);
const char *ota_service_get_partition_version(uint32_t partition);
const char *ota_service_get_partition_name(uint32_t partition);
const char *ota_service_reason_text(uint16_t reason);

uint32_t ota_service_get_active_partition(void);
uint32_t ota_service_get_inactive_partition(void);

int8_t ota_service_compare_version(const char *left, const char *right);
uint8_t ota_service_query_latest_version(char *latest_version,
                                         uint16_t latest_version_len,
                                         uint16_t *reject_reason);

void ota_service_request_upgrade(void);
void ota_service_request_rollback(void);

#endif
