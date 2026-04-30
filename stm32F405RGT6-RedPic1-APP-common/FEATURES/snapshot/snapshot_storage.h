#ifndef SNAPSHOT_STORAGE_H
#define SNAPSHOT_STORAGE_H

#include <stdint.h>

#include "redpic1_thermal.h"
#include "storage_service.h"

storage_status_t snapshot_storage_save_latest(uint32_t *out_index);
storage_status_t snapshot_storage_get_count(uint32_t *out_count);
storage_status_t snapshot_storage_get_latest_index(uint32_t *out_index);
storage_status_t snapshot_storage_load_index(uint32_t index,
                                             redpic1_thermal_snapshot_t *out_snapshot);
storage_status_t snapshot_storage_load_latest(redpic1_thermal_snapshot_t *out_snapshot,
                                              uint32_t *out_index);
storage_status_t snapshot_storage_clear_all(void);
storage_status_t snapshot_storage_build_gray_preview(const redpic1_thermal_snapshot_t *snapshot,
                                                     uint8_t *gray_frame);
storage_status_t snapshot_storage_build_view_latest_gray_preview(const redpic1_thermal_snapshot_t *snapshot,
                                                                 uint8_t *gray_frame);

#endif
