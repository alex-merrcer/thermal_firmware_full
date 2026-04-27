#ifndef THERMAL_FRAME_SLOT_H
#define THERMAL_FRAME_SLOT_H

#include <stdint.h>

#define REDPIC1_THERMAL_FRAME_SLOT_PIXEL_COUNT 768U
#define REDPIC1_THERMAL_FRAME_SLOT_COUNT       3U
#define REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE  0xFFU

typedef enum
{
    REDPIC1_THERMAL_FRAME_SLOT_FREE = 0,
    REDPIC1_THERMAL_FRAME_SLOT_WRITING,
    REDPIC1_THERMAL_FRAME_SLOT_READY,
    REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT,
    REDPIC1_THERMAL_FRAME_SLOT_FRONT
} redpic1_thermal_frame_slot_state_t;

typedef struct
{
    float   temp_frame[REDPIC1_THERMAL_FRAME_SLOT_PIXEL_COUNT];
    uint8_t gray_frame[REDPIC1_THERMAL_FRAME_SLOT_PIXEL_COUNT];
    float   min_temp;
    float   max_temp;
    float   center_temp;
    uint32_t capture_tick_ms;
    uint32_t ready_tick_ms;
    uint32_t frame_seq;
    uint8_t valid;
    redpic1_thermal_frame_slot_state_t slot_state;
} redpic1_thermal_frame_slot_t;

typedef struct
{
    void (*enter_critical)(void);
    void (*exit_critical)(void);
    uint32_t (*get_tick_ms)(void);
    uint32_t (*get_expired_frame_threshold_ms)(void);
    uint8_t (*present_gray_frame)(const uint8_t *gray_frame);
    uint8_t (*run_enabled)(void);
    uint8_t (*display_paused)(void);
    uint8_t (*overlay_hold)(void);
} redpic1_thermal_frame_slot_ops_t;

void redpic1_thermal_frame_slot_init(const redpic1_thermal_frame_slot_ops_t *ops);
void redpic1_thermal_frame_slot_bind_display_runtime(void);
redpic1_thermal_frame_slot_t *redpic1_thermal_frame_slot_acquire_back(void);
void redpic1_thermal_frame_slot_release_back(redpic1_thermal_frame_slot_t *slot);
void redpic1_thermal_frame_slot_publish_back(redpic1_thermal_frame_slot_t *slot);
void redpic1_thermal_frame_slot_present_front(void);
uint32_t redpic1_thermal_frame_slot_allocate_sequence(void);
uint8_t redpic1_thermal_frame_slot_submit_latest_ready(void);
void redpic1_thermal_frame_slot_try_submit_if_possible(void);
void redpic1_thermal_frame_slot_cancel_pending_present(void);
void redpic1_thermal_frame_slot_drop_non_inflight(void);
void redpic1_thermal_frame_slot_reset(void);

#endif
