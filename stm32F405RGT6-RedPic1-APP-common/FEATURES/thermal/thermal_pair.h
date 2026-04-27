#ifndef THERMAL_PAIR_H
#define THERMAL_PAIR_H

#include <stdint.h>

void redpic1_thermal_pair_reset(void);
uint8_t redpic1_thermal_pair_try_compose(float *frame_data,
                                         uint8_t subpage,
                                         uint32_t capture_tick_ms,
                                         uint32_t get_temp_elapsed_us,
                                         uint32_t step_elapsed_us,
                                         uint32_t *out_capture_tick_ms);

#endif
