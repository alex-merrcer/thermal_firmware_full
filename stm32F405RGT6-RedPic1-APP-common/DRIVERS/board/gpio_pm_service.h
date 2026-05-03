/**
 * @file gpio_pm_service.h
 * @brief GPIO低功耗管理适配层对外接口定义
 * @details 提供系统进入STOP/STANDBY模式前的GPIO安全重配置，以及唤醒后的状态恢复接口。
 *          核心目标：阻断休眠漏电流路径、防止通信引脚虚假唤醒、保持低功耗生命周期对称。
 */
#ifndef GPIO_PM_SERVICE_H
#define GPIO_PM_SERVICE_H

/**
 * @brief 准备进入STOP停机模式（重配置外设GPIO以最小化漏电流）
 */
void gpio_pm_prepare_stop(void);

/**
 * @brief STOP模式唤醒后恢复GPIO上下文
 */
void gpio_pm_restore_after_stop(void);

/**
 * @brief 准备进入STANDBY待机模式（重配置外设GPIO以最小化漏电流）
 * @note 当前序列与STOP一致，独立接口用于状态机对称管理及未来备份域引脚扩展
 */
void gpio_pm_prepare_standby(void);

void low_power_runtime_prepare_mcu_for_stop(void);

#endif /* GPIO_PM_SERVICE_H */
