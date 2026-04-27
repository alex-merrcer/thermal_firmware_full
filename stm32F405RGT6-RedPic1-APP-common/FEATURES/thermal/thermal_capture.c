#include "thermal_capture.h"

#include <string.h>

#include "app_perf_baseline.h"
#include "power_manager.h"
#include "MLX90640.h"
#include "MLX90640_I2C_Driver.h"
#include "redpic1_thermal.h"

#define THERMAL_CAPTURE_BACKOFF_MS                20UL
#define THERMAL_CAPTURE_RESTORE_THRESHOLD         3U
#define THERMAL_CAPTURE_MLX90640_REG_STATUS       0x8000U
#define THERMAL_CAPTURE_STATUS_DATA_READY_MASK    (1U << 3)

#if (REDPIC1_THERMAL_STAGE6R_ENABLE != 0U) && (REDPIC1_THERMAL_STAGE6R_1_ENABLE != 0U)
    #define THERMAL_CAPTURE_STAGE6R_1_ACTIVE 1U
#else
    #define THERMAL_CAPTURE_STAGE6R_1_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6R_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_1_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_2_ENABLE != 0U)
    #define THERMAL_CAPTURE_STAGE6R_2_ACTIVE 1U
#else
    #define THERMAL_CAPTURE_STAGE6R_2_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGE6R_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_1_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_2_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_3_ENABLE != 0U)
    #define THERMAL_CAPTURE_STAGE6R_3_ACTIVE 1U
#else
    #define THERMAL_CAPTURE_STAGE6R_3_ACTIVE 0U
#endif

#if (REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ENABLE != 0U)
    #define THERMAL_CAPTURE_FULL_SUBPAGE_PAIR_ACTIVE 1U
#else
    #define THERMAL_CAPTURE_FULL_SUBPAGE_PAIR_ACTIVE 0U
#endif

static redpic1_thermal_capture_ops_t s_ops;
static uint32_t s_backoff_until_ms = 0U;
static uint8_t s_restore_bus_pending = 0U;
static uint8_t s_consecutive_transport_failures = 0U;

static uint8_t redpic1_thermal_capture_get_refresh_rate(void)
{
    if (s_ops.get_refresh_rate != 0)
    {
        return s_ops.get_refresh_rate();
    }

    return 0U;
}

static void redpic1_thermal_capture_apply_refresh_rate(uint8_t refresh_rate, uint8_t force_write)
{
    if (s_ops.apply_refresh_rate != 0)
    {
        s_ops.apply_refresh_rate(refresh_rate, force_write);
    }
}

static void redpic1_thermal_capture_invalidate_history(void)
{
    if (s_ops.invalidate_history != 0)
    {
        s_ops.invalidate_history();
    }
}

static uint8_t redpic1_thermal_capture_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (((int32_t)(now_ms - deadline_ms)) >= 0) ? 1U : 0U;
}

static void redpic1_thermal_capture_restore_bus_now(void)
{
    MLX90640_I2CInit();
    redpic1_thermal_capture_apply_refresh_rate(redpic1_thermal_capture_get_refresh_rate(), 1U);
    s_restore_bus_pending = 0U;
    s_consecutive_transport_failures = 0U;
    redpic1_thermal_capture_invalidate_history();
}

void redpic1_thermal_capture_init(const redpic1_thermal_capture_ops_t *ops)
{
    memset(&s_ops, 0, sizeof(s_ops));
    if (ops != 0)
    {
        memcpy(&s_ops, ops, sizeof(s_ops));
    }

    redpic1_thermal_capture_reset();
}

void redpic1_thermal_capture_reset(void)
{
    s_backoff_until_ms = 0U;
    s_restore_bus_pending = 0U;
    s_consecutive_transport_failures = 0U;
}

void redpic1_thermal_capture_note_backoff(uint8_t transport_related)
{
    uint32_t now_ms = power_manager_get_tick_ms();

    app_perf_baseline_record_thermal_capture_failure();
    app_perf_baseline_record_thermal_backoff();

    if (transport_related != 0U)
    {
        s_consecutive_transport_failures++;

#if (THERMAL_CAPTURE_STAGE6R_3_ACTIVE != 0U)
        if (s_consecutive_transport_failures == 1U)
        {
            s_backoff_until_ms = now_ms + 2U;
        }
        else if (s_consecutive_transport_failures == 2U)
        {
            s_backoff_until_ms = now_ms + 5U;
        }
        else
        {
            s_restore_bus_pending = 1U;
            s_backoff_until_ms = now_ms + THERMAL_CAPTURE_BACKOFF_MS;
        }
#else
        s_backoff_until_ms = now_ms + THERMAL_CAPTURE_BACKOFF_MS;
        if (s_consecutive_transport_failures >= THERMAL_CAPTURE_RESTORE_THRESHOLD)
        {
            s_restore_bus_pending = 1U;
        }
#endif
    }
    else
    {
        s_backoff_until_ms = now_ms + 2U;
        s_consecutive_transport_failures = 0U;
    }
}

uint8_t redpic1_thermal_capture_prepare_step(void)
{
    uint32_t now_ms = power_manager_get_tick_ms();

    if (s_backoff_until_ms != 0U &&
        redpic1_thermal_capture_deadline_reached(now_ms, s_backoff_until_ms) == 0U)
    {
        return 0U;
    }

    s_backoff_until_ms = 0U;
    if (s_restore_bus_pending != 0U)
    {
        redpic1_thermal_capture_restore_bus_now();
    }

#if (THERMAL_CAPTURE_STAGE6R_1_ACTIVE == 0U)
    {
        uint16_t state = 0U;

        if (MLX90640_I2CRead(MLX90640_ADDR,
                             THERMAL_CAPTURE_MLX90640_REG_STATUS,
                             1,
                             &state) != 0)
        {
            redpic1_thermal_capture_note_backoff(1U);
            return 0U;
        }

        if ((state & THERMAL_CAPTURE_STATUS_DATA_READY_MASK) == 0U)
        {
            return 0U;
        }
    }
#endif

    return 1U;
}

uint8_t redpic1_thermal_capture_read_frame(float *frame_data,
                                           uint8_t *out_subpage,
                                           uint32_t *out_capture_tick_ms,
                                           uint32_t *out_get_temp_elapsed_us)
{
    uint32_t get_temp_start_cycle = 0U;
    uint32_t get_temp_elapsed_us = 0U;
    float ta = 0.0f;
    uint8_t captured_subpage = 0U;
    int temp_status = 0;

    if (frame_data == 0)
    {
        return 0U;
    }

    get_temp_start_cycle = app_perf_baseline_cycle_now();
#if (THERMAL_CAPTURE_FULL_SUBPAGE_PAIR_ACTIVE != 0U)
    temp_status = get_temp_ex(frame_data, &ta, &captured_subpage);
#else
    temp_status = get_temp(frame_data, &ta);
#endif
    get_temp_elapsed_us = app_perf_baseline_elapsed_us(get_temp_start_cycle);
    app_perf_baseline_record_get_temp_us(get_temp_elapsed_us);

    if (out_get_temp_elapsed_us != 0)
    {
        *out_get_temp_elapsed_us = get_temp_elapsed_us;
    }

    if (temp_status < 0)
    {
#if (THERMAL_CAPTURE_STAGE6R_2_ACTIVE != 0U)
        if (temp_status == -9)
        {
            app_perf_baseline_record_thermal_soft_timeout();
            redpic1_thermal_capture_note_backoff(0U);
        }
        else
#endif
        {
            redpic1_thermal_capture_invalidate_history();
            redpic1_thermal_capture_note_backoff(1U);
        }

        return 0U;
    }

    if (out_subpage != 0)
    {
        *out_subpage = captured_subpage;
    }

    if (out_capture_tick_ms != 0)
    {
        *out_capture_tick_ms = power_manager_get_tick_ms();
    }

    return 1U;
}

void redpic1_thermal_capture_note_success(void)
{
    s_consecutive_transport_failures = 0U;
}

void redpic1_thermal_capture_request_restore_after_stop(uint8_t scheduler_running)
{
    if (scheduler_running != 0U)
    {
        s_restore_bus_pending = 1U;
        s_backoff_until_ms = 0U;
        return;
    }

    redpic1_thermal_capture_restore_bus_now();
}
