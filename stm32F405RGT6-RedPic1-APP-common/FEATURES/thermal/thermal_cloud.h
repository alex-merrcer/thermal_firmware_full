#ifndef THERMAL_CLOUD_H
#define THERMAL_CLOUD_H

#include <stdint.h>

void redpic1_thermal_cloud_init(void);
void redpic1_thermal_cloud_reset(void);
uint8_t redpic1_thermal_cloud_pause_send_esp_enabled(void);
uint8_t redpic1_thermal_cloud_submit_snapshot_to_esp(void);

#endif
