#include "watchdog_supervisor.h"

#include <string.h>

#include "iwdg.h"
#include "power_manager.h"

#define WATCHDOG_SUPERVISOR_DEFAULT_FEED_MS  1000UL
#define WATCHDOG_SUPERVISOR_MAX_TASKS        16U

static watchdog_supervisor_task_t s_tasks[WATCHDOG_SUPERVISOR_MAX_TASKS];
static uint8_t s_task_count = 0U;
static uint32_t s_required_progress_mask = 0U;
static uint32_t s_feed_interval_ms = WATCHDOG_SUPERVISOR_DEFAULT_FEED_MS;
static uint32_t s_last_feed_ms = 0U;
static uint32_t s_window_progress_mask = 0U;
static uint32_t s_window_fault_flags = 0U;
static uint32_t s_last_missing_progress_mask = 0U;
static uint32_t s_last_fault_flags = 0U;
static uint8_t s_stop_entry_ready = 1U;
static watchdog_supervisor_reset_snapshot_t s_reset_snapshot;

static uint8_t watchdog_supervisor_task_name_equal(const char *lhs, const char *rhs)
{
    if (lhs == 0 || rhs == 0)
    {
        return 0U;
    }

    return (strcmp(lhs, rhs) == 0) ? 1U : 0U;
}

static uint8_t watchdog_supervisor_window_is_healthy(void)
{
    s_last_missing_progress_mask =
        (s_required_progress_mask & (~s_window_progress_mask));
    s_last_fault_flags = s_window_fault_flags;

    return ((s_last_missing_progress_mask == 0U) &&
            (s_last_fault_flags == 0U)) ? 1U : 0U;
}

static void watchdog_supervisor_capture_reset_snapshot(uint32_t tick_ms)
{
    s_reset_snapshot.valid = 1U;
    s_reset_snapshot.tick_ms = tick_ms;
    s_reset_snapshot.missing_progress_mask = s_last_missing_progress_mask;
    s_reset_snapshot.fault_flags = s_last_fault_flags;
}

void watchdog_supervisor_init(uint32_t feed_interval_ms)
{
    if (feed_interval_ms == 0U)
    {
        feed_interval_ms = WATCHDOG_SUPERVISOR_DEFAULT_FEED_MS;
    }

    memset(s_tasks, 0, sizeof(s_tasks));
    memset(&s_reset_snapshot, 0, sizeof(s_reset_snapshot));

    s_task_count = 0U;
    s_required_progress_mask = 0U;
    s_feed_interval_ms = feed_interval_ms;
    s_last_feed_ms = power_manager_get_tick_ms();
    s_window_progress_mask = 0U;
    s_window_fault_flags = 0U;
    s_last_missing_progress_mask = 0U;
    s_last_fault_flags = 0U;
    s_stop_entry_ready = 1U;
    IWDG_Feed();
}

uint8_t watchdog_supervisor_register_task(const watchdog_supervisor_task_t *task)
{
    uint8_t i = 0U;

    if (task == 0 || task->name == 0 || task->progress_mask == 0U)
    {
        return 0U;
    }

    for (i = 0U; i < s_task_count; ++i)
    {
        if (watchdog_supervisor_task_name_equal(s_tasks[i].name, task->name) != 0U &&
            s_tasks[i].progress_mask == task->progress_mask)
        {
            return 1U;
        }

        if ((s_tasks[i].progress_mask & task->progress_mask) != 0U)
        {
            return 0U;
        }
    }

    if (s_task_count >= WATCHDOG_SUPERVISOR_MAX_TASKS)
    {
        return 0U;
    }

    s_tasks[s_task_count] = *task;
    if (task->required != 0U)
    {
        s_required_progress_mask |= task->progress_mask;
    }
    s_task_count++;
    return 1U;
}

void watchdog_supervisor_begin_window(void)
{
    s_window_progress_mask = 0U;
    s_window_fault_flags = 0U;
}

void watchdog_supervisor_mark_progress(uint32_t progress_mask)
{
    s_window_progress_mask |= progress_mask;
}

void watchdog_supervisor_report_fault_flags(uint32_t fault_flags)
{
    s_window_fault_flags |= fault_flags;
}

void watchdog_supervisor_note_stop_wake(void)
{
    s_stop_entry_ready = 0U;
}

void watchdog_supervisor_step(void)
{
    uint32_t now_ms = power_manager_get_tick_ms();
    uint8_t healthy = watchdog_supervisor_window_is_healthy();

    s_stop_entry_ready = healthy;
    if (healthy == 0U)
    {
        watchdog_supervisor_capture_reset_snapshot(now_ms);
        return;
    }

    if ((now_ms - s_last_feed_ms) >= s_feed_interval_ms)
    {
        s_last_feed_ms = now_ms;
        IWDG_Feed();
    }
}

void watchdog_supervisor_force_feed(void)
{
    s_last_feed_ms = power_manager_get_tick_ms();
    IWDG_Feed();
}

uint8_t watchdog_supervisor_is_healthy(void)
{
    return watchdog_supervisor_window_is_healthy();
}

uint8_t watchdog_supervisor_can_enter_stop(void)
{
    return s_stop_entry_ready;
}

uint32_t watchdog_supervisor_get_missing_progress_mask(void)
{
    return s_last_missing_progress_mask;
}

uint32_t watchdog_supervisor_get_last_fault_flags(void)
{
    return s_last_fault_flags;
}

uint32_t watchdog_supervisor_get_feed_interval_ms(void)
{
    return s_feed_interval_ms;
}

void watchdog_supervisor_get_reset_snapshot(watchdog_supervisor_reset_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0)
    {
        return;
    }

    *out_snapshot = s_reset_snapshot;
}
