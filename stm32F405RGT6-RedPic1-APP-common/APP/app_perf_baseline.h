#ifndef APP_PERF_BASELINE_H
#define APP_PERF_BASELINE_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "clock_profile_service.h"
#include "power_manager.h"
#include "task.h"

#ifndef APP_PERF_BASELINE_ENABLE
	#define APP_PERF_BASELINE_ENABLE 1
#endif

typedef enum
{
    APP_PERF_NOTIFY_INPUT = 0,
    APP_PERF_NOTIFY_UI,
    APP_PERF_NOTIFY_SERVICE,
    APP_PERF_NOTIFY_DISPLAY
} app_perf_notify_target_t;

typedef enum
{
    APP_PERF_LCD_DMA_STATUS_NONE = 0,
    APP_PERF_LCD_DMA_STATUS_OK,
    APP_PERF_LCD_DMA_STATUS_TIMEOUT,
    APP_PERF_LCD_DMA_STATUS_ERROR
} app_perf_lcd_dma_status_t;

typedef enum
{
    APP_PERF_I2C_ERROR_AF = 0,
    APP_PERF_I2C_ERROR_BERR,
    APP_PERF_I2C_ERROR_ARLO,
    APP_PERF_I2C_ERROR_OVR,
    APP_PERF_I2C_ERROR_TIMEOUT,
    APP_PERF_I2C_ERROR_BUSY_STUCK,
    APP_PERF_I2C_ERROR_DMA_ERR
} app_perf_i2c_error_t;

typedef struct
{
    uint8_t enabled;
    uint8_t last_dma_ok;
    uint8_t last_dma_status;
    uint8_t thermal_active;
    uint8_t screen_off;
    power_state_t power_state;
    clock_profile_t clock_profile;

    uint32_t thermal_capture_frames;
    uint32_t thermal_display_frames;
    uint32_t thermal_capture_failures;
    uint32_t thermal_fps;
    uint32_t thermal_display_fps;
    uint32_t thermal_display_age_samples;
    uint32_t thermal_display_age_last_ms;
    uint32_t thermal_display_age_max_ms;
    uint32_t thermal_display_age_avg_ms;

    uint32_t thermal_frame_period_samples;
    uint32_t thermal_frame_period_last_ms;
    uint32_t thermal_frame_period_min_ms;
    uint32_t thermal_frame_period_max_ms;
    uint32_t thermal_frame_period_avg_ms;

    uint32_t get_temp_samples;
    uint32_t get_temp_last_us;
    uint32_t get_temp_max_us;
    uint32_t get_temp_avg_us;

    uint32_t gray_samples;
    uint32_t gray_last_us;
    uint32_t gray_max_us;
    uint32_t gray_avg_us;

    uint32_t thermal_step_samples;
    uint32_t thermal_step_last_us;
    uint32_t thermal_step_max_us;
    uint32_t thermal_step_avg_us;

    uint32_t lcd_dma_samples;
    uint32_t lcd_dma_last_us;
    uint32_t lcd_dma_max_us;
    uint32_t lcd_dma_avg_us;
    uint32_t lcd_dma_render_samples;
    uint32_t lcd_dma_render_last_us;
    uint32_t lcd_dma_render_max_us;
    uint32_t lcd_dma_render_avg_us;
    uint32_t lcd_dma_start_samples;
    uint32_t lcd_dma_start_last_us;
    uint32_t lcd_dma_start_max_us;
    uint32_t lcd_dma_start_avg_us;
    uint32_t lcd_dma_wait_samples;
    uint32_t lcd_dma_wait_last_us;
    uint32_t lcd_dma_wait_max_us;
    uint32_t lcd_dma_wait_avg_us;
    uint32_t lcd_dma_spi_idle_samples;
    uint32_t lcd_dma_spi_idle_last_us;
    uint32_t lcd_dma_spi_idle_max_us;
    uint32_t lcd_dma_spi_idle_avg_us;
    uint32_t lcd_dma_overlay_samples;
    uint32_t lcd_dma_overlay_last_us;
    uint32_t lcd_dma_overlay_max_us;
    uint32_t lcd_dma_overlay_avg_us;

    float latest_min_temp;
    float latest_max_temp;
    float latest_center_temp;

    uint32_t key_queue_drop_count;
    uint32_t ui_msg_drop_count;
    uint32_t service_queue_fail_count;
    uint32_t display_queue_fail_count;

    uint32_t input_notify_count;
    uint32_t ui_notify_count;
    uint32_t service_notify_count;
    uint32_t display_notify_count;

    uint32_t uart_error_count;
    uint32_t last_uart_error_flags;
    uint32_t i2c_failure_count;
    uint32_t i2c_af_count;
    uint32_t i2c_berr_count;
    uint32_t i2c_arlo_count;
    uint32_t i2c_ovr_count;
    uint32_t i2c_timeout_count;
    uint32_t i2c_busy_stuck_count;
    uint32_t i2c_dma_err_count;
    uint32_t i2c_dma_timeout_ndtr;
    uint32_t i2c_dma_timeout_state;
    uint32_t i2c_dma_timeout_sr1;
    uint32_t i2c_dma_timeout_sr2;
    uint32_t i2c_dma_tc_ndtr;
    uint32_t i2c_dma_tc_state;
    uint32_t i2c_dma_tc_sr1;
    uint32_t i2c_dma_tc_sr2;
    uint32_t i2c_dma_ev_irq_count;
    uint32_t i2c_dma_tc_irq_count;
    uint32_t dma_timeout_count;
    uint32_t thermal_backoff_count;
    uint32_t thermal_pair_timeout_count;
    uint32_t thermal_ready_replace_count;
    uint32_t thermal_display_cancel_count;
    uint32_t thermal_3d_sync_present_attempt_count;
    uint32_t thermal_3d_sync_present_ok_count;
    uint32_t thermal_3d_sync_present_fail_count;
    uint32_t thermal_3d_claim_count;
    uint32_t thermal_3d_done_ok_count;
    uint32_t thermal_3d_done_error_count;
    uint32_t thermal_3d_done_cancel_count;
    uint32_t thermal_3d_wait_timeout_count;
    uint32_t lcd_dma_enter_count;
    uint32_t dma_irq_tc_count;
    uint32_t dma_irq_te_count;
    uint32_t dma_wait_take_count;

    uint32_t watchdog_missing_progress_mask;
    uint32_t watchdog_fault_flags;

    UBaseType_t input_stack_words;
    UBaseType_t service_stack_words;
    UBaseType_t ui_stack_words;
    UBaseType_t display_stack_words;
    UBaseType_t thermal_stack_words;
    UBaseType_t power_stack_words;
} app_perf_baseline_snapshot_t;

void app_perf_baseline_init(void);
void app_perf_baseline_reset(void);
uint8_t app_perf_baseline_is_enabled(void);
uint32_t app_perf_baseline_cycle_now(void);
uint32_t app_perf_baseline_elapsed_us(uint32_t start_cycle);
void app_perf_baseline_record_thermal_capture_success(uint32_t capture_tick_ms,
                                                      float min_temp,
                                                      float max_temp,
                                                      float center_temp);
void app_perf_baseline_record_thermal_capture_failure(void);
void app_perf_baseline_record_thermal_display_age_ms(uint32_t elapsed_ms);
void app_perf_baseline_record_get_temp_us(uint32_t elapsed_us);
void app_perf_baseline_record_gray_us(uint32_t elapsed_us);
void app_perf_baseline_record_thermal_step_us(uint32_t elapsed_us);
void app_perf_baseline_record_lcd_dma_result(uint32_t elapsed_us,
                                             app_perf_lcd_dma_status_t status);
void app_perf_baseline_record_lcd_dma_render_us(uint32_t elapsed_us);
void app_perf_baseline_record_lcd_dma_start_us(uint32_t elapsed_us);
void app_perf_baseline_record_lcd_dma_wait_us(uint32_t elapsed_us);
void app_perf_baseline_record_lcd_dma_spi_idle_us(uint32_t elapsed_us);
void app_perf_baseline_record_lcd_dma_overlay_us(uint32_t elapsed_us);
void app_perf_baseline_record_task_notify(app_perf_notify_target_t target);
void app_perf_baseline_record_key_queue_drop(void);
void app_perf_baseline_record_ui_msg_drop(void);
void app_perf_baseline_record_service_queue_fail(void);
void app_perf_baseline_record_display_queue_fail(void);
void app_perf_baseline_record_uart_errors(uint32_t flags);
void app_perf_baseline_record_i2c_failure(void);
void app_perf_baseline_record_i2c_transport_error(app_perf_i2c_error_t error_kind);
void app_perf_baseline_record_i2c_dma_timeout_snapshot(uint16_t ndtr,
                                                       uint8_t state,
                                                       uint32_t sr1,
                                                       uint32_t sr2);
void app_perf_baseline_record_i2c_dma_tc_snapshot(uint16_t ndtr,
                                                  uint8_t state,
                                                  uint32_t sr1,
                                                  uint32_t sr2);
void app_perf_baseline_record_i2c_dma_ev_irq(void);
void app_perf_baseline_record_i2c_dma_tc_irq(void);
void app_perf_baseline_record_thermal_backoff(void);
void app_perf_baseline_record_thermal_pair_timeout(void);
void app_perf_baseline_record_thermal_ready_replace(void);
void app_perf_baseline_record_thermal_display_cancel(void);
void app_perf_baseline_record_thermal_3d_sync_present_attempt(void);
void app_perf_baseline_record_thermal_3d_sync_present_ok(void);
void app_perf_baseline_record_thermal_3d_sync_present_fail(void);
void app_perf_baseline_record_thermal_3d_claim(void);
void app_perf_baseline_record_thermal_3d_done_ok(void);
void app_perf_baseline_record_thermal_3d_done_error(void);
void app_perf_baseline_record_thermal_3d_done_cancel(void);
void app_perf_baseline_record_thermal_3d_wait_timeout(void);
void app_perf_baseline_record_lcd_dma_enter(void);
void app_perf_baseline_record_dma_irq_tc(void);
void app_perf_baseline_record_dma_irq_te(void);
void app_perf_baseline_record_dma_wait_take(void);
void app_perf_baseline_set_watchdog_snapshot(uint32_t missing_progress_mask,
                                             uint32_t fault_flags);
void app_perf_baseline_set_runtime_state(power_state_t power_state,
                                         clock_profile_t clock_profile,
                                         uint8_t thermal_active,
                                         uint8_t screen_off);
void app_perf_baseline_refresh_task_stacks(TaskHandle_t input_task,
                                           TaskHandle_t service_task,
                                           TaskHandle_t ui_task,
                                           TaskHandle_t display_task,
                                           TaskHandle_t thermal_task,
                                           TaskHandle_t power_task);
void app_perf_baseline_get_snapshot(app_perf_baseline_snapshot_t *snapshot);

#endif
