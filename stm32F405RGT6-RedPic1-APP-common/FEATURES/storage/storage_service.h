#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H

#include <stdint.h>

typedef enum
{
    STORAGE_STATUS_OK = 0,
    STORAGE_STATUS_NOT_READY,
    STORAGE_STATUS_INIT_FAIL,
    STORAGE_STATUS_MOUNT_FAIL,
    STORAGE_STATUS_FS_ERROR,
    STORAGE_STATUS_IO_ERROR,
    STORAGE_STATUS_NO_SNAPSHOT
} storage_status_t;

typedef struct
{
    uint32_t total_kb;
    uint32_t free_kb;
    uint8_t mounted;
    uint8_t card_present;
    storage_status_t last_status;
} storage_info_t;

void storage_service_init(void);
uint8_t storage_service_mount(void);
uint8_t storage_service_is_mounted(void);
storage_status_t storage_service_get_info(storage_info_t *info);
storage_status_t storage_service_test_file(void);
storage_status_t storage_service_ensure_redpic_dirs(void);
const char *storage_service_status_text(storage_status_t status);

#endif
