#include "thermal_frame_slot.h"

#include <string.h>

#include "app_display_runtime.h"
#include "app_perf_baseline.h"
#include "redpic1_thermal.h"

#define THERMAL_FRAME_SLOT_TOKEN_SHIFT      8U
#define THERMAL_FRAME_SLOT_TOKEN_SLOT_MASK  0xFFU

#if (REDPIC1_THERMAL_DROP_EXPIRED_FRAME_ENABLE != 0U)
    #define THERMAL_FRAME_SLOT_DROP_EXPIRED_ACTIVE 1U
#else
    #define THERMAL_FRAME_SLOT_DROP_EXPIRED_ACTIVE 0U
#endif

static redpic1_thermal_frame_slot_ops_t s_ops;
static redpic1_thermal_frame_slot_t s_frame_slots[REDPIC1_THERMAL_FRAME_SLOT_COUNT];
static uint32_t s_frame_sequence = 0U;
static uint8_t s_front_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
static uint8_t s_ready_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
static uint8_t s_inflight_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
static uintptr_t s_last_submitted_token = 0U;
static uint8_t s_last_submitted_valid = 0U;

static void redpic1_thermal_frame_slot_enter_critical(void)
{
    if (s_ops.enter_critical != 0)
    {
        s_ops.enter_critical();
    }
}

static void redpic1_thermal_frame_slot_exit_critical(void)
{
    if (s_ops.exit_critical != 0)
    {
        s_ops.exit_critical();
    }
}

static uint32_t redpic1_thermal_frame_slot_get_tick_ms(void)
{
    if (s_ops.get_tick_ms != 0)
    {
        return s_ops.get_tick_ms();
    }

    return 0U;
}

static uint32_t redpic1_thermal_frame_slot_get_expired_threshold_ms(void)
{
    if (s_ops.get_expired_frame_threshold_ms != 0)
    {
        return s_ops.get_expired_frame_threshold_ms();
    }

    return 0U;
}

static uint8_t redpic1_thermal_frame_slot_run_enabled(void)
{
    if (s_ops.run_enabled != 0)
    {
        return s_ops.run_enabled();
    }

    return 1U;
}

static uint8_t redpic1_thermal_frame_slot_display_paused(void)
{
    if (s_ops.display_paused != 0)
    {
        return s_ops.display_paused();
    }

    return 0U;
}

static uint8_t redpic1_thermal_frame_slot_overlay_hold(void)
{
    if (s_ops.overlay_hold != 0)
    {
        return s_ops.overlay_hold();
    }

    return 0U;
}

static uintptr_t redpic1_thermal_frame_slot_make_token(uint8_t slot_index, uint32_t frame_seq)
{
    return ((((uintptr_t)frame_seq) << THERMAL_FRAME_SLOT_TOKEN_SHIFT) |
            (uintptr_t)slot_index);
}

static uint8_t redpic1_thermal_frame_slot_token_slot_index(uintptr_t token)
{
    return (uint8_t)(token & THERMAL_FRAME_SLOT_TOKEN_SLOT_MASK);
}

static uint32_t redpic1_thermal_frame_slot_token_frame_seq(uintptr_t token)
{
    return (uint32_t)(token >> THERMAL_FRAME_SLOT_TOKEN_SHIFT);
}

static void redpic1_thermal_frame_slot_clear_submitted_token_locked(void)
{
    s_last_submitted_token = 0U;
    s_last_submitted_valid = 0U;
}

static void redpic1_thermal_frame_slot_clear_submitted_token(void)
{
    redpic1_thermal_frame_slot_enter_critical();
    redpic1_thermal_frame_slot_clear_submitted_token_locked();
    redpic1_thermal_frame_slot_exit_critical();
}

static void redpic1_thermal_frame_slot_free_locked(uint8_t slot_index)
{
    if (slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT)
    {
        return;
    }

    s_frame_slots[slot_index].valid = 0U;
    s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FREE;

    if (s_front_slot_index == slot_index)
    {
        s_front_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    }
    if (s_ready_slot_index == slot_index)
    {
        s_ready_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    }
    if (s_inflight_slot_index == slot_index)
    {
        s_inflight_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    }
    if (s_last_submitted_valid != 0U &&
        redpic1_thermal_frame_slot_token_slot_index(s_last_submitted_token) == slot_index)
    {
        redpic1_thermal_frame_slot_clear_submitted_token_locked();
    }
}

static redpic1_thermal_frame_slot_t *redpic1_thermal_frame_slot_get_front(void)
{
    uint8_t front_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    redpic1_thermal_frame_slot_t *slot = 0;

    redpic1_thermal_frame_slot_enter_critical();
    front_index = s_front_slot_index;
    if (front_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
        s_frame_slots[front_index].valid != 0U &&
        s_frame_slots[front_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_FRONT)
    {
        slot = &s_frame_slots[front_index];
    }
    redpic1_thermal_frame_slot_exit_critical();

    return slot;
}

static uint8_t redpic1_thermal_frame_slot_ready_frame_expired_locked(uint8_t slot_index,
                                                                     uint32_t now_ms)
{
    uint32_t threshold_ms = 0U;
    uint32_t capture_tick_ms = 0U;

    if (slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT ||
        s_frame_slots[slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_READY ||
        s_frame_slots[slot_index].valid == 0U)
    {
        return 0U;
    }

    capture_tick_ms = s_frame_slots[slot_index].capture_tick_ms;
    threshold_ms = redpic1_thermal_frame_slot_get_expired_threshold_ms();
    if (capture_tick_ms == 0U || threshold_ms == 0U)
    {
        return 0U;
    }

    return ((uint32_t)(now_ms - capture_tick_ms) > threshold_ms) ? 1U : 0U;
}

static uint8_t redpic1_thermal_frame_slot_cancel_ready_by_token(uintptr_t token)
{
    uint8_t slot_index = redpic1_thermal_frame_slot_token_slot_index(token);
    uint32_t frame_seq = redpic1_thermal_frame_slot_token_frame_seq(token);
    uint8_t cancelled = 0U;

    redpic1_thermal_frame_slot_enter_critical();
    if (slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
        s_ready_slot_index == slot_index &&
        s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY &&
        s_frame_slots[slot_index].frame_seq == frame_seq)
    {
        redpic1_thermal_frame_slot_free_locked(slot_index);
        cancelled = 1U;
    }
    redpic1_thermal_frame_slot_exit_critical();

    if (cancelled != 0U)
    {
        app_perf_baseline_record_thermal_display_cancel();
        app_perf_baseline_record_thermal_3d_done_cancel();
    }

    return cancelled;
}

static uint8_t redpic1_thermal_frame_slot_can_submit_locked(void)
{
    if (redpic1_thermal_frame_slot_run_enabled() == 0U ||
        redpic1_thermal_frame_slot_display_paused() != 0U ||
        redpic1_thermal_frame_slot_overlay_hold() != 0U ||
        s_inflight_slot_index != REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE)
    {
        return 0U;
    }

    if (s_ready_slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT ||
        s_frame_slots[s_ready_slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_READY)
    {
        return 0U;
    }

    return 1U;
}

static void redpic1_thermal_frame_slot_try_submit_after_gap_close(void)
{
    uint8_t can_submit = 0U;

    redpic1_thermal_frame_slot_enter_critical();
    can_submit = redpic1_thermal_frame_slot_can_submit_locked();
    redpic1_thermal_frame_slot_exit_critical();

    if (can_submit != 0U)
    {
        (void)redpic1_thermal_frame_slot_submit_latest_ready();
    }
}

static uint8_t redpic1_thermal_frame_slot_try_claim_present(uintptr_t token, uint8_t **gray_frame)
{
    uint8_t slot_index = redpic1_thermal_frame_slot_token_slot_index(token);
    uint32_t frame_seq = redpic1_thermal_frame_slot_token_frame_seq(token);
    uint32_t now_ms = redpic1_thermal_frame_slot_get_tick_ms();
    uint8_t claimed = 0U;
    uint8_t expired = 0U;

    if (gray_frame == 0)
    {
        return 0U;
    }

    *gray_frame = 0;

    redpic1_thermal_frame_slot_enter_critical();
    if (redpic1_thermal_frame_slot_run_enabled() != 0U &&
        slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
        s_ready_slot_index == slot_index &&
        s_inflight_slot_index == REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE &&
        s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY &&
        s_frame_slots[slot_index].frame_seq == frame_seq)
    {
#if (THERMAL_FRAME_SLOT_DROP_EXPIRED_ACTIVE != 0U)
        if (redpic1_thermal_frame_slot_ready_frame_expired_locked(slot_index, now_ms) != 0U)
        {
            expired = 1U;
        }
        else
#endif
        {
            s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT;
            s_ready_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
            s_inflight_slot_index = slot_index;
            *gray_frame = s_frame_slots[slot_index].gray_frame;
            claimed = 1U;
        }
    }
    redpic1_thermal_frame_slot_exit_critical();

    if (expired != 0U)
    {
        (void)redpic1_thermal_frame_slot_cancel_ready_by_token(token);
        return 0U;
    }

    if (claimed != 0U)
    {
        app_perf_baseline_record_thermal_3d_claim();
    }

    return claimed;
}

static void redpic1_thermal_frame_slot_handle_display_done(uintptr_t token,
                                                           app_display_thermal_done_status_t status)
{
    uint8_t slot_index = redpic1_thermal_frame_slot_token_slot_index(token);
    uint32_t frame_seq = redpic1_thermal_frame_slot_token_frame_seq(token);
    uint32_t display_done_tick_ms = redpic1_thermal_frame_slot_get_tick_ms();
    uint32_t display_age_ms = 0U;
    uint8_t note_cancel = 0U;
    uint8_t note_display_age = 0U;
    uint8_t note_done_ok = 0U;
    uint8_t note_done_error = 0U;
    uint8_t note_done_cancel = 0U;

    redpic1_thermal_frame_slot_enter_critical();
    if (slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
        s_frame_slots[slot_index].frame_seq == frame_seq)
    {
        switch (status)
        {
        case APP_DISPLAY_THERMAL_DONE_OK:
            if (s_inflight_slot_index == slot_index &&
                s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT)
            {
                s_inflight_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
                if (s_frame_slots[slot_index].capture_tick_ms != 0U)
                {
                    display_age_ms = (uint32_t)(display_done_tick_ms -
                                                s_frame_slots[slot_index].capture_tick_ms);
                    note_display_age = 1U;
                }
                if (redpic1_thermal_frame_slot_run_enabled() != 0U)
                {
                    uint8_t old_front = s_front_slot_index;

                    if (old_front < REDPIC1_THERMAL_FRAME_SLOT_COUNT && old_front != slot_index)
                    {
                        redpic1_thermal_frame_slot_free_locked(old_front);
                    }

                    s_frame_slots[slot_index].valid = 1U;
                    s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FRONT;
                    s_front_slot_index = slot_index;
                }
                else
                {
                    redpic1_thermal_frame_slot_free_locked(slot_index);
                }
                note_done_ok = 1U;
            }
            break;

        case APP_DISPLAY_THERMAL_DONE_CANCELLED:
            if ((s_ready_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY) ||
                (s_inflight_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT))
            {
                redpic1_thermal_frame_slot_free_locked(slot_index);
                note_cancel = 1U;
                note_done_cancel = 1U;
            }
            break;

        case APP_DISPLAY_THERMAL_DONE_ERROR:
        default:
            if ((s_inflight_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT) ||
                (s_ready_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY))
            {
                redpic1_thermal_frame_slot_free_locked(slot_index);
                note_done_error = 1U;
            }
            break;
        }
    }
    redpic1_thermal_frame_slot_exit_critical();

    if (note_cancel != 0U)
    {
        app_perf_baseline_record_thermal_display_cancel();
    }

    if (note_display_age != 0U)
    {
        app_perf_baseline_record_thermal_display_age_ms(display_age_ms);
    }

    if (note_done_ok != 0U)
    {
        app_perf_baseline_record_thermal_3d_done_ok();
    }
    if (note_done_error != 0U)
    {
        app_perf_baseline_record_thermal_3d_done_error();
    }
    if (note_done_cancel != 0U)
    {
        app_perf_baseline_record_thermal_3d_done_cancel();
    }

    redpic1_thermal_frame_slot_enter_critical();
    if (s_last_submitted_valid != 0U && s_last_submitted_token == token)
    {
        redpic1_thermal_frame_slot_clear_submitted_token_locked();
    }
    redpic1_thermal_frame_slot_exit_critical();

    if (note_done_ok != 0U)
    {
        redpic1_thermal_frame_slot_try_submit_after_gap_close();
    }
}

void redpic1_thermal_frame_slot_init(const redpic1_thermal_frame_slot_ops_t *ops)
{
    memset(&s_ops, 0, sizeof(s_ops));
    if (ops != 0)
    {
        s_ops = *ops;
    }

    redpic1_thermal_frame_slot_reset();
}

void redpic1_thermal_frame_slot_bind_display_runtime(void)
{
    app_display_runtime_set_thermal_present_claim_callback(redpic1_thermal_frame_slot_try_claim_present);
    app_display_runtime_set_thermal_present_done_callback(redpic1_thermal_frame_slot_handle_display_done);
}

redpic1_thermal_frame_slot_t *redpic1_thermal_frame_slot_acquire_back(void)
{
    uint8_t slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    uint8_t note_replace = 0U;

    redpic1_thermal_frame_slot_enter_critical();
    for (slot_index = 0U; slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT; ++slot_index)
    {
        if (s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_FREE)
        {
            break;
        }
    }

    if (slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
        s_ready_slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT)
    {
        slot_index = s_ready_slot_index;
        redpic1_thermal_frame_slot_free_locked(slot_index);
        note_replace = 1U;
    }

    if (slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT)
    {
        s_frame_slots[slot_index].valid = 0U;
        s_frame_slots[slot_index].ready_tick_ms = 0U;
        s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_WRITING;
    }
    redpic1_thermal_frame_slot_exit_critical();

    if (note_replace != 0U)
    {
        app_perf_baseline_record_thermal_ready_replace();
    }

    if (slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT)
    {
        return 0;
    }

    return &s_frame_slots[slot_index];
}

void redpic1_thermal_frame_slot_release_back(redpic1_thermal_frame_slot_t *slot)
{
    uint8_t slot_index = 0U;

    if (slot == 0)
    {
        return;
    }

    slot_index = (uint8_t)(slot - &s_frame_slots[0]);
    if (slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT)
    {
        return;
    }

    redpic1_thermal_frame_slot_enter_critical();
    if (s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_WRITING)
    {
        redpic1_thermal_frame_slot_free_locked(slot_index);
    }
    redpic1_thermal_frame_slot_exit_critical();
}

void redpic1_thermal_frame_slot_publish_back(redpic1_thermal_frame_slot_t *slot)
{
    uint8_t publish_index = 0U;
    uint8_t old_ready = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    uint8_t note_replace = 0U;
    uint32_t ready_tick_ms = redpic1_thermal_frame_slot_get_tick_ms();

    if (slot == 0)
    {
        return;
    }

    publish_index = (uint8_t)(slot - &s_frame_slots[0]);
    if (publish_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT)
    {
        return;
    }

    redpic1_thermal_frame_slot_enter_critical();
    if (s_frame_slots[publish_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_WRITING)
    {
        old_ready = s_ready_slot_index;
        if (old_ready < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
            old_ready != publish_index &&
            s_frame_slots[old_ready].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY)
        {
            redpic1_thermal_frame_slot_free_locked(old_ready);
            note_replace = 1U;
        }
        s_frame_slots[publish_index].valid = 1U;
        s_frame_slots[publish_index].ready_tick_ms = ready_tick_ms;
        s_frame_slots[publish_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_READY;
        s_ready_slot_index = publish_index;
    }
    redpic1_thermal_frame_slot_exit_critical();

    if (note_replace != 0U)
    {
        app_perf_baseline_record_thermal_ready_replace();
    }
}

void redpic1_thermal_frame_slot_present_front(void)
{
    redpic1_thermal_frame_slot_t *slot = 0;

    if (redpic1_thermal_frame_slot_run_enabled() == 0U ||
        redpic1_thermal_frame_slot_display_paused() != 0U ||
        redpic1_thermal_frame_slot_overlay_hold() != 0U)
    {
        return;
    }

    slot = redpic1_thermal_frame_slot_get_front();
    if (slot == 0 || s_ops.present_gray_frame == 0)
    {
        return;
    }

    (void)s_ops.present_gray_frame(slot->gray_frame);
}

uint32_t redpic1_thermal_frame_slot_allocate_sequence(void)
{
    uint32_t frame_seq = 0U;

    redpic1_thermal_frame_slot_enter_critical();
    frame_seq = ++s_frame_sequence;
    redpic1_thermal_frame_slot_exit_critical();

    return frame_seq;
}

uint8_t redpic1_thermal_frame_slot_submit_latest_ready(void)
{
    redpic1_thermal_frame_slot_t *slot = 0;
    uintptr_t token = 0U;
    uint8_t submit_needed = 0U;
    uint8_t ok = 0U;

    redpic1_thermal_frame_slot_enter_critical();
    if (s_ready_slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
        s_frame_slots[s_ready_slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY)
    {
        slot = &s_frame_slots[s_ready_slot_index];
        token = redpic1_thermal_frame_slot_make_token(s_ready_slot_index, slot->frame_seq);
        if (s_last_submitted_valid == 0U || s_last_submitted_token != token)
        {
            submit_needed = 1U;
        }
    }
    redpic1_thermal_frame_slot_exit_critical();

    if (slot == 0)
    {
        return 0U;
    }

    if (submit_needed == 0U)
    {
        return 1U;
    }

    ok = app_display_runtime_request_thermal_present_async(slot->gray_frame, token);
    if (ok != 0U)
    {
        redpic1_thermal_frame_slot_enter_critical();
        s_last_submitted_token = token;
        s_last_submitted_valid = 1U;
        redpic1_thermal_frame_slot_exit_critical();
    }

    return ok;
}

void redpic1_thermal_frame_slot_try_submit_if_possible(void)
{
    redpic1_thermal_frame_slot_try_submit_after_gap_close();
}

void redpic1_thermal_frame_slot_cancel_pending_present(void)
{
    app_display_runtime_cancel_thermal_present_async();
    redpic1_thermal_frame_slot_clear_submitted_token();
}

void redpic1_thermal_frame_slot_drop_non_inflight(void)
{
    uint8_t slot_index = 0U;

    redpic1_thermal_frame_slot_enter_critical();
    s_front_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    s_ready_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;

    for (slot_index = 0U; slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT; ++slot_index)
    {
        if (slot_index == s_inflight_slot_index &&
            s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT)
        {
            continue;
        }

        s_frame_slots[slot_index].valid = 0U;
        s_frame_slots[slot_index].frame_seq = 0U;
        s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FREE;
    }

    if (s_inflight_slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT ||
        s_frame_slots[s_inflight_slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT)
    {
        s_inflight_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
        s_frame_sequence = 0U;
    }
    redpic1_thermal_frame_slot_clear_submitted_token_locked();
    redpic1_thermal_frame_slot_exit_critical();
}

void redpic1_thermal_frame_slot_reset(void)
{
    uint8_t slot_index = 0U;

    redpic1_thermal_frame_slot_enter_critical();
    s_front_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    s_ready_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    s_inflight_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    redpic1_thermal_frame_slot_clear_submitted_token_locked();
    for (slot_index = 0U; slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT; ++slot_index)
    {
        s_frame_slots[slot_index].valid = 0U;
        s_frame_slots[slot_index].frame_seq = 0U;
        s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FREE;
    }
    redpic1_thermal_frame_slot_exit_critical();

    s_frame_sequence = 0U;
}
