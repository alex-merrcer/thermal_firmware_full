#include "thermal_snapshot_file.h"

#include <string.h>

uint16_t thermal_snapshot_file_crc16(const uint8_t *data, uint32_t length)
{
    uint16_t crc = 0xFFFFU;
    uint32_t i = 0U;
    uint32_t bit = 0U;

    if (data == 0)
    {
        return 0U;
    }

    for (i = 0U; i < length; ++i)
    {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((crc & 0x8000U) != 0U)
            {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

void thermal_snapshot_file_fill(redpic_snapshot_t *file_snapshot,
                                const redpic1_thermal_snapshot_t *thermal_snapshot)
{
    uint16_t index = 0U;

    if (file_snapshot == 0 || thermal_snapshot == 0)
    {
        return;
    }

    memset(file_snapshot, 0, sizeof(*file_snapshot));
    file_snapshot->magic = REDPIC_RTS_MAGIC;
    file_snapshot->version = REDPIC_RTS_VERSION;
    file_snapshot->width = REDPIC_RTS_WIDTH;
    file_snapshot->height = REDPIC_RTS_HEIGHT;
    file_snapshot->min_x10 = thermal_snapshot->min_x10;
    file_snapshot->max_x10 = thermal_snapshot->max_x10;
    file_snapshot->center_x10 = thermal_snapshot->center_x10;
    file_snapshot->frame_id = thermal_snapshot->frame_id;
    file_snapshot->timestamp = thermal_snapshot->timestamp_ms;
    file_snapshot->crc16 = 0U;
    for (index = 0U; index < REDPIC_RTS_PIXELS; ++index)
    {
        file_snapshot->pixels_x10[index] = thermal_snapshot->pixels_x10[index];
    }

    file_snapshot->crc16 = thermal_snapshot_file_crc16((const uint8_t *)file_snapshot,
                                                       (uint32_t)sizeof(*file_snapshot));
}
