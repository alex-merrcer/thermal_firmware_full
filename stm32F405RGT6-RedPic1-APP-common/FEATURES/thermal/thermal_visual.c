#include "thermal_visual.h"

#include <math.h>

#include "redpic1_thermal.h"

#define THERMAL_VISUAL_SRC_ROWS                       24U
#define THERMAL_VISUAL_SRC_COLS                       32U
#define THERMAL_VISUAL_PIXEL_COUNT                    768U
#define THERMAL_VISUAL_VALID_TEMP_MIN_C               (-40.0f)
#define THERMAL_VISUAL_VALID_TEMP_MAX_C               (300.0f)
#define THERMAL_VISUAL_VALID_MIN_SPAN_C               (0.5f)
#define THERMAL_VISUAL_DISPLAY_WINDOW_MIN_SPAN_C      1.5f
#define THERMAL_VISUAL_DISPLAY_WINDOW_NORMAL_EMA_ALPHA REDPIC1_THERMAL_STAGEP7_NORMAL_EMA_ALPHA
#define THERMAL_VISUAL_DISPLAY_WINDOW_NORMAL_MAX_STEP_C REDPIC1_THERMAL_STAGEP7_NORMAL_MAX_STEP_C
#define THERMAL_VISUAL_DISPLAY_WINDOW_MOTION_EMA_ALPHA 0.45f
#define THERMAL_VISUAL_DISPLAY_WINDOW_MOTION_MAX_STEP_C 3.0f
#define THERMAL_VISUAL_DISPLAY_WINDOW_HALF_SPAN_C     (THERMAL_VISUAL_DISPLAY_WINDOW_MIN_SPAN_C * 0.5f)
#define THERMAL_VISUAL_HIGH_MOTION_DELTA_C            0.8f
#define THERMAL_VISUAL_HIGH_MOTION_STRONG_DELTA_C     2.2f
#define THERMAL_VISUAL_HIGH_MOTION_PIXEL_THRESHOLD    48U

static float s_display_min_temp = 0.0f;
static float s_display_max_temp = 0.0f;
static uint8_t s_display_window_valid = 0U;
static CCMRAM float s_previous_filtered_temp_frame[THERMAL_VISUAL_PIXEL_COUNT];
static CCMRAM float s_current_visual_temp_frame[THERMAL_VISUAL_PIXEL_COUNT];
static uint8_t s_filter_history_valid = 0U;

static void redpic1_thermal_visual_reset_display_window_state(void)
{
    s_display_min_temp = 0.0f;
    s_display_max_temp = 0.0f;
    s_display_window_valid = 0U;
}

static float redpic1_thermal_visual_limit_display_window_step(float current_value,
                                                              float target_value,
                                                              float max_step_c)
{
    float delta = target_value - current_value;

    if (max_step_c <= 0.0f)
    {
        return target_value;
    }

    if (delta > max_step_c)
    {
        delta = max_step_c;
    }
    else if (delta < -max_step_c)
    {
        delta = -max_step_c;
    }

    return current_value + delta;
}

static void redpic1_thermal_visual_get_display_window(float raw_min_temp,
                                                      float raw_max_temp,
                                                      uint8_t high_motion_frame,
                                                      float *out_display_min_temp,
                                                      float *out_display_max_temp)
{
    float ema_alpha = THERMAL_VISUAL_DISPLAY_WINDOW_NORMAL_EMA_ALPHA;
    float max_step_c = THERMAL_VISUAL_DISPLAY_WINDOW_NORMAL_MAX_STEP_C;
    float center_temp = (raw_min_temp + raw_max_temp) * 0.5f;
    float half_span = fmaxf((raw_max_temp - raw_min_temp) * 0.5f,
                            THERMAL_VISUAL_DISPLAY_WINDOW_HALF_SPAN_C);
    float target_min_temp = center_temp - half_span;
    float target_max_temp = center_temp + half_span;

    if (high_motion_frame != 0U)
    {
        ema_alpha = THERMAL_VISUAL_DISPLAY_WINDOW_MOTION_EMA_ALPHA;
        max_step_c = THERMAL_VISUAL_DISPLAY_WINDOW_MOTION_MAX_STEP_C;
    }

    if (s_display_window_valid == 0U)
    {
        s_display_min_temp = target_min_temp;
        s_display_max_temp = target_max_temp;
        s_display_window_valid = 1U;
    }
    else
    {
        float ema_min_temp = 0.0f;
        float ema_max_temp = 0.0f;
        float cur_center = 0.0f;
        float cur_half_span = 0.0f;

        ema_min_temp = s_display_min_temp + ((target_min_temp - s_display_min_temp) * ema_alpha);
        ema_max_temp = s_display_max_temp + ((target_max_temp - s_display_max_temp) * ema_alpha);

        s_display_min_temp = redpic1_thermal_visual_limit_display_window_step(s_display_min_temp,
                                                                              ema_min_temp,
                                                                              max_step_c);
        s_display_max_temp = redpic1_thermal_visual_limit_display_window_step(s_display_max_temp,
                                                                              ema_max_temp,
                                                                              max_step_c);

        cur_center = (s_display_min_temp + s_display_max_temp) * 0.5f;
        cur_half_span = fmaxf((s_display_max_temp - s_display_min_temp) * 0.5f,
                              THERMAL_VISUAL_DISPLAY_WINDOW_HALF_SPAN_C);

        s_display_min_temp = cur_center - cur_half_span;
        s_display_max_temp = cur_center + cur_half_span;

        if (s_display_max_temp <= s_display_min_temp)
        {
            s_display_min_temp = target_min_temp;
            s_display_max_temp = target_max_temp;
        }
    }

    *out_display_min_temp = s_display_min_temp;
    *out_display_max_temp = s_display_max_temp;
}

static void redpic1_thermal_visual_reset_filter_state(void)
{
    s_filter_history_valid = 0U;
}

static void redpic1_thermal_visual_adopt_raw_history(const float *raw_frame_data)
{
    uint16_t i = 0U;

    if (raw_frame_data == 0)
    {
        return;
    }

    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        s_current_visual_temp_frame[i] = raw_frame_data[i];
        s_previous_filtered_temp_frame[i] = raw_frame_data[i];
    }
    s_filter_history_valid = 1U;
}

static const float *redpic1_thermal_visual_get_visual_frame(const float *raw_frame_data,
                                                            uint8_t *out_high_motion_frame)
{
    uint16_t i = 0U;
    uint16_t high_motion_pixel_count = 0U;
    float max_abs_delta = 0.0f;

    if (out_high_motion_frame != 0)
    {
        *out_high_motion_frame = 0U;
    }

    if (raw_frame_data == 0)
    {
        return 0;
    }

    if (s_filter_history_valid == 0U)
    {
        redpic1_thermal_visual_adopt_raw_history(raw_frame_data);
        return s_current_visual_temp_frame;
    }

    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        float raw_temp = raw_frame_data[i];
        float prev_temp = s_previous_filtered_temp_frame[i];
        float delta = raw_temp - prev_temp;
        float abs_delta = delta;
        float current_weight = 1.0f;
        float filtered_temp = 0.0f;

        if (abs_delta < 0.0f)
        {
            abs_delta = -abs_delta;
        }

        if (abs_delta > max_abs_delta)
        {
            max_abs_delta = abs_delta;
        }

        if (abs_delta >= THERMAL_VISUAL_HIGH_MOTION_DELTA_C)
        {
            high_motion_pixel_count++;
        }

        if (abs_delta <= 0.20f)
        {
            current_weight = 0.40f;
        }
        else if (abs_delta < 1.00f)
        {
            current_weight = 0.40f + (((abs_delta - 0.20f) / 0.80f) * 0.60f);
        }

        filtered_temp = prev_temp + ((raw_temp - prev_temp) * current_weight);
        s_current_visual_temp_frame[i] = filtered_temp;
        s_previous_filtered_temp_frame[i] = filtered_temp;
    }

    if (out_high_motion_frame != 0)
    {
        if (high_motion_pixel_count >= THERMAL_VISUAL_HIGH_MOTION_PIXEL_THRESHOLD ||
            max_abs_delta >= THERMAL_VISUAL_HIGH_MOTION_STRONG_DELTA_C)
        {
            *out_high_motion_frame = 1U;
        }
    }

    return s_current_visual_temp_frame;
}

static uint8_t redpic1_thermal_visual_temp_in_range(float temp)
{
    if (temp != temp)
    {
        return 0U;
    }

    if (temp < THERMAL_VISUAL_VALID_TEMP_MIN_C)
    {
        return 0U;
    }
    if (temp > THERMAL_VISUAL_VALID_TEMP_MAX_C)
    {
        return 0U;
    }

    return 1U;
}

void redpic1_thermal_visual_init(const redpic1_thermal_visual_ops_t *ops)
{
    (void)ops;

    redpic1_thermal_visual_reset_history();
}

void redpic1_thermal_visual_reset_history(void)
{
    redpic1_thermal_visual_reset_display_window_state();
    redpic1_thermal_visual_reset_filter_state();
}

void redpic1_thermal_visual_invalidate_history(void)
{
    /* Capture-gap invalidation was part of the discarded P6 branch. */
}

uint8_t redpic1_thermal_visual_capture_gap_exceeded(uint32_t capture_tick_ms)
{
    (void)capture_tick_ms;
    return 0U;
}

void redpic1_thermal_visual_note_capture_success(uint32_t capture_tick_ms)
{
    (void)capture_tick_ms;
}

const float *redpic1_thermal_visual_get_gray_source_frame(const float *raw_frame_data,
                                                          uint8_t *out_high_motion_frame)
{
    const float *visual_frame = redpic1_thermal_visual_get_visual_frame(raw_frame_data,
                                                                        out_high_motion_frame);

    return (visual_frame != 0) ? visual_frame : raw_frame_data;
}

void redpic1_thermal_visual_prepare_gray_frame(const float *raw_frame_data,
                                               const float *display_frame_data,
                                               uint8_t high_motion_frame,
                                               uint8_t *gray_frame,
                                               float *out_min_temp,
                                               float *out_max_temp)
{
    float raw_min_temp = 300.0f;
    float raw_max_temp = -40.0f;
    float display_min_temp = 0.0f;
    float display_max_temp = 0.0f;
    float scale = 0.0f;
    uint16_t i = 0U;
    uint16_t src_row = 0U;

    if (raw_frame_data == 0 || display_frame_data == 0 || gray_frame == 0)
    {
        return;
    }

    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        float temp = raw_frame_data[i];

        if (temp > raw_max_temp)
        {
            raw_max_temp = temp;
        }
        if (temp < raw_min_temp)
        {
            raw_min_temp = temp;
        }
    }

    if (raw_max_temp <= raw_min_temp)
    {
        for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
        {
            gray_frame[i] = 0U;
        }
        if (out_min_temp != 0)
        {
            *out_min_temp = raw_min_temp;
        }
        if (out_max_temp != 0)
        {
            *out_max_temp = raw_max_temp;
        }
        return;
    }

    redpic1_thermal_visual_get_display_window(raw_min_temp,
                                              raw_max_temp,
                                              high_motion_frame,
                                              &display_min_temp,
                                              &display_max_temp);

    if (display_max_temp <= display_min_temp)
    {
        display_min_temp = raw_min_temp;
        display_max_temp = raw_max_temp;
    }

    scale = 255.0f / (display_max_temp - display_min_temp);

    for (src_row = 0U; src_row < THERMAL_VISUAL_SRC_ROWS; ++src_row)
    {
        const float *src = display_frame_data + ((uint32_t)src_row * THERMAL_VISUAL_SRC_COLS);
        uint8_t *dst = gray_frame + src_row;
        uint16_t src_col = 0U;

        for (src_col = 0U; src_col < THERMAL_VISUAL_SRC_COLS; ++src_col)
        {
            int32_t gray_value = (int32_t)(((*src++) - display_min_temp) * scale);

            if (gray_value < 0)
            {
                gray_value = 0;
            }
            else if (gray_value > 255)
            {
                gray_value = 255;
            }

            *dst = (uint8_t)gray_value;
            dst += THERMAL_VISUAL_SRC_ROWS;
        }
    }

    if (out_min_temp != 0)
    {
        *out_min_temp = raw_min_temp;
    }
    if (out_max_temp != 0)
    {
        *out_max_temp = raw_max_temp;
    }
}

float redpic1_thermal_visual_center_temp(const float *frame_data)
{
    uint16_t center_row = THERMAL_VISUAL_SRC_ROWS / 2U;
    uint16_t center_col = THERMAL_VISUAL_SRC_COLS / 2U;

    if (frame_data == 0)
    {
        return 0.0f;
    }

    return frame_data[(center_row * THERMAL_VISUAL_SRC_COLS) + center_col];
}

uint8_t redpic1_thermal_visual_frame_data_is_valid(const float *frame_data)
{
    uint16_t i = 0U;

    if (frame_data == 0)
    {
        return 0U;
    }

    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        if (redpic1_thermal_visual_temp_in_range(frame_data[i]) == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

uint8_t redpic1_thermal_visual_frame_is_valid(float min_temp,
                                              float max_temp,
                                              float center_temp)
{
    if (redpic1_thermal_visual_temp_in_range(min_temp) == 0U ||
        redpic1_thermal_visual_temp_in_range(max_temp) == 0U ||
        redpic1_thermal_visual_temp_in_range(center_temp) == 0U)
    {
        return 0U;
    }

    if (max_temp < min_temp)
    {
        return 0U;
    }

    if ((max_temp - min_temp) < THERMAL_VISUAL_VALID_MIN_SPAN_C)
    {
        return 0U;
    }

    return 1U;
}

uint8_t redpic1_thermal_visual_gray_frame_has_contrast(const uint8_t *gray_frame)
{
    uint8_t gray_min = 255U;
    uint8_t gray_max = 0U;
    uint16_t i = 0U;

    if (gray_frame == 0)
    {
        return 0U;
    }

    for (i = 0U; i < THERMAL_VISUAL_PIXEL_COUNT; ++i)
    {
        if (gray_frame[i] < gray_min)
        {
            gray_min = gray_frame[i];
        }
        if (gray_frame[i] > gray_max)
        {
            gray_max = gray_frame[i];
        }
    }

    return (gray_max > gray_min) ? 1U : 0U;
}
