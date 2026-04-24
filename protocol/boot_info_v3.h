#ifndef BOOT_INFO_V3_H
#define BOOT_INFO_V3_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_INFO_LAYOUT_MAGIC_V2      0x42495632UL
#define BOOT_INFO_LAYOUT_MAGIC_V3      0x42495633UL
#define BOOT_INFO_LAYOUT_VERSION_V3    3U
#define BOOT_INFO_VERSION_LEN          16U
#define BOOT_INFO_SLOT_COUNT           2U

#define BOOT_INFO_SLOT_APP1            0U
#define BOOT_INFO_SLOT_APP2            1U

#define BOOT_INFO_TRIAL_NONE           0U
#define BOOT_INFO_TRIAL_PENDING        1U

#define BOOT_MAGIC_NORMAL              0x00000000UL
#define BOOT_MAGIC_REQUEST             0xDEADBEAFUL
#define BOOT_MAGIC_NEW_FW              0xBEEF0000UL

#define BOOT_UPGRADE_FLAG_NONE         0U
#define BOOT_UPGRADE_FLAG_UPGRADE      1U
#define BOOT_UPGRADE_FLAG_ROLLBACK     2U

typedef struct
{
    uint32_t layout_magic;
    uint16_t layout_version;
    uint16_t struct_size;
    uint32_t data_crc32;

    uint32_t boot_magic;
    uint32_t upgrade_flag;
    uint32_t active_slot;
    uint32_t target_slot;
    uint32_t confirmed_slot;
    uint32_t trial_state;
    uint32_t boot_tries;
    uint32_t rollback_counter;

    char current_version[BOOT_INFO_VERSION_LEN];
    char slot_versions[BOOT_INFO_SLOT_COUNT][BOOT_INFO_VERSION_LEN];
    char last_good_version[BOOT_INFO_VERSION_LEN];
    char min_allowed_ota_version[BOOT_INFO_VERSION_LEN];
    char pending_floor_version[BOOT_INFO_VERSION_LEN];

    uint32_t reserved[8];
} BootInfoTypeDef;

typedef struct
{
    uint32_t layout_magic;
    uint16_t layout_version;
    uint16_t struct_size;
    uint32_t data_crc32;

    uint32_t boot_magic;
    uint32_t upgrade_flag;
    uint32_t active_partition;
    uint32_t target_partition;
    uint32_t boot_tries;
    uint32_t trial_complete;

    char current_version[BOOT_INFO_VERSION_LEN];
    char app1_version[BOOT_INFO_VERSION_LEN];
    char app2_version[BOOT_INFO_VERSION_LEN];

    uint32_t reserved[4];
} BootInfoV2Legacy;

#ifdef __cplusplus
}
#endif

#endif
