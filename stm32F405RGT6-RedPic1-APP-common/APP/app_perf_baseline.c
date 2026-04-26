#include "app_perf_baseline.h"

#include <string.h>

#include "sys.h"

typedef struct
{
    uint32_t last;
    uint32_t min;
    uint32_t max;
    uint32_t count;
    uint64_t total;
} app_perf_stat_accum_t;

static volatile uint32_t s_thermal_capture_frames = 0U;
static volatile uint32_t s_thermal_display_frames = 0U;
static volatile uint32_t s_thermal_capture_failures = 0U;
static volatile uint32_t s_thermal_fps = 0U;
static volatile uint32_t s_thermal_display_fps = 0U;
static volatile uint32_t s_last_capture_tick_ms = 0U;
static volatile uint32_t s_fps_window_start_ms = 0U;
static volatile uint32_t s_fps_window_count = 0U;
static volatile uint32_t s_display_fps_window_start_ms = 0U;
static volatile uint32_t s_display_fps_window_count = 0U;

static volatile float s_latest_min_temp = 0.0f;
static volatile float s_latest_max_temp = 0.0f;
static volatile float s_latest_center_temp = 0.0f;

static volatile uint32_t s_key_queue_drop_count = 0U;
static volatile uint32_t s_ui_msg_drop_count = 0U;
static volatile uint32_t s_service_queue_fail_count = 0U;
static volatile uint32_t s_display_queue_fail_count = 0U;

static volatile uint32_t s_input_notify_count = 0U;
static volatile uint32_t s_ui_notify_count = 0U;
static volatile uint32_t s_service_notify_count = 0U;
static volatile uint32_t s_display_notify_count = 0U;

static volatile uint32_t s_uart_error_count = 0U;
static volatile uint32_t s_last_uart_error_flags = 0U;
static volatile uint32_t s_i2c_failure_count = 0U;
static volatile uint32_t s_i2c_af_count = 0U;
static volatile uint32_t s_i2c_berr_count = 0U;
static volatile uint32_t s_i2c_arlo_count = 0U;
static volatile uint32_t s_i2c_ovr_count = 0U;
static volatile uint32_t s_i2c_timeout_count = 0U;
static volatile uint32_t s_i2c_busy_stuck_count = 0U;
static volatile uint32_t s_i2c_dma_err_count = 0U;
static volatile uint32_t s_i2c_dma_timeout_ndtr = 0U;
static volatile uint32_t s_i2c_dma_timeout_state = 0U;
static volatile uint32_t s_i2c_dma_timeout_sr1 = 0U;
static volatile uint32_t s_i2c_dma_timeout_sr2 = 0U;
static volatile uint32_t s_i2c_dma_tc_ndtr = 0U;
static volatile uint32_t s_i2c_dma_tc_state = 0U;
static volatile uint32_t s_i2c_dma_tc_sr1 = 0U;
static volatile uint32_t s_i2c_dma_tc_sr2 = 0U;
static volatile uint32_t s_i2c_dma_ev_irq_count = 0U;
static volatile uint32_t s_i2c_dma_tc_irq_count = 0U;
static volatile uint32_t s_i2c_dma_wait_timeout_count = 0U;
static volatile uint32_t s_i2c_poll_event_timeout_count = 0U;
static volatile uint32_t s_i2c_poll_busy_timeout_count = 0U;
static volatile uint32_t s_i2c_er_timeout_count = 0U;
static volatile uint32_t s_i2c_poll_timeout_path = 0U;
static volatile uint32_t s_i2c_poll_timeout_phase = 0U;
static volatile uint32_t s_i2c_poll_timeout_event = 0U;
static volatile uint32_t s_i2c_poll_timeout_sr1 = 0U;
static volatile uint32_t s_i2c_poll_timeout_sr2 = 0U;
static volatile uint32_t s_i2c_poll_timeout_start_addr = 0U;
static volatile uint32_t s_i2c_poll_timeout_word_count = 0U;
static volatile uint32_t s_dma_timeout_count = 0U;
static volatile uint32_t s_thermal_backoff_count = 0U;
static volatile uint32_t s_thermal_pair_timeout_count = 0U;
static volatile uint32_t s_thermal_pair_compose_ok_count = 0U;
static volatile uint32_t s_thermal_pair_wait_other_count = 0U;
static volatile uint32_t s_thermal_pair_last_result = APP_PERF_THERMAL_PAIR_RESULT_NONE;
static volatile uint32_t s_thermal_pair_last_subpage = 0xFFU;
static volatile uint32_t s_thermal_pair_last_missing_subpage = 0xFFU;
static volatile uint32_t s_thermal_pair_last_gap_ms = 0U;
static volatile uint32_t s_thermal_pair_timeout_gap_last_ms = 0U;
static volatile uint32_t s_thermal_pair_timeout_gap_max_ms = 0U;
static volatile uint32_t s_thermal_pair_compose_gap_last_ms = 0U;
static volatile uint32_t s_thermal_pair_compose_gap_max_ms = 0U;
static volatile uint32_t s_thermal_pair_same_subpage_streak_last = 0U;
static volatile uint32_t s_thermal_pair_same_subpage_streak_max = 0U;
static volatile uint32_t s_thermal_pair_timeout_get_temp_last_us = 0U;
static volatile uint32_t s_thermal_pair_timeout_step_last_us = 0U;
static volatile uint32_t s_thermal_ready_replace_count = 0U;
static volatile uint32_t s_thermal_display_cancel_count = 0U;
static volatile uint32_t s_thermal_3d_sync_present_attempt_count = 0U;
static volatile uint32_t s_thermal_3d_sync_present_ok_count = 0U;
static volatile uint32_t s_thermal_3d_sync_present_fail_count = 0U;
static volatile uint32_t s_thermal_3d_claim_count = 0U;
static volatile uint32_t s_thermal_3d_done_ok_count = 0U;
static volatile uint32_t s_thermal_3d_done_error_count = 0U;
static volatile uint32_t s_thermal_3d_done_cancel_count = 0U;
static volatile uint32_t s_thermal_3d_wait_timeout_count = 0U;
static volatile uint32_t s_lcd_dma_enter_count = 0U;
static volatile uint32_t s_dma_irq_tc_count = 0U;
static volatile uint32_t s_dma_irq_te_count = 0U;
static volatile uint32_t s_dma_wait_take_count = 0U;
static volatile uint8_t s_last_dma_ok = 0U;
static volatile uint8_t s_last_dma_status = APP_PERF_LCD_DMA_STATUS_NONE;

static volatile uint32_t s_watchdog_missing_progress_mask = 0U;
static volatile uint32_t s_watchdog_fault_flags = 0U;

static volatile uint8_t s_thermal_active = 0U;
static volatile uint8_t s_screen_off = 0U;
static volatile power_state_t s_power_state = POWER_STATE_ACTIVE_UI;
static volatile clock_profile_t s_clock_profile = CLOCK_PROFILE_HIGH;

static volatile UBaseType_t s_input_stack_words = 0U;
static volatile UBaseType_t s_service_stack_words = 0U;
static volatile UBaseType_t s_ui_stack_words = 0U;
static volatile UBaseType_t s_display_stack_words = 0U;
static volatile UBaseType_t s_thermal_stack_words = 0U;
static volatile UBaseType_t s_power_stack_words = 0U;

static app_perf_stat_accum_t s_frame_period_stats;
static app_perf_stat_accum_t s_thermal_display_age_stats;
static app_perf_stat_accum_t s_get_temp_stats;
static app_perf_stat_accum_t s_gray_stats;
static app_perf_stat_accum_t s_thermal_step_stats;
static app_perf_stat_accum_t s_lcd_dma_stats;
static app_perf_stat_accum_t s_lcd_dma_render_stats;
static app_perf_stat_accum_t s_lcd_dma_start_stats;
static app_perf_stat_accum_t s_lcd_dma_wait_stats;
static app_perf_stat_accum_t s_lcd_dma_spi_idle_stats;
static app_perf_stat_accum_t s_lcd_dma_overlay_stats;

static uint8_t app_perf_baseline_scheduler_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;
}

static void app_perf_stat_reset(app_perf_stat_accum_t *stat)
{
    if (stat == 0)
    {
        return;
    }

    memset(stat, 0, sizeof(*stat));
}

static void app_perf_stat_add(app_perf_stat_accum_t *stat, uint32_t value)
{
    if (stat == 0)
    {
        return;
    }

    stat->last = value;
    if (stat->count == 0U || value < stat->min)
    {
        stat->min = value;
    }
    if (value > stat->max)
    {
        stat->max = value;
    }

    stat->count++;
    stat->total += value;
}

static uint32_t app_perf_stat_avg(const app_perf_stat_accum_t *stat)
{
    if (stat == 0 || stat->count == 0U)
    {
        return 0U;
    }

    return (uint32_t)(stat->total / stat->count);
}

static uint32_t app_perf_cycles_to_us(uint32_t cycles)
{
    uint32_t cycles_per_us = SystemCoreClock / 1000000UL;

    if (cycles_per_us == 0U)
    {
        return 0U;
    }

    return cycles / cycles_per_us;
}

void app_perf_baseline_init(void)
{
    app_perf_baseline_reset();

#if APP_PERF_BASELINE_ENABLE
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
}

void app_perf_baseline_reset(void)
{
    if (app_perf_baseline_scheduler_running() != 0U)
    {
        taskENTER_CRITICAL();
    }

    s_thermal_capture_frames = 0U;
    s_thermal_display_frames = 0U;
    s_thermal_capture_failures = 0U;
    s_thermal_fps = 0U;
    s_thermal_display_fps = 0U;
    s_last_capture_tick_ms = 0U;
    s_fps_window_start_ms = 0U;
    s_fps_window_count = 0U;
    s_display_fps_window_start_ms = 0U;
    s_display_fps_window_count = 0U;

    s_latest_min_temp = 0.0f;
    s_latest_max_temp = 0.0f;
    s_latest_center_temp = 0.0f;

    s_key_queue_drop_count = 0U;
    s_ui_msg_drop_count = 0U;
    s_service_queue_fail_count = 0U;
    s_display_queue_fail_count = 0U;

    s_input_notify_count = 0U;
    s_ui_notify_count = 0U;
    s_service_notify_count = 0U;
    s_display_notify_count = 0U;

    s_uart_error_count = 0U;
    s_last_uart_error_flags = 0U;
    s_i2c_failure_count = 0U;
    s_i2c_af_count = 0U;
    s_i2c_berr_count = 0U;
    s_i2c_arlo_count = 0U;
    s_i2c_ovr_count = 0U;
    s_i2c_timeout_count = 0U;
    s_i2c_busy_stuck_count = 0U;
    s_i2c_dma_err_count = 0U;
    s_i2c_dma_timeout_ndtr = 0U;
    s_i2c_dma_timeout_state = 0U;
    s_i2c_dma_timeout_sr1 = 0U;
    s_i2c_dma_timeout_sr2 = 0U;
    s_i2c_dma_tc_ndtr = 0U;
    s_i2c_dma_tc_state = 0U;
    s_i2c_dma_tc_sr1 = 0U;
    s_i2c_dma_tc_sr2 = 0U;
    s_i2c_dma_ev_irq_count = 0U;
    s_i2c_dma_tc_irq_count = 0U;
    s_i2c_dma_wait_timeout_count = 0U;
    s_i2c_poll_event_timeout_count = 0U;
    s_i2c_poll_busy_timeout_count = 0U;
    s_i2c_er_timeout_count = 0U;
    s_i2c_poll_timeout_path = 0U;
    s_i2c_poll_timeout_phase = 0U;
    s_i2c_poll_timeout_event = 0U;
    s_i2c_poll_timeout_sr1 = 0U;
    s_i2c_poll_timeout_sr2 = 0U;
    s_i2c_poll_timeout_start_addr = 0U;
    s_i2c_poll_timeout_word_count = 0U;
    s_dma_timeout_count = 0U;
    s_thermal_backoff_count = 0U;
    s_thermal_pair_timeout_count = 0U;
    s_thermal_pair_compose_ok_count = 0U;
    s_thermal_pair_wait_other_count = 0U;
    s_thermal_pair_last_result = APP_PERF_THERMAL_PAIR_RESULT_NONE;
    s_thermal_pair_last_subpage = 0xFFU;
    s_thermal_pair_last_missing_subpage = 0xFFU;
    s_thermal_pair_last_gap_ms = 0U;
    s_thermal_pair_timeout_gap_last_ms = 0U;
    s_thermal_pair_timeout_gap_max_ms = 0U;
    s_thermal_pair_compose_gap_last_ms = 0U;
    s_thermal_pair_compose_gap_max_ms = 0U;
    s_thermal_pair_same_subpage_streak_last = 0U;
    s_thermal_pair_same_subpage_streak_max = 0U;
    s_thermal_pair_timeout_get_temp_last_us = 0U;
    s_thermal_pair_timeout_step_last_us = 0U;
    s_thermal_ready_replace_count = 0U;
    s_thermal_display_cancel_count = 0U;
    s_thermal_3d_sync_present_attempt_count = 0U;
    s_thermal_3d_sync_present_ok_count = 0U;
    s_thermal_3d_sync_present_fail_count = 0U;
    s_thermal_3d_claim_count = 0U;
    s_thermal_3d_done_ok_count = 0U;
    s_thermal_3d_done_error_count = 0U;
    s_thermal_3d_done_cancel_count = 0U;
    s_thermal_3d_wait_timeout_count = 0U;
    s_lcd_dma_enter_count = 0U;
    s_dma_irq_tc_count = 0U;
    s_dma_irq_te_count = 0U;
    s_dma_wait_take_count = 0U;
    s_last_dma_ok = 0U;
    s_last_dma_status = APP_PERF_LCD_DMA_STATUS_NONE;

    s_watchdog_missing_progress_mask = 0U;
    s_watchdog_fault_flags = 0U;

    s_thermal_active = 0U;
    s_screen_off = 0U;
    s_power_state = POWER_STATE_ACTIVE_UI;
    s_clock_profile = CLOCK_PROFILE_HIGH;

    s_input_stack_words = 0U;
    s_service_stack_words = 0U;
    s_ui_stack_words = 0U;
    s_display_stack_words = 0U;
    s_thermal_stack_words = 0U;
    s_power_stack_words = 0U;

    app_perf_stat_reset(&s_frame_period_stats);
    app_perf_stat_reset(&s_thermal_display_age_stats);
    app_perf_stat_reset(&s_get_temp_stats);
    app_perf_stat_reset(&s_gray_stats);
    app_perf_stat_reset(&s_thermal_step_stats);
    app_perf_stat_reset(&s_lcd_dma_stats);
    app_perf_stat_reset(&s_lcd_dma_render_stats);
    app_perf_stat_reset(&s_lcd_dma_start_stats);
    app_perf_stat_reset(&s_lcd_dma_wait_stats);
    app_perf_stat_reset(&s_lcd_dma_spi_idle_stats);
    app_perf_stat_reset(&s_lcd_dma_overlay_stats);

    if (app_perf_baseline_scheduler_running() != 0U)
    {
        taskEXIT_CRITICAL();
    }
}

uint8_t app_perf_baseline_is_enabled(void)
{
#if APP_PERF_BASELINE_ENABLE
    return 1U;
#else
    return 0U;
#endif
}

uint32_t app_perf_baseline_cycle_now(void)
{
#if APP_PERF_BASELINE_ENABLE
    return DWT->CYCCNT;
#else
    return 0U;
#endif
}

uint32_t app_perf_baseline_elapsed_us(uint32_t start_cycle)
{
#if APP_PERF_BASELINE_ENABLE
    return app_perf_cycles_to_us(app_perf_baseline_cycle_now() - start_cycle);
#else
    (void)start_cycle;
    return 0U;
#endif
}

void app_perf_baseline_record_thermal_capture_success(uint32_t capture_tick_ms,
                                                      float min_temp,
                                                      float max_temp,
                                                      float center_temp)
{
#if APP_PERF_BASELINE_ENABLE
    uint32_t elapsed_ms = 0U;

    s_thermal_capture_frames++;

    if (s_last_capture_tick_ms != 0U && capture_tick_ms >= s_last_capture_tick_ms)
    {
        elapsed_ms = capture_tick_ms - s_last_capture_tick_ms;
        app_perf_stat_add(&s_frame_period_stats, elapsed_ms);
    }
    s_last_capture_tick_ms = capture_tick_ms;

    s_latest_min_temp = min_temp;
    s_latest_max_temp = max_temp;
    s_latest_center_temp = center_temp;

    if (s_fps_window_start_ms == 0U)
    {
        s_fps_window_start_ms = capture_tick_ms;
        s_fps_window_count = 1U;
    }
    else
    {
        s_fps_window_count++;
        if ((capture_tick_ms - s_fps_window_start_ms) >= 1000UL)
        {
            uint32_t window_ms = capture_tick_ms - s_fps_window_start_ms;

            if (window_ms != 0U)
            {
                s_thermal_fps = (s_fps_window_count * 1000UL) / window_ms;
            }

            s_fps_window_start_ms = capture_tick_ms;
            s_fps_window_count = 0U;
        }
    }
#else
    (void)capture_tick_ms;
    (void)min_temp;
    (void)max_temp;
    (void)center_temp;
#endif
}

void app_perf_baseline_record_thermal_capture_failure(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_capture_failures++;
#endif
}

void app_perf_baseline_record_thermal_display_age_ms(uint32_t elapsed_ms)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_thermal_display_age_stats, elapsed_ms);
#else
    (void)elapsed_ms;
#endif
}

void app_perf_baseline_record_get_temp_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_get_temp_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

void app_perf_baseline_record_gray_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_gray_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

void app_perf_baseline_record_thermal_step_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_thermal_step_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

void app_perf_baseline_record_lcd_dma_result(uint32_t elapsed_us,
                                             app_perf_lcd_dma_status_t status)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_stats, elapsed_us);
    s_last_dma_status = (uint8_t)status;
    s_last_dma_ok = (status == APP_PERF_LCD_DMA_STATUS_OK) ? 1U : 0U;

    if (status == APP_PERF_LCD_DMA_STATUS_OK)
    {
        uint32_t display_tick_ms = power_manager_get_tick_ms();

        s_thermal_display_frames++;
        if (s_display_fps_window_start_ms == 0U)
        {
            s_display_fps_window_start_ms = display_tick_ms;
            s_display_fps_window_count = 1U;
        }
        else
        {
            s_display_fps_window_count++;
            if ((display_tick_ms - s_display_fps_window_start_ms) >= 1000UL)
            {
                uint32_t window_ms = display_tick_ms - s_display_fps_window_start_ms;

                if (window_ms != 0U)
                {
                    s_thermal_display_fps = (s_display_fps_window_count * 1000UL) / window_ms;
                }

                s_display_fps_window_start_ms = display_tick_ms;
                s_display_fps_window_count = 0U;
            }
        }
    }
    else
    {
        s_dma_timeout_count++;
    }
#else
    (void)elapsed_us;
    (void)status;
#endif
}

void app_perf_baseline_record_lcd_dma_render_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_render_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

void app_perf_baseline_record_lcd_dma_start_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_start_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

void app_perf_baseline_record_lcd_dma_wait_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_wait_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

void app_perf_baseline_record_lcd_dma_spi_idle_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_spi_idle_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

void app_perf_baseline_record_lcd_dma_overlay_us(uint32_t elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    app_perf_stat_add(&s_lcd_dma_overlay_stats, elapsed_us);
#else
    (void)elapsed_us;
#endif
}

void app_perf_baseline_record_task_notify(app_perf_notify_target_t target)
{
#if APP_PERF_BASELINE_ENABLE
    switch (target)
    {
    case APP_PERF_NOTIFY_INPUT:
        s_input_notify_count++;
        break;

    case APP_PERF_NOTIFY_DISPLAY:
        s_display_notify_count++;
        break;

    case APP_PERF_NOTIFY_SERVICE:
        s_service_notify_count++;
        break;

    case APP_PERF_NOTIFY_UI:
    default:
        s_ui_notify_count++;
        break;
    }
#else
    (void)target;
#endif
}

void app_perf_baseline_record_key_queue_drop(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_key_queue_drop_count++;
#endif
}

void app_perf_baseline_record_ui_msg_drop(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_ui_msg_drop_count++;
#endif
}

void app_perf_baseline_record_service_queue_fail(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_service_queue_fail_count++;
#endif
}

void app_perf_baseline_record_display_queue_fail(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_display_queue_fail_count++;
#endif
}

void app_perf_baseline_record_uart_errors(uint32_t flags)
{
#if APP_PERF_BASELINE_ENABLE
    if (flags != 0U)
    {
        s_uart_error_count++;
        s_last_uart_error_flags = flags;
    }
#else
    (void)flags;
#endif
}

void app_perf_baseline_record_i2c_failure(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_failure_count++;
#endif
}

void app_perf_baseline_record_i2c_transport_error(app_perf_i2c_error_t error_kind)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_failure_count++;

    switch (error_kind)
    {
    case APP_PERF_I2C_ERROR_AF:
        s_i2c_af_count++;
        break;

    case APP_PERF_I2C_ERROR_BERR:
        s_i2c_berr_count++;
        break;

    case APP_PERF_I2C_ERROR_ARLO:
        s_i2c_arlo_count++;
        break;

    case APP_PERF_I2C_ERROR_OVR:
        s_i2c_ovr_count++;
        break;

    case APP_PERF_I2C_ERROR_BUSY_STUCK:
        s_i2c_busy_stuck_count++;
        break;

    case APP_PERF_I2C_ERROR_DMA_ERR:
        s_i2c_dma_err_count++;
        break;

    case APP_PERF_I2C_ERROR_TIMEOUT:
    default:
        s_i2c_timeout_count++;
        break;
    }
#else
    (void)error_kind;
#endif
}

void app_perf_baseline_record_i2c_dma_timeout_snapshot(uint16_t ndtr,
                                                       uint8_t state,
                                                       uint32_t sr1,
                                                       uint32_t sr2)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_timeout_ndtr = (uint32_t)ndtr;
    s_i2c_dma_timeout_state = (uint32_t)state;
    s_i2c_dma_timeout_sr1 = sr1;
    s_i2c_dma_timeout_sr2 = sr2;
#else
    (void)ndtr;
    (void)state;
    (void)sr1;
    (void)sr2;
#endif
}

void app_perf_baseline_record_i2c_dma_tc_snapshot(uint16_t ndtr,
                                                  uint8_t state,
                                                  uint32_t sr1,
                                                  uint32_t sr2)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_tc_ndtr = (uint32_t)ndtr;
    s_i2c_dma_tc_state = (uint32_t)state;
    s_i2c_dma_tc_sr1 = sr1;
    s_i2c_dma_tc_sr2 = sr2;
#else
    (void)ndtr;
    (void)state;
    (void)sr1;
    (void)sr2;
#endif
}

void app_perf_baseline_record_i2c_dma_ev_irq(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_ev_irq_count++;
#endif
}

void app_perf_baseline_record_i2c_dma_tc_irq(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_tc_irq_count++;
#endif
}

void app_perf_baseline_record_i2c_dma_wait_timeout(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_dma_wait_timeout_count++;
#endif
}

void app_perf_baseline_record_i2c_poll_event_timeout(app_perf_i2c_poll_path_t path,
                                                     app_perf_i2c_poll_phase_t phase,
                                                     uint32_t event,
                                                     uint16_t start_addr,
                                                     uint16_t word_count,
                                                     uint32_t sr1,
                                                     uint32_t sr2)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_poll_event_timeout_count++;
    s_i2c_poll_timeout_path = (uint32_t)path;
    s_i2c_poll_timeout_phase = (uint32_t)phase;
    s_i2c_poll_timeout_event = event;
    s_i2c_poll_timeout_sr1 = sr1;
    s_i2c_poll_timeout_sr2 = sr2;
    s_i2c_poll_timeout_start_addr = (uint32_t)start_addr;
    s_i2c_poll_timeout_word_count = (uint32_t)word_count;
#else
    (void)path;
    (void)phase;
    (void)event;
    (void)start_addr;
    (void)word_count;
    (void)sr1;
    (void)sr2;
#endif
}

void app_perf_baseline_record_i2c_poll_busy_timeout(app_perf_i2c_poll_path_t path,
                                                    uint16_t start_addr,
                                                    uint16_t word_count,
                                                    uint32_t sr1,
                                                    uint32_t sr2)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_poll_busy_timeout_count++;
    s_i2c_poll_timeout_path = (uint32_t)path;
    s_i2c_poll_timeout_phase = (uint32_t)APP_PERF_I2C_POLL_PHASE_WAIT_BUSY;
    s_i2c_poll_timeout_event = 0U;
    s_i2c_poll_timeout_sr1 = sr1;
    s_i2c_poll_timeout_sr2 = sr2;
    s_i2c_poll_timeout_start_addr = (uint32_t)start_addr;
    s_i2c_poll_timeout_word_count = (uint32_t)word_count;
#else
    (void)path;
    (void)start_addr;
    (void)word_count;
    (void)sr1;
    (void)sr2;
#endif
}

void app_perf_baseline_record_i2c_er_timeout(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_i2c_er_timeout_count++;
#endif
}

void app_perf_baseline_record_thermal_backoff(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_backoff_count++;
#endif
}

void app_perf_baseline_record_thermal_pair_timeout(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_pair_timeout_count++;
    s_thermal_pair_last_result = APP_PERF_THERMAL_PAIR_RESULT_TIMEOUT;
#endif
}

void app_perf_baseline_record_thermal_pair_wait_other(uint8_t subpage,
                                                      uint8_t missing_subpage,
                                                      uint32_t gap_ms,
                                                      uint32_t same_subpage_streak)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_pair_wait_other_count++;
    s_thermal_pair_last_result = APP_PERF_THERMAL_PAIR_RESULT_WAIT_OTHER;
    s_thermal_pair_last_subpage = (uint32_t)subpage;
    s_thermal_pair_last_missing_subpage = (uint32_t)missing_subpage;
    s_thermal_pair_last_gap_ms = gap_ms;
    s_thermal_pair_same_subpage_streak_last = same_subpage_streak;
    if (same_subpage_streak > s_thermal_pair_same_subpage_streak_max)
    {
        s_thermal_pair_same_subpage_streak_max = same_subpage_streak;
    }
#else
    (void)subpage;
    (void)missing_subpage;
    (void)gap_ms;
    (void)same_subpage_streak;
#endif
}

void app_perf_baseline_record_thermal_pair_timeout_detail(uint8_t subpage,
                                                          uint8_t missing_subpage,
                                                          uint32_t gap_ms,
                                                          uint32_t same_subpage_streak,
                                                          uint32_t get_temp_elapsed_us,
                                                          uint32_t step_elapsed_us)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_pair_timeout_count++;
    s_thermal_pair_last_result = APP_PERF_THERMAL_PAIR_RESULT_TIMEOUT;
    s_thermal_pair_last_subpage = (uint32_t)subpage;
    s_thermal_pair_last_missing_subpage = (uint32_t)missing_subpage;
    s_thermal_pair_last_gap_ms = gap_ms;
    s_thermal_pair_timeout_gap_last_ms = gap_ms;
    if (gap_ms > s_thermal_pair_timeout_gap_max_ms)
    {
        s_thermal_pair_timeout_gap_max_ms = gap_ms;
    }
    s_thermal_pair_same_subpage_streak_last = same_subpage_streak;
    if (same_subpage_streak > s_thermal_pair_same_subpage_streak_max)
    {
        s_thermal_pair_same_subpage_streak_max = same_subpage_streak;
    }
    s_thermal_pair_timeout_get_temp_last_us = get_temp_elapsed_us;
    s_thermal_pair_timeout_step_last_us = step_elapsed_us;
#else
    (void)subpage;
    (void)missing_subpage;
    (void)gap_ms;
    (void)same_subpage_streak;
    (void)get_temp_elapsed_us;
    (void)step_elapsed_us;
#endif
}

void app_perf_baseline_record_thermal_pair_compose_ok(uint8_t subpage,
                                                      uint8_t other_subpage,
                                                      uint32_t gap_ms,
                                                      uint32_t same_subpage_streak)
{
#if APP_PERF_BASELINE_ENABLE
    (void)other_subpage;
    s_thermal_pair_compose_ok_count++;
    s_thermal_pair_last_result = APP_PERF_THERMAL_PAIR_RESULT_COMPOSE_OK;
    s_thermal_pair_last_subpage = (uint32_t)subpage;
    s_thermal_pair_last_missing_subpage = 0xFFU;
    s_thermal_pair_last_gap_ms = gap_ms;
    s_thermal_pair_compose_gap_last_ms = gap_ms;
    if (gap_ms > s_thermal_pair_compose_gap_max_ms)
    {
        s_thermal_pair_compose_gap_max_ms = gap_ms;
    }
    s_thermal_pair_same_subpage_streak_last = same_subpage_streak;
    if (same_subpage_streak > s_thermal_pair_same_subpage_streak_max)
    {
        s_thermal_pair_same_subpage_streak_max = same_subpage_streak;
    }
#else
    (void)subpage;
    (void)other_subpage;
    (void)gap_ms;
    (void)same_subpage_streak;
#endif
}

void app_perf_baseline_record_thermal_ready_replace(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_ready_replace_count++;
#endif
}

void app_perf_baseline_record_thermal_display_cancel(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_display_cancel_count++;
#endif
}

void app_perf_baseline_record_thermal_3d_sync_present_attempt(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_sync_present_attempt_count++;
#endif
}

void app_perf_baseline_record_thermal_3d_sync_present_ok(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_sync_present_ok_count++;
#endif
}

void app_perf_baseline_record_thermal_3d_sync_present_fail(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_sync_present_fail_count++;
#endif
}

void app_perf_baseline_record_thermal_3d_claim(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_claim_count++;
#endif
}

void app_perf_baseline_record_thermal_3d_done_ok(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_done_ok_count++;
#endif
}

void app_perf_baseline_record_thermal_3d_done_error(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_done_error_count++;
#endif
}

void app_perf_baseline_record_thermal_3d_done_cancel(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_done_cancel_count++;
#endif
}

void app_perf_baseline_record_thermal_3d_wait_timeout(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_thermal_3d_wait_timeout_count++;
#endif
}

void app_perf_baseline_record_lcd_dma_enter(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_lcd_dma_enter_count++;
#endif
}

void app_perf_baseline_record_dma_irq_tc(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_dma_irq_tc_count++;
#endif
}

void app_perf_baseline_record_dma_irq_te(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_dma_irq_te_count++;
#endif
}

void app_perf_baseline_record_dma_wait_take(void)
{
#if APP_PERF_BASELINE_ENABLE
    s_dma_wait_take_count++;
#endif
}

void app_perf_baseline_set_watchdog_snapshot(uint32_t missing_progress_mask,
                                             uint32_t fault_flags)
{
#if APP_PERF_BASELINE_ENABLE
    s_watchdog_missing_progress_mask = missing_progress_mask;
    s_watchdog_fault_flags = fault_flags;
#else
    (void)missing_progress_mask;
    (void)fault_flags;
#endif
}

void app_perf_baseline_set_runtime_state(power_state_t power_state,
                                         clock_profile_t clock_profile,
                                         uint8_t thermal_active,
                                         uint8_t screen_off)
{
#if APP_PERF_BASELINE_ENABLE
    s_power_state = power_state;
    s_clock_profile = clock_profile;
    s_thermal_active = (thermal_active != 0U) ? 1U : 0U;
    s_screen_off = (screen_off != 0U) ? 1U : 0U;
#else
    (void)power_state;
    (void)clock_profile;
    (void)thermal_active;
    (void)screen_off;
#endif
}

void app_perf_baseline_refresh_task_stacks(TaskHandle_t input_task,
                                           TaskHandle_t service_task,
                                           TaskHandle_t ui_task,
                                           TaskHandle_t display_task,
                                           TaskHandle_t thermal_task,
                                           TaskHandle_t power_task)
{
#if APP_PERF_BASELINE_ENABLE
    if (app_perf_baseline_scheduler_running() == 0U)
    {
        return;
    }

    if (input_task != 0)
    {
        s_input_stack_words = uxTaskGetStackHighWaterMark(input_task);
    }
    if (service_task != 0)
    {
        s_service_stack_words = uxTaskGetStackHighWaterMark(service_task);
    }
    if (ui_task != 0)
    {
        s_ui_stack_words = uxTaskGetStackHighWaterMark(ui_task);
    }
    if (display_task != 0)
    {
        s_display_stack_words = uxTaskGetStackHighWaterMark(display_task);
    }
    if (thermal_task != 0)
    {
        s_thermal_stack_words = uxTaskGetStackHighWaterMark(thermal_task);
    }
    if (power_task != 0)
    {
        s_power_stack_words = uxTaskGetStackHighWaterMark(power_task);
    }
#else
    (void)input_task;
    (void)service_task;
    (void)ui_task;
    (void)display_task;
    (void)thermal_task;
    (void)power_task;
#endif
}

void app_perf_baseline_get_snapshot(app_perf_baseline_snapshot_t *snapshot)
{
    if (snapshot == 0)
    {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->enabled = app_perf_baseline_is_enabled();

#if APP_PERF_BASELINE_ENABLE
    if (app_perf_baseline_scheduler_running() != 0U)
    {
        taskENTER_CRITICAL();
    }

    snapshot->last_dma_ok = s_last_dma_ok;
    snapshot->last_dma_status = s_last_dma_status;
    snapshot->thermal_active = s_thermal_active;
    snapshot->screen_off = s_screen_off;
    snapshot->power_state = s_power_state;
    snapshot->clock_profile = s_clock_profile;

    snapshot->thermal_capture_frames = s_thermal_capture_frames;
    snapshot->thermal_display_frames = s_thermal_display_frames;
    snapshot->thermal_capture_failures = s_thermal_capture_failures;
    snapshot->thermal_fps = s_thermal_fps;
    snapshot->thermal_display_fps = s_thermal_display_fps;
    snapshot->thermal_display_age_samples = s_thermal_display_age_stats.count;
    snapshot->thermal_display_age_last_ms = s_thermal_display_age_stats.last;
    snapshot->thermal_display_age_max_ms = s_thermal_display_age_stats.max;
    snapshot->thermal_display_age_avg_ms = app_perf_stat_avg(&s_thermal_display_age_stats);

    snapshot->thermal_frame_period_samples = s_frame_period_stats.count;
    snapshot->thermal_frame_period_last_ms = s_frame_period_stats.last;
    snapshot->thermal_frame_period_min_ms = s_frame_period_stats.min;
    snapshot->thermal_frame_period_max_ms = s_frame_period_stats.max;
    snapshot->thermal_frame_period_avg_ms = app_perf_stat_avg(&s_frame_period_stats);

    snapshot->get_temp_samples = s_get_temp_stats.count;
    snapshot->get_temp_last_us = s_get_temp_stats.last;
    snapshot->get_temp_max_us = s_get_temp_stats.max;
    snapshot->get_temp_avg_us = app_perf_stat_avg(&s_get_temp_stats);

    snapshot->gray_samples = s_gray_stats.count;
    snapshot->gray_last_us = s_gray_stats.last;
    snapshot->gray_max_us = s_gray_stats.max;
    snapshot->gray_avg_us = app_perf_stat_avg(&s_gray_stats);

    snapshot->thermal_step_samples = s_thermal_step_stats.count;
    snapshot->thermal_step_last_us = s_thermal_step_stats.last;
    snapshot->thermal_step_max_us = s_thermal_step_stats.max;
    snapshot->thermal_step_avg_us = app_perf_stat_avg(&s_thermal_step_stats);

    snapshot->lcd_dma_samples = s_lcd_dma_stats.count;
    snapshot->lcd_dma_last_us = s_lcd_dma_stats.last;
    snapshot->lcd_dma_max_us = s_lcd_dma_stats.max;
    snapshot->lcd_dma_avg_us = app_perf_stat_avg(&s_lcd_dma_stats);
    snapshot->lcd_dma_render_samples = s_lcd_dma_render_stats.count;
    snapshot->lcd_dma_render_last_us = s_lcd_dma_render_stats.last;
    snapshot->lcd_dma_render_max_us = s_lcd_dma_render_stats.max;
    snapshot->lcd_dma_render_avg_us = app_perf_stat_avg(&s_lcd_dma_render_stats);
    snapshot->lcd_dma_start_samples = s_lcd_dma_start_stats.count;
    snapshot->lcd_dma_start_last_us = s_lcd_dma_start_stats.last;
    snapshot->lcd_dma_start_max_us = s_lcd_dma_start_stats.max;
    snapshot->lcd_dma_start_avg_us = app_perf_stat_avg(&s_lcd_dma_start_stats);
    snapshot->lcd_dma_wait_samples = s_lcd_dma_wait_stats.count;
    snapshot->lcd_dma_wait_last_us = s_lcd_dma_wait_stats.last;
    snapshot->lcd_dma_wait_max_us = s_lcd_dma_wait_stats.max;
    snapshot->lcd_dma_wait_avg_us = app_perf_stat_avg(&s_lcd_dma_wait_stats);
    snapshot->lcd_dma_spi_idle_samples = s_lcd_dma_spi_idle_stats.count;
    snapshot->lcd_dma_spi_idle_last_us = s_lcd_dma_spi_idle_stats.last;
    snapshot->lcd_dma_spi_idle_max_us = s_lcd_dma_spi_idle_stats.max;
    snapshot->lcd_dma_spi_idle_avg_us = app_perf_stat_avg(&s_lcd_dma_spi_idle_stats);
    snapshot->lcd_dma_overlay_samples = s_lcd_dma_overlay_stats.count;
    snapshot->lcd_dma_overlay_last_us = s_lcd_dma_overlay_stats.last;
    snapshot->lcd_dma_overlay_max_us = s_lcd_dma_overlay_stats.max;
    snapshot->lcd_dma_overlay_avg_us = app_perf_stat_avg(&s_lcd_dma_overlay_stats);

    snapshot->latest_min_temp = s_latest_min_temp;
    snapshot->latest_max_temp = s_latest_max_temp;
    snapshot->latest_center_temp = s_latest_center_temp;

    snapshot->key_queue_drop_count = s_key_queue_drop_count;
    snapshot->ui_msg_drop_count = s_ui_msg_drop_count;
    snapshot->service_queue_fail_count = s_service_queue_fail_count;
    snapshot->display_queue_fail_count = s_display_queue_fail_count;

    snapshot->input_notify_count = s_input_notify_count;
    snapshot->ui_notify_count = s_ui_notify_count;
    snapshot->service_notify_count = s_service_notify_count;
    snapshot->display_notify_count = s_display_notify_count;

    snapshot->uart_error_count = s_uart_error_count;
    snapshot->last_uart_error_flags = s_last_uart_error_flags;
    snapshot->i2c_failure_count = s_i2c_failure_count;
    snapshot->i2c_af_count = s_i2c_af_count;
    snapshot->i2c_berr_count = s_i2c_berr_count;
    snapshot->i2c_arlo_count = s_i2c_arlo_count;
    snapshot->i2c_ovr_count = s_i2c_ovr_count;
    snapshot->i2c_timeout_count = s_i2c_timeout_count;
    snapshot->i2c_busy_stuck_count = s_i2c_busy_stuck_count;
    snapshot->i2c_dma_err_count = s_i2c_dma_err_count;
    snapshot->i2c_dma_timeout_ndtr = s_i2c_dma_timeout_ndtr;
    snapshot->i2c_dma_timeout_state = s_i2c_dma_timeout_state;
    snapshot->i2c_dma_timeout_sr1 = s_i2c_dma_timeout_sr1;
    snapshot->i2c_dma_timeout_sr2 = s_i2c_dma_timeout_sr2;
    snapshot->i2c_dma_tc_ndtr = s_i2c_dma_tc_ndtr;
    snapshot->i2c_dma_tc_state = s_i2c_dma_tc_state;
    snapshot->i2c_dma_tc_sr1 = s_i2c_dma_tc_sr1;
    snapshot->i2c_dma_tc_sr2 = s_i2c_dma_tc_sr2;
    snapshot->i2c_dma_ev_irq_count = s_i2c_dma_ev_irq_count;
    snapshot->i2c_dma_tc_irq_count = s_i2c_dma_tc_irq_count;
    snapshot->i2c_dma_wait_timeout_count = s_i2c_dma_wait_timeout_count;
    snapshot->i2c_poll_event_timeout_count = s_i2c_poll_event_timeout_count;
    snapshot->i2c_poll_busy_timeout_count = s_i2c_poll_busy_timeout_count;
    snapshot->i2c_er_timeout_count = s_i2c_er_timeout_count;
    snapshot->i2c_poll_timeout_path = s_i2c_poll_timeout_path;
    snapshot->i2c_poll_timeout_phase = s_i2c_poll_timeout_phase;
    snapshot->i2c_poll_timeout_event = s_i2c_poll_timeout_event;
    snapshot->i2c_poll_timeout_sr1 = s_i2c_poll_timeout_sr1;
    snapshot->i2c_poll_timeout_sr2 = s_i2c_poll_timeout_sr2;
    snapshot->i2c_poll_timeout_start_addr = s_i2c_poll_timeout_start_addr;
    snapshot->i2c_poll_timeout_word_count = s_i2c_poll_timeout_word_count;
    snapshot->dma_timeout_count = s_dma_timeout_count;
    snapshot->thermal_backoff_count = s_thermal_backoff_count;
    snapshot->thermal_pair_timeout_count = s_thermal_pair_timeout_count;
    snapshot->thermal_pair_compose_ok_count = s_thermal_pair_compose_ok_count;
    snapshot->thermal_pair_wait_other_count = s_thermal_pair_wait_other_count;
    snapshot->thermal_pair_last_result = s_thermal_pair_last_result;
    snapshot->thermal_pair_last_subpage = s_thermal_pair_last_subpage;
    snapshot->thermal_pair_last_missing_subpage = s_thermal_pair_last_missing_subpage;
    snapshot->thermal_pair_last_gap_ms = s_thermal_pair_last_gap_ms;
    snapshot->thermal_pair_timeout_gap_last_ms = s_thermal_pair_timeout_gap_last_ms;
    snapshot->thermal_pair_timeout_gap_max_ms = s_thermal_pair_timeout_gap_max_ms;
    snapshot->thermal_pair_compose_gap_last_ms = s_thermal_pair_compose_gap_last_ms;
    snapshot->thermal_pair_compose_gap_max_ms = s_thermal_pair_compose_gap_max_ms;
    snapshot->thermal_pair_same_subpage_streak_last = s_thermal_pair_same_subpage_streak_last;
    snapshot->thermal_pair_same_subpage_streak_max = s_thermal_pair_same_subpage_streak_max;
    snapshot->thermal_pair_timeout_get_temp_last_us = s_thermal_pair_timeout_get_temp_last_us;
    snapshot->thermal_pair_timeout_step_last_us = s_thermal_pair_timeout_step_last_us;
    snapshot->thermal_ready_replace_count = s_thermal_ready_replace_count;
    snapshot->thermal_display_cancel_count = s_thermal_display_cancel_count;
    snapshot->thermal_3d_sync_present_attempt_count = s_thermal_3d_sync_present_attempt_count;
    snapshot->thermal_3d_sync_present_ok_count = s_thermal_3d_sync_present_ok_count;
    snapshot->thermal_3d_sync_present_fail_count = s_thermal_3d_sync_present_fail_count;
    snapshot->thermal_3d_claim_count = s_thermal_3d_claim_count;
    snapshot->thermal_3d_done_ok_count = s_thermal_3d_done_ok_count;
    snapshot->thermal_3d_done_error_count = s_thermal_3d_done_error_count;
    snapshot->thermal_3d_done_cancel_count = s_thermal_3d_done_cancel_count;
    snapshot->thermal_3d_wait_timeout_count = s_thermal_3d_wait_timeout_count;
    snapshot->lcd_dma_enter_count = s_lcd_dma_enter_count;
    snapshot->dma_irq_tc_count = s_dma_irq_tc_count;
    snapshot->dma_irq_te_count = s_dma_irq_te_count;
    snapshot->dma_wait_take_count = s_dma_wait_take_count;

    snapshot->watchdog_missing_progress_mask = s_watchdog_missing_progress_mask;
    snapshot->watchdog_fault_flags = s_watchdog_fault_flags;

    snapshot->input_stack_words = s_input_stack_words;
    snapshot->service_stack_words = s_service_stack_words;
    snapshot->ui_stack_words = s_ui_stack_words;
    snapshot->display_stack_words = s_display_stack_words;
    snapshot->thermal_stack_words = s_thermal_stack_words;
    snapshot->power_stack_words = s_power_stack_words;

    if (app_perf_baseline_scheduler_running() != 0U)
    {
        taskEXIT_CRITICAL();
    }
#endif
}
