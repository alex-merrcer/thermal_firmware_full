#ifndef THERMAL_SNAPSHOT_FILE_H
#define THERMAL_SNAPSHOT_FILE_H

#include <stdint.h>

#include "redpic1_thermal.h"

#define REDPIC_RTS_MAGIC   0x31535452UL
#define REDPIC_RTS_VERSION 1U
#define REDPIC_RTS_WIDTH   32U
#define REDPIC_RTS_HEIGHT  24U
#define REDPIC_RTS_PIXELS  (REDPIC_RTS_WIDTH * REDPIC_RTS_HEIGHT)

typedef __packed struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t width;
    uint16_t height;
    int16_t min_x10;
    int16_t max_x10;
    int16_t center_x10;
    uint32_t frame_id;
    uint32_t timestamp;
    uint16_t crc16;
    int16_t pixels_x10[REDPIC_RTS_PIXELS];
} redpic_snapshot_t;

uint16_t thermal_snapshot_file_crc16(const uint8_t *data, uint32_t length);
void thermal_snapshot_file_fill(redpic_snapshot_t *file_snapshot,
                                const redpic1_thermal_snapshot_t *thermal_snapshot);
uint8_t thermal_snapshot_file_parse(redpic1_thermal_snapshot_t *thermal_snapshot,
                                    const redpic_snapshot_t *file_snapshot);

#endif
