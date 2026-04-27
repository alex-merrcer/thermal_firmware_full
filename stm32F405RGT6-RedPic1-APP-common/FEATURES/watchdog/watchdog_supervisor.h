#ifndef WATCHDOG_SUPERVISOR_H
#define WATCHDOG_SUPERVISOR_H

#include <stdint.h>

typedef struct
{
    const char *name;
    uint32_t progress_mask;
    uint8_t required;
} watchdog_supervisor_task_t;

typedef struct
{
    uint32_t tick_ms;
    uint32_t missing_progress_mask;
    uint32_t fault_flags;
    uint8_t valid;
} watchdog_supervisor_reset_snapshot_t;

void watchdog_supervisor_init(uint32_t feed_interval_ms);
uint8_t watchdog_supervisor_register_task(const watchdog_supervisor_task_t *task);
void watchdog_supervisor_begin_window(void);
void watchdog_supervisor_mark_progress(uint32_t progress_mask);
void watchdog_supervisor_report_fault_flags(uint32_t fault_flags);
void watchdog_supervisor_note_stop_wake(void);
void watchdog_supervisor_step(void);
void watchdog_supervisor_force_feed(void);
uint8_t watchdog_supervisor_is_healthy(void);
uint8_t watchdog_supervisor_can_enter_stop(void);
uint32_t watchdog_supervisor_get_missing_progress_mask(void);
uint32_t watchdog_supervisor_get_last_fault_flags(void);
uint32_t watchdog_supervisor_get_feed_interval_ms(void);
void watchdog_supervisor_get_reset_snapshot(watchdog_supervisor_reset_snapshot_t *out_snapshot);

#endif
