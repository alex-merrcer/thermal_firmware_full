/**
 * @file power_manager.h
 * @brief 系统电源管理与活动追踪中间件对外接口定义
 * @details 提供软件时基维护、电源状态机决策、业务锁管理、屏幕超时控制及休眠时间补偿等公共API。
 *          设计契约：本模块采用 PRIMASK 临界区保护，兼容 RTOS 启动前与中断上下文调用。
 */
#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdint.h>

/** 系统电源状态枚举 */
typedef enum
{
    POWER_STATE_ACTIVE_THERMAL = 0, /**< 热成像活跃状态（高性能，禁止休眠） */
    POWER_STATE_ACTIVE_UI,          /**< UI交互活跃状态（常规性能） */
    POWER_STATE_SCREEN_OFF_IDLE     /**< 息屏空闲状态（允许进入STOP低功耗） */
} power_state_t;

/** 电源策略配置枚举 */
typedef enum
{
    POWER_POLICY_PERFORMANCE = 0,   /**< 高性能策略：忽略节能，保持全速 */
    POWER_POLICY_BALANCED,          /**< 均衡策略：根据负载与超时动态调频/休眠 */
    POWER_POLICY_ECO,               /**< 节能策略：激进降频，缩短息屏超时 */
    POWER_POLICY_COUNT              /**< 策略数量（用于边界校验） */
} power_policy_t;

/* 历史兼容别名 */
#define POWER_STATE_ACTIVE_MENU   POWER_STATE_ACTIVE_UI
#define POWER_STATE_IDLE_SLEEP    POWER_STATE_SCREEN_OFF_IDLE

/** 电源业务锁掩码类型（位域设计，支持多业务并发持有） */
typedef uint32_t power_lock_mask_t;

#define POWER_LOCK_THERMAL      ((power_lock_mask_t)(1UL << 0)) /**< 热成像采集锁（阻止休眠） */
#define POWER_LOCK_OTA          ((power_lock_mask_t)(1UL << 1)) /**< OTA升级锁（阻止休眠） */
#define POWER_LOCK_DISPLAY_DMA  ((power_lock_mask_t)(1UL << 2)) /**< LCD DMA传输锁（阻止休眠） */
#define POWER_LOCK_ESP_HOST     ((power_lock_mask_t)(1UL << 3)) /**< Wi-Fi/蓝牙保活锁（允许息屏） */
#define POWER_LOCK_UI_MODAL     ((power_lock_mask_t)(1UL << 4)) /**< 模态对话框锁（阻止休眠） */
#define POWER_LOCK_USER         ((power_lock_mask_t)(1UL << 5)) /**< 用户自定义锁（阻止休眠） */

/**
 * @brief 电源管理器初始化（重置状态机、配置TIM5软件时基）
 */
void power_manager_init(void);

/**
 * @brief 通知系统发生用户/业务活动（重置息屏超时计时器）
 */
void power_manager_notify_activity(void);

/**
 * @brief 申请电源业务锁（阻止系统进入低功耗状态）
 * @param mask 锁掩码（支持按位或组合）
 */
void power_manager_acquire_lock(power_lock_mask_t mask);

/**
 * @brief 释放电源业务锁
 * @param mask 锁掩码
 */
void power_manager_release_lock(power_lock_mask_t mask);

/**
 * @brief 获取当前持有的电源锁掩码
 * @return power_lock_mask_t 当前锁状态
 */
power_lock_mask_t power_manager_get_lock_mask(void);

/**
 * @brief 设置电源策略
 * @param policy 目标策略枚举值
 */
void power_manager_set_policy(power_policy_t policy);

/**
 * @brief 获取当前电源策略
 * @return power_policy_t 当前策略枚举值
 */
power_policy_t power_manager_get_policy(void);

/**
 * @brief 设置低功耗模式开关（兼容旧版API，内部映射为策略切换）
 * @param enabled 1:启用节能 0:强制高性能
 */
void power_manager_set_low_power_enabled(uint8_t enabled);

/**
 * @brief 获取低功耗模式开关状态
 * @return 1:已启用 0:已禁用
 */
uint8_t power_manager_is_low_power_enabled(void);

/**
 * @brief 设置屏幕无操作超时时间（毫秒）
 * @param timeout_ms 超时阈值（低于10ms将重置为默认值）
 */
void power_manager_set_screen_off_timeout_ms(uint32_t timeout_ms);

/**
 * @brief 获取屏幕无操作超时时间
 * @return 超时时间(毫秒)
 */
uint32_t power_manager_get_screen_off_timeout_ms(void);

/**
 * @brief 电源状态机步进函数（需在主循环或调度任务中周期性调用）
 */
void power_manager_step(void);

/**
 * @brief 获取当前电源状态
 * @return power_state_t 当前状态枚举值
 */
power_state_t power_manager_get_state(void);

/**
 * @brief 获取系统软件滴答时间（毫秒）
 * @return 自初始化以来的流逝时间(ms)
 */
uint32_t power_manager_get_tick_ms(void);

/**
 * @brief 补偿STOP/STANDBY休眠期间流逝的时间
 * @param elapsed_ms 实际休眠时长(毫秒)，用于校准软件时基与业务定时器
 */
void power_manager_advance_sleep_time(uint32_t elapsed_ms);

/**
 * @brief 重新配置时基定时器（系统主频变更后调用）
 */
void power_manager_reconfigure_timebase(void);

#endif /* POWER_MANAGER_H */
