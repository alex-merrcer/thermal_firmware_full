#ifndef REDPIC1_THERMAL_H
#define REDPIC1_THERMAL_H

#include <stdint.h>
#include "lcd_init.h"

/*
 * redpic1_thermal.h
 * Thermal capture/display public interface.
 */

/* Thermal diagnostic mode selection. */
#define REDPIC1_THERMAL_DIAG_MODE_NORMAL        0U
#define REDPIC1_THERMAL_DIAG_MODE_TEST_PATTERN  1U

#ifndef REDPIC1_THERMAL_DIAG_MODE
    #define REDPIC1_THERMAL_DIAG_MODE REDPIC1_THERMAL_DIAG_MODE_NORMAL
#endif

/* Stage 6L: thermal latency/ghosting diagnostics and mitigation. */
#ifndef REDPIC1_THERMAL_STAGE6L_ENABLE
    #define REDPIC1_THERMAL_STAGE6L_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_STAGE6L_1_ENABLE
    #define REDPIC1_THERMAL_STAGE6L_1_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_STAGE6L_1_WINDOW_ENABLE
    #define REDPIC1_THERMAL_STAGE6L_1_WINDOW_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_STAGE6L_1_FILTER_ENABLE
    #define REDPIC1_THERMAL_STAGE6L_1_FILTER_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_STAGEP7_NORMAL_EMA_ALPHA
    #define REDPIC1_THERMAL_STAGEP7_NORMAL_EMA_ALPHA 0.25f
#endif

#ifndef REDPIC1_THERMAL_STAGEP7_NORMAL_MAX_STEP_C
    #define REDPIC1_THERMAL_STAGEP7_NORMAL_MAX_STEP_C 0.75f
#endif

#if (REDPIC1_THERMAL_STAGE6L_ENABLE == 0U)
    #if (REDPIC1_THERMAL_STAGE6L_1_ENABLE != 0U)
        #error "REDPIC1_THERMAL_STAGE6L_ENABLE=0 requires 6L-1 to stay disabled."
    #endif
#endif

#if (REDPIC1_THERMAL_STAGE6L_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6L_1_ENABLE == 0U) && \
    ((REDPIC1_THERMAL_STAGE6L_1_WINDOW_ENABLE != 1U) || \
     (REDPIC1_THERMAL_STAGE6L_1_FILTER_ENABLE != 1U))
    #error "6L-1 window/filter sub-switches only take effect when REDPIC1_THERMAL_STAGE6L_1_ENABLE=1."
#endif

/* Stage 6R: thermal real pipeline latency optimization. */
#ifndef REDPIC1_THERMAL_STAGE6R_ENABLE
    #define REDPIC1_THERMAL_STAGE6R_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_STAGE6R_1_ENABLE
    #define REDPIC1_THERMAL_STAGE6R_1_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_STAGE6R_2_ENABLE
    #define REDPIC1_THERMAL_STAGE6R_2_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_STAGE6R_3_ENABLE
    #define REDPIC1_THERMAL_STAGE6R_3_ENABLE 1U
#endif

/* Stage V4: display anomaly split diagnostics. */
#ifndef REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ENABLE
    #define REDPIC1_THERMAL_STAGEV4_C1_FULL_SUBPAGE_PAIR_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_MAX_AGE_MS
    #define REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_MAX_AGE_MS 80UL
#endif

#ifndef REDPIC1_THERMAL_STAGEV4_C1_PAIR_GRACE_ENABLE
    #define REDPIC1_THERMAL_STAGEV4_C1_PAIR_GRACE_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_GRACE_AGE_MS
    #define REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_GRACE_AGE_MS 160UL
#endif

#ifndef REDPIC1_THERMAL_DROP_EXPIRED_FRAME_ENABLE
    #define REDPIC1_THERMAL_DROP_EXPIRED_FRAME_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_DROP_EXPIRED_FRAME_MIN_MS
    #define REDPIC1_THERMAL_DROP_EXPIRED_FRAME_MIN_MS 120UL
#endif

#ifndef REDPIC1_THERMAL_OVERLAY_DIRTY_ONLY_ENABLE
    #define REDPIC1_THERMAL_OVERLAY_DIRTY_ONLY_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ENABLE
    #define REDPIC1_THERMAL_PAUSE_SEND_ESP_FEATURE_ENABLE 1U
#endif

#ifndef REDPIC1_THERMAL_KEY2_ENTRY_GUARD_MS
    #define REDPIC1_THERMAL_KEY2_ENTRY_GUARD_MS 220UL
#endif

#if (REDPIC1_THERMAL_STAGE6R_ENABLE == 0U)
    #if (REDPIC1_THERMAL_STAGE6R_1_ENABLE != 0U) || \
        (REDPIC1_THERMAL_STAGE6R_2_ENABLE != 0U) || \
        (REDPIC1_THERMAL_STAGE6R_3_ENABLE != 0U)
        #error "REDPIC1_THERMAL_STAGE6R_ENABLE=0 requires all 6R sub-stages to stay disabled."
    #endif
#endif

#if (REDPIC1_THERMAL_STAGE6R_2_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGE6R_1_ENABLE == 0U)
    #error "6R-2 requires REDPIC1_THERMAL_STAGE6R_1_ENABLE=1."
#endif

#if (REDPIC1_THERMAL_STAGE6R_3_ENABLE != 0U) && \
    ((REDPIC1_THERMAL_STAGE6R_1_ENABLE == 0U) || \
     (REDPIC1_THERMAL_STAGE6R_2_ENABLE == 0U))
    #error "6R-3 requires REDPIC1_THERMAL_STAGE6R_1_ENABLE=1 and REDPIC1_THERMAL_STAGE6R_2_ENABLE=1."
#endif

#if (REDPIC1_THERMAL_STAGEV4_C1_PAIR_GRACE_ENABLE != 0U) && \
    (REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_GRACE_AGE_MS <= REDPIC1_THERMAL_STAGEV4_C1_SUBPAGE_MAX_AGE_MS)
    #error "V4-C1 pair grace age must be greater than the normal max-age threshold."
#endif

void redpic1_thermal_init(void);
void redpic1_thermal_bind_display_runtime(void);
void redpic1_thermal_step(void);
uint32_t redpic1_thermal_get_active_period_ms(void);
void redpic1_thermal_force_refresh(void);
void redpic1_thermal_handle_key(uint8_t key_value);
void redpic1_thermal_render_runtime_overlay(void);
uint8_t redpic1_thermal_runtime_overlay_visible(void);
void redpic1_thermal_suspend(void);
void redpic1_thermal_resume(void);
void redpic1_thermal_restore_bus_after_stop(void);
void redpic1_thermal_set_overlay_hold(uint8_t enabled);

#endif
