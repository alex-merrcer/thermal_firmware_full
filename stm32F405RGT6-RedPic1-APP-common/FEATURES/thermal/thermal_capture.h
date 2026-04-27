#ifndef THERMAL_CAPTURE_H
#define THERMAL_CAPTURE_H

#include <stdint.h>

typedef struct
{
    uint8_t (*get_refresh_rate)(void);
    void (*apply_refresh_rate)(uint8_t refresh_rate, uint8_t force_write);
    void (*invalidate_history)(void);
} redpic1_thermal_capture_ops_t;

void redpic1_thermal_capture_init(const redpic1_thermal_capture_ops_t *ops);
void redpic1_thermal_capture_reset(void);
uint8_t redpic1_thermal_capture_prepare_step(void);
uint8_t redpic1_thermal_capture_read_frame(float *frame_data,
                                           uint8_t *out_subpage,
                                           uint32_t *out_capture_tick_ms,
                                           uint32_t *out_get_temp_elapsed_us);
void redpic1_thermal_capture_note_backoff(uint8_t transport_related);
void redpic1_thermal_capture_note_success(void);
void redpic1_thermal_capture_request_restore_after_stop(uint8_t scheduler_running);

#endif
