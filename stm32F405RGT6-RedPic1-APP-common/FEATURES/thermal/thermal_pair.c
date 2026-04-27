#include "thermal_pair.h"

#include <string.h>

#include "app_perf_baseline.h"
#include "redpic1_thermal.h"
#include "sys.h"

#define THERMAL_PAIR_PIXEL_COUNT 768U
#define THERMAL_PAIR_SRC_COLS    32U

#if (REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ENABLE != 0U)
    #define THERMAL_PAIR_ACTIVE 1U
#else
    #define THERMAL_PAIR_ACTIVE 0U
#endif

#if (THERMAL_PAIR_ACTIVE != 0U) && (REDPIC1_THERMAL_STAGEV4_C1_PAIR_GRACE_ENABLE != 0U)
    #define THERMAL_PAIR_GRACE_ACTIVE 1U
#else
    #define THERMAL_PAIR_GRACE_ACTIVE 0U
#endif

#if (THERMAL_PAIR_ACTIVE != 0U)
static CCMRAM float s_v4_subpage_temp_frame[2][THERMAL_PAIR_PIXEL_COUNT];
static uint32_t s_v4_subpage_tick_ms[2] = { 0U, 0U };
static uint8_t s_v4_subpage_valid[2] = { 0U, 0U };
static uint8_t s_v4_pair_last_arrived_subpage = 0xFFU;
static uint32_t s_v4_pair_same_subpage_streak = 0U;

static uint8_t redpic1_thermal_pair_pixel_subpage(uint16_t pixel_index)
{
    uint16_t row = (uint16_t)(pixel_index / THERMAL_PAIR_SRC_COLS);
    uint16_t col = (uint16_t)(pixel_index % THERMAL_PAIR_SRC_COLS);

    return (uint8_t)((row ^ col) & 0x01U);
}
#endif

void redpic1_thermal_pair_reset(void)
{
#if (THERMAL_PAIR_ACTIVE != 0U)
    s_v4_subpage_tick_ms[0] = 0U;
    s_v4_subpage_tick_ms[1] = 0U;
    s_v4_subpage_valid[0] = 0U;
    s_v4_subpage_valid[1] = 0U;
    s_v4_pair_last_arrived_subpage = 0xFFU;
    s_v4_pair_same_subpage_streak = 0U;
#endif
}

uint8_t redpic1_thermal_pair_try_compose(float *frame_data,
                                         uint8_t subpage,
                                         uint32_t capture_tick_ms,
                                         uint32_t get_temp_elapsed_us,
                                         uint32_t step_elapsed_us,
                                         uint32_t *out_capture_tick_ms)
{
#if (THERMAL_PAIR_ACTIVE != 0U)
    uint8_t other_subpage = (uint8_t)(subpage ^ 0x01U);
    uint16_t pixel_index = 0U;
    uint32_t gap_ms = 0U;
    uint8_t pair_grace_ok = 0U;

    if (frame_data == 0 || subpage > 1U)
    {
        return 0U;
    }

    if (s_v4_pair_last_arrived_subpage == subpage)
    {
        s_v4_pair_same_subpage_streak++;
    }
    else
    {
        s_v4_pair_last_arrived_subpage = subpage;
        s_v4_pair_same_subpage_streak = 1U;
    }

    memcpy(s_v4_subpage_temp_frame[subpage],
           frame_data,
           sizeof(s_v4_subpage_temp_frame[subpage]));
    s_v4_subpage_tick_ms[subpage] = capture_tick_ms;
    s_v4_subpage_valid[subpage] = 1U;

    if (s_v4_subpage_valid[other_subpage] != 0U)
    {
        gap_ms = capture_tick_ms - s_v4_subpage_tick_ms[other_subpage];
    }

    if (s_v4_subpage_valid[other_subpage] != 0U &&
        gap_ms > REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_MAX_AGE_MS)
    {
#if (THERMAL_PAIR_GRACE_ACTIVE != 0U)
        if (gap_ms <= REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_GRACE_AGE_MS)
        {
            pair_grace_ok = 1U;
        }
        else
#endif
        {
            app_perf_baseline_record_thermal_pair_timeout_detail(subpage,
                                                                 other_subpage,
                                                                 gap_ms,
                                                                 s_v4_pair_same_subpage_streak,
                                                                 get_temp_elapsed_us,
                                                                 step_elapsed_us);
            s_v4_subpage_valid[other_subpage] = 0U;
            s_v4_subpage_tick_ms[other_subpage] = 0U;
        }
    }

    if (s_v4_subpage_valid[other_subpage] == 0U)
    {
        app_perf_baseline_record_thermal_pair_wait_other(subpage,
                                                         other_subpage,
                                                         gap_ms,
                                                         s_v4_pair_same_subpage_streak);
        return 0U;
    }

    for (pixel_index = 0U; pixel_index < THERMAL_PAIR_PIXEL_COUNT; ++pixel_index)
    {
        uint8_t owner_subpage = redpic1_thermal_pair_pixel_subpage(pixel_index);
        frame_data[pixel_index] = s_v4_subpage_temp_frame[owner_subpage][pixel_index];
    }

    if (out_capture_tick_ms != 0)
    {
        *out_capture_tick_ms = (s_v4_subpage_tick_ms[subpage] >= s_v4_subpage_tick_ms[other_subpage]) ?
                               s_v4_subpage_tick_ms[subpage] :
                               s_v4_subpage_tick_ms[other_subpage];
    }

    if (pair_grace_ok != 0U)
    {
        app_perf_baseline_record_thermal_pair_grace_ok(subpage,
                                                       other_subpage,
                                                       gap_ms,
                                                       s_v4_pair_same_subpage_streak);
    }
    else
    {
        app_perf_baseline_record_thermal_pair_compose_ok(subpage,
                                                         other_subpage,
                                                         gap_ms,
                                                         s_v4_pair_same_subpage_streak);
    }

    return 1U;
#else
    (void)get_temp_elapsed_us;
    (void)step_elapsed_us;

    if (frame_data == 0 || subpage > 1U)
    {
        return 0U;
    }

    if (out_capture_tick_ms != 0)
    {
        *out_capture_tick_ms = capture_tick_ms;
    }

    return 1U;
#endif
}
