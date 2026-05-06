/**
 * @file low_power_runtime.h
 * @brief 低功耗运行时管理模块对外接口定义
 * @details 提供系统低功耗状态机调度、手动/自动待机触发、早期启动恢复等公共API。
 *          内部基于RTC备份域实现跨待机上下文保存，支持低电指数退避唤醒策略。
 */
#ifndef LOW_POWER_RUNTIME_H
#define LOW_POWER_RUNTIME_H

#include <stdint.h>

/** 低功耗运行时状态枚举 */
typedef enum {
    LP_RUNTIME_STATE_RUN = 0,             /**< 正常运行状态 */
    LP_RUNTIME_STATE_STOP_IDLE,           /**< STOP停机休眠状态（保留RAM，RTC唤醒） */
    LP_RUNTIME_STATE_STANDBY_PROTECT      /**< STANDBY待机保护状态（仅备份域供电，冷启动恢复） */
} lp_runtime_state_t;

/**
 * @brief 低功耗运行时模块初始化
 */
void low_power_runtime_init(void);

/**
 * @brief 低功耗调度心跳函数（需在主循环或低功耗任务中周期性调用）
 */
void low_power_runtime_step(void);

/**
 * @brief 获取当前低功耗运行时状态
 * @return lp_runtime_state_t 当前状态枚举值
 */
lp_runtime_state_t low_power_runtime_get_state(void);

/**
 * @brief 处理早期启动阶段的待机恢复逻辑
 * @details 若系统从STANDBY唤醒且电池仍处于低电状态，将直接重新进入待机保护，
 *          防止低电反复冷启动导致系统死锁或电池过放。
 * @return 1: 已重新进入待机（启动流程终止） 0: 正常继续启动
 */
uint8_t low_power_runtime_handle_early_boot(void);

/**
 * @brief 请求手动进入待机模式（异步触发，由step函数实际执行）
 */

#endif /* LOW_POWER_RUNTIME_H */
