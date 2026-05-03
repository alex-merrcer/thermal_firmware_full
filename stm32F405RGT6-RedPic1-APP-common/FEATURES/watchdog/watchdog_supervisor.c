/**
 * @file    watchdog_supervisor.c
 * @brief   看门狗监管器 —— 多任务进度监控与独立看门狗喂狗
 * @note    本模块实现基于进度位掩码的多任务健康监控机制。
 *          通过独立看门狗（IWDG）实现硬件级故障恢复。
 *
 * @par 监控机制
 *      1. 每个注册任务分配一个唯一的进度位（bit mask）
 *      2. 每个监控窗口开始时清除进度位
 *      3. 各任务在完成工作后上报自己的进度位
 *      4. 窗口结束时检查：所有必需任务是否都已上报进度
 *      5. 健康时按间隔喂 IWDG；不健康时停止喂狗，等待硬件复位
 *
 * @par 故障标志
 *      除了进度缺失，任务还可以主动上报故障标志：
 *      - WATCHDOG_FAULT_KEY_STUCK:     按键卡死
 *      - WATCHDOG_FAULT_UART_ERROR:    UART 硬件错误
 *      - WATCHDOG_FAULT_UART_OVERFLOW: UART 接收缓冲区溢出
 *
 * @par 复位快照
 *      当检测到不健康状态时，自动保存复位快照：
 *      - 时间戳、缺失进度掩码、故障标志
 *      - 系统复位后可通过 get_reset_snapshot() 读取诊断信息
 *
 * @par STOP 模式保护
 *      从 STOP 模式唤醒后的首周期不检查健康状态，
 *      避免因唤醒延迟导致的误报。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "watchdog_supervisor.h"

#include <string.h>

#include "iwdg.h"
#include "power_manager.h"
#include "stm32f4xx_rtc.h"

/* =========================================================================
 *  2. 内部宏定义
 * ======================================================================= */

/** 默认喂狗间隔（ms） */
#define WATCHDOG_SUPERVISOR_DEFAULT_FEED_MS  1000UL

/** 最大可注册任务数 */
#define WATCHDOG_SUPERVISOR_MAX_TASKS        16U

/** 息屏空闲态下允许延后上报的进度位：由低优先级异步任务驱动 */
#define WATCHDOG_SUPERVISOR_SCREEN_OFF_IDLE_OPTIONAL_MASK \
    ((1UL << 7) | (1UL << 8))

/* =========================================================================
 *  3. 模块级静态变量
 * ======================================================================= */

static watchdog_supervisor_task_t s_tasks[WATCHDOG_SUPERVISOR_MAX_TASKS]; /**< 任务注册表     */
static uint8_t  s_task_count              = 0U;   /**< 已注册任务数                     */
static uint32_t s_required_progress_mask  = 0U;   /**< 所有必需任务的进度位掩码（OR）   */
static uint32_t s_feed_interval_ms        = WATCHDOG_SUPERVISOR_DEFAULT_FEED_MS; /**< 喂狗间隔 */
static uint32_t s_last_feed_ms            = 0U;   /**< 上次喂狗时间戳（ms）             */
static uint32_t s_window_progress_mask    = 0U;   /**< 当前窗口已上报的进度位掩码       */
static uint32_t s_window_fault_flags      = 0U;   /**< 当前窗口已上报的故障标志         */
static uint32_t s_last_missing_progress_mask = 0U; /**< 最近一次检查的缺失进度掩码      */
static uint32_t s_last_fault_flags        = 0U;   /**< 最近一次检查的故障标志           */
static uint8_t  s_stop_entry_ready        = 1U;   /**< STOP 模式准入就绪标志            */
static uint8_t  s_skip_health_check_once  = 0U;   /**< STOP 唤醒后首周期豁免标志        */

static watchdog_supervisor_reset_snapshot_t s_reset_snapshot; /**< 复位诊断快照      */

/* =========================================================================
 *  3a. C5 修复：备份寄存器持久化（IWDG 复位后数据不丢失）
 * ======================================================================= */

/** 备份寄存器分配：DR5=tick_ms, DR6=missing_mask, DR7=fault_flags, DR8=valid+魔数 */
#define WDG_BKP_REG_TICK        RTC_BKP_DR5
#define WDG_BKP_REG_MISSING     RTC_BKP_DR6
#define WDG_BKP_REG_FAULT       RTC_BKP_DR7
#define WDG_BKP_REG_VALID       RTC_BKP_DR8

/** 快照有效性魔数（用于判断备份寄存器中是否有有效快照） */
#define WDG_BKP_SNAPSHOT_MAGIC  0x57444753UL  /* "WDGS" */

/* =========================================================================
 *  4. 内部函数实现 —— 任务名比较
 * ======================================================================= */

/**
 * @brief  比较两个任务名是否相等
 * @param  lhs — 字符串指针 A
 * @param  rhs — 字符串指针 B
 * @retval 1 — 相等；0 — 不相等或指针无效
 */
static uint8_t watchdog_supervisor_task_name_equal(const char *lhs, const char *rhs)
{
    if (lhs == 0 || rhs == 0)
    {
        return 0U;
    }

    return (strcmp(lhs, rhs) == 0) ? 1U : 0U;
}

/* =========================================================================
 *  5. 内部函数实现 —— 窗口健康检查
 * ======================================================================= */

/**
 * @brief  检查当前监控窗口是否健康
 * @note   健康条件：
 *         1. 所有必需任务的进度位均已上报（缺失掩码为 0）
 *         2. 无故障标志
 * @retval 1 — 健康；0 — 不健康
 */
static uint32_t watchdog_supervisor_get_required_progress_mask(void)
{
    uint32_t required_mask = s_required_progress_mask;

    if (power_manager_get_state() == POWER_STATE_SCREEN_OFF_IDLE)
    {
        required_mask &= ~WATCHDOG_SUPERVISOR_SCREEN_OFF_IDLE_OPTIONAL_MASK;
    }

    return required_mask;
}

static uint8_t watchdog_supervisor_window_is_healthy(void)
{
    uint32_t required_mask = watchdog_supervisor_get_required_progress_mask();

    /* 计算缺失的进度位（必需但未上报的） */
    s_last_missing_progress_mask =
        (required_mask & (~s_window_progress_mask));
    s_last_fault_flags = s_window_fault_flags;

    return ((s_last_missing_progress_mask == 0U) &&
            (s_last_fault_flags == 0U)) ? 1U : 0U;
}

/* =========================================================================
 *  6. 内部函数实现 —— 复位快照捕获
 * ======================================================================= */

/**
 * @brief  捕获复位诊断快照
 * @note   在检测到不健康状态时调用，保存当前诊断信息。
 *         系统复位后可通过 get_reset_snapshot() 读取。
 * @param  tick_ms — 当前系统时间戳（ms）
 */
static void watchdog_supervisor_capture_reset_snapshot(uint32_t tick_ms)
{
    s_reset_snapshot.valid = 1U;
    s_reset_snapshot.tick_ms = tick_ms;
    s_reset_snapshot.missing_progress_mask = s_last_missing_progress_mask;
    s_reset_snapshot.fault_flags = s_last_fault_flags;

    /* C5 修复：同步写入 RTC 备份寄存器，IWDG 复位后数据不丢失 */
    RTC_WriteBackupRegister(WDG_BKP_REG_TICK,    tick_ms);
    RTC_WriteBackupRegister(WDG_BKP_REG_MISSING, s_last_missing_progress_mask);
    RTC_WriteBackupRegister(WDG_BKP_REG_FAULT,   s_last_fault_flags);
    RTC_WriteBackupRegister(WDG_BKP_REG_VALID,   WDG_BKP_SNAPSHOT_MAGIC);
}

/* =========================================================================
 *  7. 公共接口实现 —— 初始化
 * ======================================================================= */

/**
 * @brief  初始化看门狗监管器
 * @note   清除所有任务注册、进度标志和故障标志，立即喂一次 IWDG。
 * @param  feed_interval_ms — 喂狗间隔（ms），0 使用默认值（1000ms）
 */
void watchdog_supervisor_init(uint32_t feed_interval_ms)
{
    if (feed_interval_ms == 0U)
    {
        feed_interval_ms = WATCHDOG_SUPERVISOR_DEFAULT_FEED_MS;
    }

    memset(s_tasks, 0, sizeof(s_tasks));

    /* C5 修复：从备份寄存器恢复复位快照（IWDG 复位后 SRAM 丢失，但备份寄存器保持） */
    if (RTC_ReadBackupRegister(WDG_BKP_REG_VALID) == WDG_BKP_SNAPSHOT_MAGIC)
    {
        s_reset_snapshot.valid = 1U;
        s_reset_snapshot.tick_ms = RTC_ReadBackupRegister(WDG_BKP_REG_TICK);
        s_reset_snapshot.missing_progress_mask = RTC_ReadBackupRegister(WDG_BKP_REG_MISSING);
        s_reset_snapshot.fault_flags = RTC_ReadBackupRegister(WDG_BKP_REG_FAULT);
        /* 恢复后清除有效性标记，防止下次正常复位时误读 */
        RTC_WriteBackupRegister(WDG_BKP_REG_VALID, 0U);
    }
    else
    {
        memset(&s_reset_snapshot, 0, sizeof(s_reset_snapshot));
    }

    s_task_count               = 0U;
    s_required_progress_mask   = 0U;
    s_feed_interval_ms         = feed_interval_ms;
    s_last_feed_ms             = power_manager_get_tick_ms();
    s_window_progress_mask     = 0U;
    s_window_fault_flags       = 0U;
    s_last_missing_progress_mask = 0U;
    s_last_fault_flags         = 0U;
    s_stop_entry_ready         = 1U;
    s_skip_health_check_once   = 0U;

    /* 初始化时立即喂狗，防止启动期间超时复位 */
    IWDG_Feed();
}

/* =========================================================================
 *  8. 公共接口实现 —— 任务注册
 * ======================================================================= */

/**
 * @brief  注册一个监控任务
 * @note   注册规则：
 *         1. 任务名和进度掩码不能为空
 *         2. 同名同掩码的重复注册视为成功（幂等）
 *         3. 进度掩码冲突（与其他任务重叠）时注册失败
 *         4. 超过最大任务数时注册失败
 *         5. required=1 的任务会被加入必需进度掩码
 * @param  task — 任务描述结构体指针
 * @retval 1 — 注册成功；0 — 注册失败
 */
uint8_t watchdog_supervisor_register_task(const watchdog_supervisor_task_t *task)
{
    uint8_t i = 0U;

    if (task == 0 || task->name == 0 || task->progress_mask == 0U)
    {
        return 0U;
    }

    /* 检查重复注册和掩码冲突 */
    for (i = 0U; i < s_task_count; ++i)
    {
        /* 同名同掩码：幂等注册 */
        if (watchdog_supervisor_task_name_equal(s_tasks[i].name, task->name) != 0U &&
            s_tasks[i].progress_mask == task->progress_mask)
        {
            return 1U;
        }

        /* 掩码冲突：不同任务使用了相同的进度位 */
        if ((s_tasks[i].progress_mask & task->progress_mask) != 0U)
        {
            return 0U;
        }
    }

    /* 容量检查 */
    if (s_task_count >= WATCHDOG_SUPERVISOR_MAX_TASKS)
    {
        return 0U;
    }

    /* 注册任务 */
    s_tasks[s_task_count] = *task;
    if (task->required != 0U)
    {
        s_required_progress_mask |= task->progress_mask;
    }
    s_task_count++;
    return 1U;
}

/* =========================================================================
 *  9. 公共接口实现 —— 窗口管理
 * ======================================================================= */

/**
 * @brief  开启一个新的监控窗口
 * @note   清除当前窗口的进度位和故障标志。
 */
void watchdog_supervisor_begin_window(void)
{
    s_window_progress_mask = 0U;
    s_window_fault_flags   = 0U;
}

/**
 * @brief  标记指定任务的进度
 * @param  progress_mask — 进度位掩码（WATCHDOG_PROGRESS_xxx）
 */
void watchdog_supervisor_mark_progress(uint32_t progress_mask)
{
    s_window_progress_mask |= progress_mask;
}

/**
 * @brief  上报故障标志
 * @param  fault_flags — 故障位掩码（WATCHDOG_FAULT_xxx）
 */
void watchdog_supervisor_report_fault_flags(uint32_t fault_flags)
{
    s_window_fault_flags |= fault_flags;
}

/* =========================================================================
 *  10. 公共接口实现 —— STOP 模式支持
 * ======================================================================= */

/**
 * @brief  通知看门狗即将从 STOP 模式唤醒
 * @note   将 s_stop_entry_ready 置为 0，使首周期跳过健康检查。
 */
void watchdog_supervisor_note_stop_wake(void)
{
    s_stop_entry_ready = 0U;
    s_skip_health_check_once = 1U;
}

/* =========================================================================
 *  11. 公共接口实现 —— 调度与喂狗
 * ======================================================================= */

/**
 * @brief  执行一次看门狗调度步骤
 * @note   建议在主循环中定期调用。流程：
 *         1. 检查当前窗口健康状态
 *         2. 更新 STOP 准入标志
 *         3. 不健康时捕获复位快照并返回（不喂狗 → IWDG 超时复位）
 *         4. 健康时按间隔喂 IWDG
 */
void watchdog_supervisor_step(void)
{
    uint32_t now_ms = power_manager_get_tick_ms();
    uint8_t healthy = 0U;

    if (s_skip_health_check_once != 0U)
    {
        s_skip_health_check_once = 0U;
        s_stop_entry_ready = 0U;
        s_last_missing_progress_mask = 0U;
        s_last_fault_flags = 0U;
        return;
    }

    healthy = watchdog_supervisor_window_is_healthy();

    /* 更新 STOP 准入状态（仅健康时允许进入 STOP） */
    s_stop_entry_ready = healthy;

    if (healthy == 0U)
    {
        /* 不健康：捕获快照但不喂狗，等待 IWDG 硬件复位 */
        watchdog_supervisor_capture_reset_snapshot(now_ms);
        return;
    }

    /* 健康：按间隔喂狗 */
    if ((now_ms - s_last_feed_ms) >= s_feed_interval_ms)
    {
        s_last_feed_ms = now_ms;
        IWDG_Feed();
    }
}

/**
 * @brief  强制立即喂狗
 * @note   用于特殊场景（如 OTA 升级前）防止意外复位。
 */
void watchdog_supervisor_force_feed(void)
{
    s_last_feed_ms = power_manager_get_tick_ms();
    IWDG_Feed();
}

/* =========================================================================
 *  12. 公共接口实现 —— 状态查询
 * ======================================================================= */

/**
 * @brief  查询当前监控窗口是否健康
 * @retval 1 — 健康；0 — 不健康
 */
uint8_t watchdog_supervisor_is_healthy(void)
{
    return watchdog_supervisor_window_is_healthy();
}

/**
 * @brief  查询是否可以安全进入 STOP 低功耗模式
 * @retval 1 — 可以进入；0 — 当前不健康或刚从 STOP 唤醒
 */
uint8_t watchdog_supervisor_can_enter_stop(void)
{
    return s_stop_entry_ready;
}

/**
 * @brief  获取缺失的进度位掩码
 * @return 未上报进度的必需任务位掩码
 */
uint32_t watchdog_supervisor_get_missing_progress_mask(void)
{
    return s_last_missing_progress_mask;
}

/**
 * @brief  获取最近一次的故障标志
 * @return 故障位掩码（WATCHDOG_FAULT_xxx）
 */
uint32_t watchdog_supervisor_get_last_fault_flags(void)
{
    return s_last_fault_flags;
}

/**
 * @brief  获取当前喂狗间隔
 * @return 喂狗间隔（ms）
 */
uint32_t watchdog_supervisor_get_feed_interval_ms(void)
{
    return s_feed_interval_ms;
}

/**
 * @brief  获取看门狗复位快照
 * @note   复位快照记录了导致看门狗复位时的诊断信息。
 * @param  out_snapshot — 输出：复位快照结构体指针
 */
void watchdog_supervisor_get_reset_snapshot(watchdog_supervisor_reset_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0)
    {
        return;
    }

    *out_snapshot = s_reset_snapshot;
}
