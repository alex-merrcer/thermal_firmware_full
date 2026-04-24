#ifndef __FLASH_IF_H
#define __FLASH_IF_H

#include "stm32f4xx.h"
#include "app_slot_config.h"
#include "boot_info_v3.h"

#define BOOT_INFO_ADDR            ((uint32_t)0x0800C000U)
#define BOOT_INFO_LAYOUT_MAGIC    BOOT_INFO_LAYOUT_MAGIC_V3
#define BOOT_INFO_LAYOUT_VERSION  BOOT_INFO_LAYOUT_VERSION_V3

#define BOOT_INFO_PARTITION_APP1  BOOT_INFO_SLOT_APP1
#define BOOT_INFO_PARTITION_APP2  BOOT_INFO_SLOT_APP2

#define MAGIC_NORMAL              BOOT_MAGIC_NORMAL
#define MAGIC_REQUEST             BOOT_MAGIC_REQUEST
#define MAGIC_NEW_FW              BOOT_MAGIC_NEW_FW

/* Compatibility aliases while legacy v2 field names are migrated. */
#define active_partition          active_slot
#define target_partition          target_slot
#define app1_version              slot_versions[BOOT_INFO_SLOT_APP1]
#define app2_version              slot_versions[BOOT_INFO_SLOT_APP2]

#define ADDR_FLASH_SECTOR_0     ((uint32_t)0x08000000)
#define ADDR_FLASH_SECTOR_1     ((uint32_t)0x08004000)
#define ADDR_FLASH_SECTOR_2     ((uint32_t)0x08008000)
#define ADDR_FLASH_SECTOR_3     ((uint32_t)0x0800C000)
#define ADDR_FLASH_SECTOR_4     ((uint32_t)0x08010000)
#define ADDR_FLASH_SECTOR_5     ((uint32_t)0x08020000)
#define ADDR_FLASH_SECTOR_6     ((uint32_t)0x08040000)
#define ADDR_FLASH_SECTOR_7     ((uint32_t)0x08060000)
#define ADDR_FLASH_SECTOR_8     ((uint32_t)0x08080000)
#define ADDR_FLASH_SECTOR_9     ((uint32_t)0x080A0000)
#define ADDR_FLASH_SECTOR_10    ((uint32_t)0x080C0000)
#define ADDR_FLASH_SECTOR_11    ((uint32_t)0x080E0000)

#define USER_FLASH_END_ADDRESS  0x080FFFFF
#define USER_FLASH_SIZE         (USER_FLASH_END_ADDRESS - APPLICATION_ADDRESS + 1U)

#define APPLICATION_ADDRESS     ((uint32_t)APP_CFG_APPLICATION_ADDRESS)

uint32_t FLASH_If_Write(__IO uint32_t *FlashAddress, uint32_t *Data, uint32_t DataLength);
uint32_t MY_FLASH_Erase(uint32_t Address);

#endif
