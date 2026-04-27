#ifndef __IAP_H
#define __IAP_H

#include "flash_if.h"

uint8_t iap_query_latest_version(const BootInfoTypeDef *boot_info,
                                 char *latest_version,
                                 uint16_t latest_version_len,
                                 uint16_t *reject_reason);

#endif
