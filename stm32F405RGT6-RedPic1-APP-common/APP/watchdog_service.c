#include "watchdog_service.h"

#include "iwdg.h"
#include "power_manager.h"
#include "uart_rx_ring.h"

#define WATCHDOG_SERVICE_DEFAULT_FEED_MS 1000UL
#define WATCHDOG_SERVICE_REQUIRED_PROGRESS_MASK \
    (WATCHDOG_PROGRESS_MAIN_LOOP | WATCHDOG_PROGRESS_KEY | WATCHDOG_PROGRESS_OTA | \
     WATCHDOG_PROGRESS_ESP_HOST | WATCHDOG_PROGRESS_BATTERY | WATCHDOG_PROGRESS_UI | \
     WATCHDOG_PROGRESS_POWER)

static uint32_t s_feed_interval_ms = WATCHDOG_SERVICE_DEFAULT_FEED_MS;
static uint32_t s_last_feed_ms = 0U;
static uint32_t s_cycle_progress_mask = 0U;
static uint32_t s_cycle_fault_flags = 0U;
static uint32_t s_last_missing_progress_mask = 0U;
static uint32_t s_last_fault_flags = 0U;
static uint8_t s_stop_entry_ready = 1U;

static uint8_t watchdog_service_cycle_is_healthy(void)
{
    s_last_missing_progress_mask =
        (WATCHDOG_SERVICE_REQUIRED_PROGRESS_MASK & (~s_cycle_progress_mask));
    s_last_fault_flags = s_cycle_fault_flags;

    return ((s_last_missing_progress_mask == 0U) && (s_last_fault_flags == 0U)) ? 1U : 0U;
}

void watchdog_service_init(uint32_t feed_interval_ms)
{
    if (feed_interval_ms == 0U)
    {
        feed_interval_ms = WATCHDOG_SERVICE_DEFAULT_FEED_MS;
    }

    s_feed_interval_ms = feed_interval_ms;
    s_last_feed_ms = power_manager_get_tick_ms();
    s_cycle_progress_mask = 0U;
    s_cycle_fault_flags = 0U;
    s_last_missing_progress_mask = 0U;
    s_last_fault_flags = 0U;
    s_stop_entry_ready = 1U;
    IWDG_Feed();
}

void watchdog_service_begin_cycle(void)
{
    s_cycle_progress_mask = 0U;
    s_cycle_fault_flags = 0U;
}

void watchdog_service_mark_progress(uint32_t mask)
{
    s_cycle_progress_mask |= mask;
}

void watchdog_service_report_key_health(uint8_t healthy)
{
    s_cycle_progress_mask |= WATCHDOG_PROGRESS_KEY;
    if (healthy == 0U)
    {
        s_cycle_fault_flags |= WATCHDOG_FAULT_KEY_STUCK;
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
        s_cycle_fault_flags |= WATCHDOG_FAULT_UART_OVERFLOW;
    }

    if ((flags & (UART_RX_RING_FLAG_ORE | UART_RX_RING_FLAG_FE | UART_RX_RING_FLAG_NE)) != 0U)
    {
        s_cycle_fault_flags |= WATCHDOG_FAULT_UART_ERROR;
    }
}

void watchdog_service_note_stop_wake(void)
{
    s_stop_entry_ready = 0U;
}

void watchdog_service_step(void)
{
    uint32_t now_ms = power_manager_get_tick_ms();
    uint8_t healthy = watchdog_service_cycle_is_healthy();

    s_stop_entry_ready = healthy;

    if (healthy != 0U && (now_ms - s_last_feed_ms) >= s_feed_interval_ms)
    {
        s_last_feed_ms = now_ms;
        IWDG_Feed();
    }
}

void watchdog_service_force_feed(void)
{
    s_last_feed_ms = power_manager_get_tick_ms();
    IWDG_Feed();
}

uint8_t watchdog_service_is_healthy(void)
{
    return watchdog_service_cycle_is_healthy();
}

uint8_t watchdog_service_can_enter_stop(void)
{
    return s_stop_entry_ready;
}

uint32_t watchdog_service_get_missing_progress_mask(void)
{
    return s_last_missing_progress_mask;
}

uint32_t watchdog_service_get_last_fault_flags(void)
{
    return s_last_fault_flags;
}

uint32_t watchdog_service_get_feed_interval_ms(void)
{
    return s_feed_interval_ms;
}
