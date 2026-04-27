#include "watchdog_service.h"

#include "uart_rx_ring.h"
#include "watchdog_supervisor.h"

static uint8_t watchdog_service_register_default_tasks(void)
{
    static const watchdog_supervisor_task_t s_default_tasks[] = {
        { "main_loop", WATCHDOG_PROGRESS_MAIN_LOOP, 1U },
        { "key", WATCHDOG_PROGRESS_KEY, 1U },
        { "ota", WATCHDOG_PROGRESS_OTA, 1U },
        { "esp_host", WATCHDOG_PROGRESS_ESP_HOST, 1U },
        { "battery", WATCHDOG_PROGRESS_BATTERY, 1U },
        { "ui", WATCHDOG_PROGRESS_UI, 1U },
        { "power", WATCHDOG_PROGRESS_POWER, 1U }
    };
    uint8_t i = 0U;

    for (i = 0U; i < (uint8_t)(sizeof(s_default_tasks) / sizeof(s_default_tasks[0])); ++i)
    {
        if (watchdog_supervisor_register_task(&s_default_tasks[i]) == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

void watchdog_service_init(uint32_t feed_interval_ms)
{
    watchdog_supervisor_init(feed_interval_ms);
    (void)watchdog_service_register_default_tasks();
}

void watchdog_service_begin_cycle(void)
{
    watchdog_supervisor_begin_window();
}

void watchdog_service_mark_progress(uint32_t mask)
{
    watchdog_supervisor_mark_progress(mask);
}

void watchdog_service_report_key_health(uint8_t healthy)
{
    watchdog_supervisor_mark_progress(WATCHDOG_PROGRESS_KEY);
    if (healthy == 0U)
    {
        watchdog_supervisor_report_fault_flags(WATCHDOG_FAULT_KEY_STUCK);
    }
}

void watchdog_service_report_uart_errors(uint32_t flags)
{
    if (flags == 0U)
    {
        return;
    }

    if ((flags & UART_RX_RING_FLAG_OVERFLOW) != 0U)
    {
        watchdog_supervisor_report_fault_flags(WATCHDOG_FAULT_UART_OVERFLOW);
    }

    if ((flags & (UART_RX_RING_FLAG_ORE | UART_RX_RING_FLAG_FE | UART_RX_RING_FLAG_NE)) != 0U)
    {
        watchdog_supervisor_report_fault_flags(WATCHDOG_FAULT_UART_ERROR);
    }
}

void watchdog_service_note_stop_wake(void)
{
    watchdog_supervisor_note_stop_wake();
}

void watchdog_service_step(void)
{
    watchdog_supervisor_step();
}

void watchdog_service_force_feed(void)
{
    watchdog_supervisor_force_feed();
}

uint8_t watchdog_service_is_healthy(void)
{
    return watchdog_supervisor_is_healthy();
}

uint8_t watchdog_service_can_enter_stop(void)
{
    return watchdog_supervisor_can_enter_stop();
}

uint32_t watchdog_service_get_missing_progress_mask(void)
{
    return watchdog_supervisor_get_missing_progress_mask();
}

uint32_t watchdog_service_get_last_fault_flags(void)
{
    return watchdog_supervisor_get_last_fault_flags();
}

uint32_t watchdog_service_get_feed_interval_ms(void)
{
    return watchdog_supervisor_get_feed_interval_ms();
}

void watchdog_service_get_reset_snapshot(watchdog_reset_snapshot_t *out_snapshot)
{
    watchdog_supervisor_reset_snapshot_t snapshot;

    if (out_snapshot == 0)
    {
        return;
    }

    watchdog_supervisor_get_reset_snapshot(&snapshot);

    out_snapshot->tick_ms = snapshot.tick_ms;
    out_snapshot->missing_progress_mask = snapshot.missing_progress_mask;
    out_snapshot->fault_flags = snapshot.fault_flags;
    out_snapshot->valid = snapshot.valid;
}
