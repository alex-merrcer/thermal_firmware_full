#ifndef SNAPSHOT_STORAGE_H
#define SNAPSHOT_STORAGE_H

#include <stdint.h>

#include "storage_service.h"

storage_status_t snapshot_storage_save_latest(uint32_t *out_index);
storage_status_t snapshot_storage_get_latest_index(uint32_t *out_index);

#endif
