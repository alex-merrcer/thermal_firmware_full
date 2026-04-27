#ifndef __IAP_H
#define __IAP_H

#include "stm32f4xx.h"
#include "../../../protocol/boot_info_v3.h"

/* Dual-slot application layout for STM32F405RG (1MB flash).
 * Keep bootloader/boot-info sectors unchanged and expand APP slots. */
#define FLASH_APP1_ADDR        0x08010000U
#define FLASH_APP2_ADDR        0x08080000U
#define FLASH_APP_MAX_SIZE     0x70000U

/* Boot journal base address. */
#define BOOT_INFO_ADDR         0x0800C000U

#define BOOT_INFO_LAYOUT_MAGIC    BOOT_INFO_LAYOUT_MAGIC_V3
#define BOOT_INFO_LAYOUT_VERSION  BOOT_INFO_LAYOUT_VERSION_V3

/* Boot request flags. */
#define MAGIC_NORMAL           BOOT_MAGIC_NORMAL
#define MAGIC_REQUEST          BOOT_MAGIC_REQUEST
#define MAGIC_NEW_FW           BOOT_MAGIC_NEW_FW

/* Upgrade action flags. */
#define BOOT_UPGRADE_FLAG_NONE      0U
#define BOOT_UPGRADE_FLAG_UPGRADE   1U
#define BOOT_UPGRADE_FLAG_ROLLBACK  2U

#define BOOT_INFO_PARTITION_APP1    BOOT_INFO_SLOT_APP1
#define BOOT_INFO_PARTITION_APP2    BOOT_INFO_SLOT_APP2

/* Compatibility aliases while legacy v2 field names are migrated. */
#define active_partition            active_slot
#define target_partition            target_slot
#define app1_version                slot_versions[BOOT_INFO_SLOT_APP1]
#define app2_version                slot_versions[BOOT_INFO_SLOT_APP2]

/* Reset reason hints. */
#define BOOT_REASON_NORMAL      0
#define BOOT_REASON_IWDG        1
#define BOOT_REASON_POWER       2
#define BOOT_REASON_SOFTWARE    3

/* Active application entry address. */
extern uint32_t APPLICATION_ADDRESS;

/* Bootloader public entry points. */
void iap_boot_entry(void);
void iap_main(void);
void jump_to_app(uint32_t app_addr);

#endif
