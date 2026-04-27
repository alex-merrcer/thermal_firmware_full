#ifndef THERMAL_VISUAL_H
#define THERMAL_VISUAL_H

#include <stdint.h>

typedef struct
{
    uint32_t (*get_active_period_ms)(void);
} redpic1_thermal_visual_ops_t;

void redpic1_thermal_visual_init(const redpic1_thermal_visual_ops_t *ops);
void redpic1_thermal_visual_reset_history(void);
void redpic1_thermal_visual_invalidate_history(void);
uint8_t redpic1_thermal_visual_capture_gap_exceeded(uint32_t capture_tick_ms);
void redpic1_thermal_visual_note_capture_success(uint32_t capture_tick_ms);
const float *redpic1_thermal_visual_get_gray_source_frame(const float *raw_frame_data,
                                                          uint8_t *out_high_motion_frame);
void redpic1_thermal_visual_prepare_gray_frame(const float *raw_frame_data,
                                               const float *display_frame_data,
                                               uint8_t high_motion_frame,
                                               uint8_t *gray_frame,
                                               float *out_min_temp,
                                               float *out_max_temp);
float redpic1_thermal_visual_center_temp(const float *frame_data);
uint8_t redpic1_thermal_visual_frame_data_is_valid(const float *frame_data);
uint8_t redpic1_thermal_visual_frame_is_valid(float min_temp,
                                              float max_temp,
                                              float center_temp);
uint8_t redpic1_thermal_visual_gray_frame_has_contrast(const uint8_t *gray_frame);

#endif
