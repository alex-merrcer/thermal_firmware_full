/**
 * @file clock_profile_service.h
 * @brief 系统时钟配置与动态调频服务对外接口定义
 * @details 提供运行时性能档位切换、STOP模式唤醒后时钟树恢复、下游时基同步等公共API。
 *          设计契约：本模块非线程安全，应由电源/时钟管理任务单上下文调用。
 */
#ifndef CLOCK_PROFILE_SERVICE_H
#define CLOCK_PROFILE_SERVICE_H

#include <stdint.h>

/** 系统时钟性能档位枚举 */
typedef enum {
    CLOCK_PROFILE_HIGH = 0,    /**< 高性能模式 (HCLK = SYSCLK / 1, 典型168MHz) */
    CLOCK_PROFILE_MEDIUM       /**< 中等性能模式 (HCLK = SYSCLK / 2, 典型84MHz) */
} clock_profile_t;

/** 时钟策略配置枚举 */
typedef enum {
    CLOCK_PROFILE_POLICY_AUTO = 0, /* auto switch by workload */
    CLOCK_PROFILE_POLICY_HIGH_ONLY, /* fixed high clock: 168MHz */
    CLOCK_PROFILE_POLICY_MEDIUM_ONLY /* fixed medium clock: 84MHz */
} clock_profile_policy_t;

/**
 * @brief 时钟调频服务初始化
 */
void clock_profile_service_init(void);

/**
 * @brief 设置系统时钟性能档位
 * @param profile 目标档位枚举值
 */
void clock_profile_set(clock_profile_t profile);

/**
 * @brief 获取当前激活的时钟档位
 * @return clock_profile_t 当前档位枚举值
 */
clock_profile_t clock_profile_get(void);

/**
 * @brief STOP模式唤醒后恢复时钟树（含UART波特率重初始化）
 */
void clock_profile_restore_after_stop(void);

/**
 * @brief STOP模式唤醒后恢复时钟树（保持UART休眠/当前配置）
 * @note 适用于RTC定时唤醒且无需立即通信的场景，降低唤醒瞬时功耗
 */
void clock_profile_restore_after_stop_keep_uart_sleep(void);

#endif /* CLOCK_PROFILE_SERVICE_H */
