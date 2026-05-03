/**
 * @file    thermal_frame_slot.c
 * @brief   热成像帧槽位管理模块
 * @note    本模块实现三槽位异步送显状态机，解耦传感器采集与屏幕刷新。
 *
 * @par 槽位状态机
 *      ```
 *      FREE ──acquire──→ WRITING ──publish──→ READY ──claim──→ INFLIGHT ──done──→ FRONT
 *        ↑                  │                   │                 │                │
 *        └────release───────┘                   └──cancel/expire──┘                │
 *        ↑                                                                        │
 *        └────────────────────────────free─────────────────────────────────────────┘
 *      ```
 *
 * @par 槽位生命周期
 *      - FREE: 空闲状态，可被 acquire
 *      - WRITING: 正在写入帧数据（thermal task 持有）
 *      - READY: 帧数据就绪，等待 display runtime 领取
 *      - INFLIGHT: 已提交到 display runtime，等待 DMA 传输完成
 *      - FRONT: 最新稳定帧，可被强制刷新重送
 *
 * @par Token 机制
 *      Token = (frame_seq << 8) | slot_index，用于 display runtime 回调时
 *      精确匹配槽位，防止帧序号不一致导致的误操作。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "thermal_frame_slot.h"

#include <string.h>

#include "app_display_runtime.h"
#include "app_perf_baseline.h"
#include "redpic1_thermal.h"

/* =========================================================================
 *  2. 内部宏定义
 * ======================================================================= */

#define THERMAL_FRAME_SLOT_TOKEN_SHIFT      8U      /**< Token 中帧序号左移位数  */
#define THERMAL_FRAME_SLOT_TOKEN_SLOT_MASK  0xFFU   /**< Token 中槽位索引掩码   */

/* =========================================================================
 *  3. 模块级静态变量
 * ======================================================================= */

static redpic1_thermal_frame_slot_ops_t s_ops;                              /**< 回调函数集    */
static redpic1_thermal_frame_slot_t     s_frame_slots[REDPIC1_THERMAL_FRAME_SLOT_COUNT]; /**< 槽位数组 */
static uint32_t     s_frame_sequence        = 0U;                           /**< 帧序号分配器  */
static uint8_t      s_front_slot_index      = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE; /**< FRONT 槽位索引 */
static uint8_t      s_ready_slot_index      = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE; /**< READY 槽位索引 */
static uint8_t      s_inflight_slot_index   = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE; /**< INFLIGHT 槽位索引 */
static uintptr_t    s_last_submitted_token  = 0U;                           /**< 上次提交 Token */
static uint8_t      s_last_submitted_valid  = 0U;                           /**< Token 有效性  */

/* =========================================================================
 *  4. 内部函数实现 —— 回调代理
 * ======================================================================= */

/** @brief  进入临界区（通过回调） */
static void redpic1_thermal_frame_slot_enter_critical(void)
{
    if (s_ops.enter_critical != 0)
    {
        s_ops.enter_critical();
    }
}

/** @brief  退出临界区（通过回调） */
static void redpic1_thermal_frame_slot_exit_critical(void)
{
    if (s_ops.exit_critical != 0)
    {
        s_ops.exit_critical();
    }
}

/** @brief  获取系统 tick（ms，通过回调） */
static uint32_t redpic1_thermal_frame_slot_get_tick_ms(void)
{
    if (s_ops.get_tick_ms != 0)
    {
        return s_ops.get_tick_ms();
    }
    return 0U;
}

/** @brief  获取帧过期阈值（ms，通过回调） */
static uint32_t redpic1_thermal_frame_slot_get_expired_threshold_ms(void)
{
    if (s_ops.get_expired_frame_threshold_ms != 0)
    {
        return s_ops.get_expired_frame_threshold_ms();
    }
    return 0U;
}

/** @brief  查询模块使能状态（通过回调） */
static uint8_t redpic1_thermal_frame_slot_run_enabled(void)
{
    if (s_ops.run_enabled != 0)
    {
        return s_ops.run_enabled();
    }
    return 1U;
}

/** @brief  查询送显暂停状态（通过回调） */
static uint8_t redpic1_thermal_frame_slot_display_paused(void)
{
    if (s_ops.display_paused != 0)
    {
        return s_ops.display_paused();
    }
    return 0U;
}

/** @brief  查询叠加层持有状态（通过回调） */
static uint8_t redpic1_thermal_frame_slot_overlay_hold(void)
{
    if (s_ops.overlay_hold != 0)
    {
        return s_ops.overlay_hold();
    }
    return 0U;
}

/* =========================================================================
 *  5. 内部函数实现 —— Token 编解码
 * ======================================================================= */

/**
 * @brief  构造 Token
 * @param  slot_index — 槽位索引
 * @param  frame_seq  — 帧序号
 * @return Token 值
 */
static uintptr_t redpic1_thermal_frame_slot_make_token(uint8_t slot_index,
                                                       uint32_t frame_seq)
{
    return ((((uintptr_t)frame_seq) << THERMAL_FRAME_SLOT_TOKEN_SHIFT) |
            (uintptr_t)slot_index);
}

/**
 * @brief  从 Token 提取槽位索引
 * @param  token — Token 值
 * @return 槽位索引
 */
static uint8_t redpic1_thermal_frame_slot_token_slot_index(uintptr_t token)
{
    return (uint8_t)(token & THERMAL_FRAME_SLOT_TOKEN_SLOT_MASK);
}

/**
 * @brief  从 Token 提取帧序号
 * @param  token — Token 值
 * @return 帧序号
 */
static uint32_t redpic1_thermal_frame_slot_token_frame_seq(uintptr_t token)
{
    return (uint32_t)(token >> THERMAL_FRAME_SLOT_TOKEN_SHIFT);
}

/* =========================================================================
 *  6. 内部函数实现 —— Token 状态管理
 * ======================================================================= */

/** @brief  清除已提交 Token（临界区内调用） */
static void redpic1_thermal_frame_slot_clear_submitted_token_locked(void)
{
    s_last_submitted_token = 0U;
    s_last_submitted_valid = 0U;
}

/** @brief  清除已提交 Token（带临界区保护） */
static void redpic1_thermal_frame_slot_clear_submitted_token(void)
{
    redpic1_thermal_frame_slot_enter_critical();
    redpic1_thermal_frame_slot_clear_submitted_token_locked();
    redpic1_thermal_frame_slot_exit_critical();
}

/* =========================================================================
 *  7. 内部函数实现 —— 槽位释放
 * ======================================================================= */

/**
 * @brief  释放指定槽位（临界区内调用）
 * @note   将槽位状态重置为 FREE，并清除相关索引引用。
 * @param  slot_index — 槽位索引
 */
static void redpic1_thermal_frame_slot_free_locked(uint8_t slot_index)
{
    if (slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT)
    {
        return;
    }

    s_frame_slots[slot_index].valid      = 0U;
    s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FREE;

    /* 清除索引引用 */
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

    /* 清除匹配的已提交 Token */
    if (s_last_submitted_valid != 0U &&
        redpic1_thermal_frame_slot_token_slot_index(s_last_submitted_token) == slot_index)
    {
        redpic1_thermal_frame_slot_clear_submitted_token_locked();
    }
}

/* =========================================================================
 *  8. 内部函数实现 —— FRONT 槽位获取
 * ======================================================================= */

/**
 * @brief  获取当前 FRONT 槽位（带临界区保护）
 * @return FRONT 槽位指针；无有效 FRONT 槽位时返回 NULL
 */
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

/* =========================================================================
 *  9. 内部函数实现 —— 帧过期判定
 * ======================================================================= */

/**
 * @brief  判断 READY 槽位的帧是否已过期（临界区内调用）
 * @param  slot_index — 槽位索引
 * @param  now_ms     — 当前时间（ms）
 * @retval 1 — 已过期；0 — 未过期或无效
 */
static uint8_t redpic1_thermal_frame_slot_ready_frame_expired_locked(
    uint8_t slot_index, uint32_t now_ms)
{
    uint32_t threshold_ms    = 0U;
    uint32_t capture_tick_ms = 0U;

    if (slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT ||
        s_frame_slots[slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_READY ||
        s_frame_slots[slot_index].valid == 0U)
    {
        return 0U;
    }

    capture_tick_ms = s_frame_slots[slot_index].capture_tick_ms;
    threshold_ms    = redpic1_thermal_frame_slot_get_expired_threshold_ms();

    if (capture_tick_ms == 0U || threshold_ms == 0U)
    {
        return 0U;
    }

    return ((uint32_t)(now_ms - capture_tick_ms) > threshold_ms) ? 1U : 0U;
}

/* =========================================================================
 *  10. 内部函数实现 —— READY 槽位取消
 * ======================================================================= */

/**
 * @brief  通过 Token 取消 READY 槽位
 * @param  token — Token 值
 * @retval 1 — 取消成功；0 — 未匹配
 */
static uint8_t redpic1_thermal_frame_slot_cancel_ready_by_token(uintptr_t token)
{
    uint8_t  slot_index = redpic1_thermal_frame_slot_token_slot_index(token);
    uint32_t frame_seq  = redpic1_thermal_frame_slot_token_frame_seq(token);
    uint8_t  cancelled  = 0U;

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

/* =========================================================================
 *  11. 内部函数实现 —— 提交条件检查
 * ======================================================================= */

/**
 * @brief  检查是否可以提交 READY 槽位到 display runtime（临界区内调用）
 * @retval 1 — 可以提交；0 — 不可提交
 */
static uint8_t redpic1_thermal_frame_slot_can_submit_locked(void)
{
    /* 前置条件检查 */
    if (redpic1_thermal_frame_slot_run_enabled() == 0U ||
        redpic1_thermal_frame_slot_display_paused() != 0U ||
        redpic1_thermal_frame_slot_overlay_hold() != 0U ||
        s_inflight_slot_index != REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE)
    {
        return 0U;
    }

    /* READY 槽位有效性检查 */
    if (s_ready_slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT ||
        s_frame_slots[s_ready_slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_READY)
    {
        return 0U;
    }

    return 1U;
}

/**
 * @brief  在 display done 回调后尝试提交
 * @note   当 inflight 槽位完成释放后，检查是否有新的 READY 槽位可以提交。
 */
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

/* =========================================================================
 *  12. 内部函数实现 —— Display Runtime 回调
 * ======================================================================= */

/**
 * @brief  display runtime 领取回调（claim）
 * @note   当 display runtime 准备传输时调用，将 READY 槽位提升为 INFLIGHT。
 *         帧过期时自动取消。
 * @param  token       — Token 值
 * @param  gray_frame  — 输出：灰度帧指针
 * @retval 1 — 领取成功；0 — 领取失败
 */
static uint8_t redpic1_thermal_frame_slot_try_claim_present(
    uintptr_t token, uint8_t **gray_frame)
{
    uint8_t  slot_index = redpic1_thermal_frame_slot_token_slot_index(token);
    uint32_t frame_seq  = redpic1_thermal_frame_slot_token_frame_seq(token);
    uint32_t now_ms     = redpic1_thermal_frame_slot_get_tick_ms();
    uint8_t  claimed    = 0U;
    uint8_t  expired    = 0U;

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
        /* 帧过期检查 */
        if (redpic1_thermal_frame_slot_ready_frame_expired_locked(slot_index, now_ms) != 0U)
        {
            expired = 1U;
        }
        else
        {
            /* READY → INFLIGHT */
            s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT;
            s_ready_slot_index    = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
            s_inflight_slot_index = slot_index;
            *gray_frame = s_frame_slots[slot_index].gray_frame;
            claimed = 1U;
        }
    }
    redpic1_thermal_frame_slot_exit_critical();

    /* 过期：取消该 READY 槽位 */
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

/**
 * @brief  display runtime 完成回调（done）
 * @note   当 DMA 传输完成或取消时调用，更新槽位状态并记录性能数据。
 * @param  token  — Token 值
 * @param  status — 完成状态
 */
static void redpic1_thermal_frame_slot_handle_display_done(
    uintptr_t token, app_display_thermal_done_status_t status)
{
    uint8_t  slot_index          = redpic1_thermal_frame_slot_token_slot_index(token);
    uint32_t frame_seq           = redpic1_thermal_frame_slot_token_frame_seq(token);
    uint32_t display_done_tick_ms = redpic1_thermal_frame_slot_get_tick_ms();
    uint32_t display_age_ms      = 0U;
    uint8_t  note_cancel         = 0U;
    uint8_t  note_display_age    = 0U;
    uint8_t  note_done_ok        = 0U;
    uint8_t  note_done_error     = 0U;
    uint8_t  note_done_cancel    = 0U;

    redpic1_thermal_frame_slot_enter_critical();

    if (slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
        s_frame_slots[slot_index].frame_seq == frame_seq)
    {
        switch (status)
        {
        /* ---- 传输成功 ---- */
        case APP_DISPLAY_THERMAL_DONE_OK:
            if (s_inflight_slot_index == slot_index &&
                s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT)
            {
                s_inflight_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;

                /* 计算显示延迟 */
                if (s_frame_slots[slot_index].capture_tick_ms != 0U)
                {
                    display_age_ms = (uint32_t)(display_done_tick_ms -
                                                s_frame_slots[slot_index].capture_tick_ms);
                    note_display_age = 1U;
                }

                /* INFLIGHT → FRONT */
                if (redpic1_thermal_frame_slot_run_enabled() != 0U)
                {
                    uint8_t old_front = s_front_slot_index;

                    if (old_front < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
                        old_front != slot_index)
                    {
                        redpic1_thermal_frame_slot_free_locked(old_front);
                    }

                    s_frame_slots[slot_index].valid      = 1U;
                    s_frame_slots[slot_index].slot_state  = REDPIC1_THERMAL_FRAME_SLOT_FRONT;
                    s_front_slot_index = slot_index;
                }
                else
                {
                    redpic1_thermal_frame_slot_free_locked(slot_index);
                }

                note_done_ok = 1U;
            }
            break;

        /* ---- 传输取消 ---- */
        case APP_DISPLAY_THERMAL_DONE_CANCELLED:
            if ((s_ready_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY) ||
                (s_inflight_slot_index == slot_index &&
                 s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT))
            {
                redpic1_thermal_frame_slot_free_locked(slot_index);
                note_cancel      = 1U;
                note_done_cancel = 1U;
            }
            break;

        /* ---- 传输错误 ---- */
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

    /* 记录性能事件 */
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

    /* 清除匹配的已提交 Token */
    redpic1_thermal_frame_slot_enter_critical();
    if (s_last_submitted_valid != 0U && s_last_submitted_token == token)
    {
        redpic1_thermal_frame_slot_clear_submitted_token_locked();
    }
    redpic1_thermal_frame_slot_exit_critical();

    /* 传输成功后尝试提交下一个 READY 槽位 */
    if (note_done_ok != 0U)
    {
        redpic1_thermal_frame_slot_try_submit_after_gap_close();
    }
}

/* =========================================================================
 *  13. 公共接口实现 —— 初始化与绑定
 * ======================================================================= */

/**
 * @brief  初始化帧槽位管理模块
 * @param  ops — 回调函数集指针
 */
void redpic1_thermal_frame_slot_init(const redpic1_thermal_frame_slot_ops_t *ops)
{
    memset(&s_ops, 0, sizeof(s_ops));
    if (ops != 0)
    {
        s_ops = *ops;
    }

    redpic1_thermal_frame_slot_reset();
}

/**
 * @brief  绑定 display runtime 回调
 * @note   注册 claim 和 done 回调函数。
 */
void redpic1_thermal_frame_slot_bind_display_runtime(void)
{
    app_display_runtime_set_thermal_present_claim_callback(
        redpic1_thermal_frame_slot_try_claim_present);
    app_display_runtime_set_thermal_present_done_callback(
        redpic1_thermal_frame_slot_handle_display_done);
}

/* =========================================================================
 *  14. 公共接口实现 —— 槽位获取与释放
 * ======================================================================= */

/**
 * @brief  获取一个空闲的 back 槽位（用于写入新帧数据）
 * @note   优先使用 FREE 槽位；无 FREE 时替换最旧的 READY 槽位。
 * @return 槽位指针；无可用槽位时返回 NULL
 */
redpic1_thermal_frame_slot_t *redpic1_thermal_frame_slot_acquire_back(void)
{
    uint8_t slot_index  = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    uint8_t note_replace = 0U;

    redpic1_thermal_frame_slot_enter_critical();

    /* 查找 FREE 槽位 */
    for (slot_index = 0U; slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT; ++slot_index)
    {
        if (s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_FREE)
        {
            break;
        }
    }

    /* 无 FREE 槽位：替换最旧的 READY 槽位 */
    if (slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
        s_ready_slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT)
    {
        slot_index = s_ready_slot_index;
        redpic1_thermal_frame_slot_free_locked(slot_index);
        note_replace = 1U;
    }

    /* 标记为 WRITING */
    if (slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT)
    {
        s_frame_slots[slot_index].valid         = 0U;
        s_frame_slots[slot_index].ready_tick_ms = 0U;
        s_frame_slots[slot_index].slot_state    = REDPIC1_THERMAL_FRAME_SLOT_WRITING;
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

/**
 * @brief  释放 back 槽位（写入失败时调用）
 * @param  slot — 槽位指针
 */
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

/* =========================================================================
 *  15. 公共接口实现 —— 槽位发布
 * ======================================================================= */

/**
 * @brief  发布 back 槽位为 READY（写入完成后调用）
 * @note   将 WRITING → READY，并替换旧的 READY 槽位。
 * @param  slot — 槽位指针
 */
void redpic1_thermal_frame_slot_publish_back(redpic1_thermal_frame_slot_t *slot)
{
    uint8_t publish_index = 0U;
    uint8_t old_ready     = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    uint8_t note_replace  = 0U;
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
        /* 替换旧的 READY 槽位 */
        old_ready = s_ready_slot_index;
        if (old_ready < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
            old_ready != publish_index &&
            s_frame_slots[old_ready].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY)
        {
            redpic1_thermal_frame_slot_free_locked(old_ready);
            note_replace = 1U;
        }

        /* WRITING → READY */
        s_frame_slots[publish_index].valid         = 1U;
        s_frame_slots[publish_index].ready_tick_ms = ready_tick_ms;
        s_frame_slots[publish_index].slot_state    = REDPIC1_THERMAL_FRAME_SLOT_READY;
        s_ready_slot_index = publish_index;
    }
    redpic1_thermal_frame_slot_exit_critical();

    if (note_replace != 0U)
    {
        app_perf_baseline_record_thermal_ready_replace();
    }
}

/* =========================================================================
 *  16. 公共接口实现 —— FRONT 帧重送
 * ======================================================================= */

/**
 * @brief  重送 FRONT 帧到 LCD
 * @note   仅在非暂停、非叠加持有状态下执行。
 */
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

/* =========================================================================
 *  17. 公共接口实现 —— 帧序号分配
 * ======================================================================= */

/**
 * @brief  分配一个新的帧序号
 * @return 帧序号（单调递增）
 */
uint32_t redpic1_thermal_frame_slot_allocate_sequence(void)
{
    uint32_t frame_seq = 0U;

    redpic1_thermal_frame_slot_enter_critical();
    frame_seq = ++s_frame_sequence;
    redpic1_thermal_frame_slot_exit_critical();

    return frame_seq;
}

/* =========================================================================
 *  18. 公共接口实现 —— 提交到 Display Runtime
 * ======================================================================= */

/**
 * @brief  提交最新的 READY 槽位到 display runtime
 * @note   生成 Token 并异步提交，避免重复提交。
 * @retval 1 — 提交成功或已提交；0 — 无 READY 槽位或提交失败
 */
uint8_t redpic1_thermal_frame_slot_submit_latest_ready(void)
{
    redpic1_thermal_frame_slot_t *slot = 0;
    uintptr_t token         = 0U;
    uint8_t  submit_needed  = 0U;
    uint8_t  ok             = 0U;

    redpic1_thermal_frame_slot_enter_critical();

    if (s_ready_slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT &&
        s_frame_slots[s_ready_slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_READY)
    {
        slot  = &s_frame_slots[s_ready_slot_index];
        token = redpic1_thermal_frame_slot_make_token(s_ready_slot_index, slot->frame_seq);

        /* 检查是否已提交过相同 Token */
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

    /* 已提交：直接返回成功 */
    if (submit_needed == 0U)
    {
        return 1U;
    }

    /* 异步提交到 display runtime */
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

/**
 * @brief  尝试提交（如果有可用 READY 槽位）
 */
void redpic1_thermal_frame_slot_try_submit_if_possible(void)
{
    redpic1_thermal_frame_slot_try_submit_after_gap_close();
}

/* =========================================================================
 *  19. 公共接口实现 —— 取消与丢弃
 * ======================================================================= */

/**
 * @brief  取消待处理的异步送显请求
 */
void redpic1_thermal_frame_slot_cancel_pending_present(void)
{
    app_display_runtime_cancel_thermal_present_async();
    redpic1_thermal_frame_slot_clear_submitted_token();
}

/**
 * @brief  丢弃所有非 INFLIGHT 槽位
 * @note   保留 INFLIGHT 槽位（DMA 正在传输），清除 FRONT 和 READY。
 */
void redpic1_thermal_frame_slot_drop_non_inflight(void)
{
    uint8_t slot_index = 0U;

    redpic1_thermal_frame_slot_enter_critical();

    s_front_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    s_ready_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;

    for (slot_index = 0U; slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT; ++slot_index)
    {
        /* 跳过 INFLIGHT 槽位 */
        if (slot_index == s_inflight_slot_index &&
            s_frame_slots[slot_index].slot_state == REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT)
        {
            continue;
        }

        s_frame_slots[slot_index].valid      = 0U;
        s_frame_slots[slot_index].frame_seq  = 0U;
        s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FREE;
    }

    /* INFLIGHT 状态一致性检查 */
    if (s_inflight_slot_index >= REDPIC1_THERMAL_FRAME_SLOT_COUNT ||
        s_frame_slots[s_inflight_slot_index].slot_state != REDPIC1_THERMAL_FRAME_SLOT_INFLIGHT)
    {
        s_inflight_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
        s_frame_sequence = 0U;
    }

    redpic1_thermal_frame_slot_clear_submitted_token_locked();
    redpic1_thermal_frame_slot_exit_critical();
}

/* =========================================================================
 *  20. 公共接口实现 —— 完整重置
 * ======================================================================= */

/**
 * @brief  完整重置所有槽位状态
 * @note   清除所有索引、Token 和槽位数据。
 */
void redpic1_thermal_frame_slot_reset(void)
{
    uint8_t slot_index = 0U;

    redpic1_thermal_frame_slot_enter_critical();

    s_front_slot_index    = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    s_ready_slot_index    = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    s_inflight_slot_index = REDPIC1_THERMAL_FRAME_SLOT_INDEX_NONE;
    redpic1_thermal_frame_slot_clear_submitted_token_locked();

    for (slot_index = 0U; slot_index < REDPIC1_THERMAL_FRAME_SLOT_COUNT; ++slot_index)
    {
        s_frame_slots[slot_index].valid      = 0U;
        s_frame_slots[slot_index].frame_seq  = 0U;
        s_frame_slots[slot_index].slot_state = REDPIC1_THERMAL_FRAME_SLOT_FREE;
    }
    redpic1_thermal_frame_slot_exit_critical();

    s_frame_sequence = 0U;
}
