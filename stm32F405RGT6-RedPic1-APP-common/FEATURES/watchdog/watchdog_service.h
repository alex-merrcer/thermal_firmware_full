#ifndef WATCHDOG_SERVICE_H
#define WATCHDOG_SERVICE_H

#include <stdint.h>

typedef enum
{
    WATCHDOG_PROGRESS_MAIN_LOOP = (1UL << 0),
    WATCHDOG_PROGRESS_KEY       = (1UL << 1),
    WATCHDOG_PROGRESS_OTA       = (1UL << 2),
    WATCHDOG_PROGRESS_ESP_HOST  = (1UL << 3),
    WATCHDOG_PROGRESS_BATTERY   = (1UL << 4),
    WATCHDOG_PROGRESS_UI        = (1UL << 5),
    WATCHDOG_PROGRESS_POWER     = (1UL << 6)
} watchdog_progress_mask_t;

typedef enum
{
    WATCHDOG_FAULT_NONE          = 0U,
    WATCHDOG_FAULT_KEY_STUCK     = (1UL << 0),
    WATCHDOG_FAULT_UART_ERROR    = (1UL << 1),
    WATCHDOG_FAULT_UART_OVERFLOW = (1UL << 2)
} watchdog_fault_mask_t;

typedef struct
{
    uint32_t tick_ms;
    uint32_t missing_progress_mask;
    uint32_t fault_flags;
    uint8_t valid;
} watchdog_reset_snapshot_t;

void watchdog_service_init(uint32_t feed_interval_ms);
void watchdog_service_begin_cycle(void);
void watchdog_service_mark_progress(uint32_t mask);
void watchdog_service_report_key_health(uint8_t healthy);
void watchdog_service_report_uart_errors(uint32_t flags);
void watchdog_service_note_stop_wake(void);
void watchdog_service_step(void);
void watchdog_service_force_feed(void);
uint8_t watchdog_service_is_healthy(void);
uint8_t watchdog_service_can_enter_stop(void);
uint32_t watchdog_service_get_missing_progress_mask(void);
uint32_t watchdog_service_get_last_fault_flags(void);
uint32_t watchdog_service_get_feed_interval_ms(void);
void watchdog_service_get_reset_snapshot(watchdog_reset_snapshot_t *out_snapshot);

#endif
