/**
 * @file rtc_lp_service.h
 * @brief 低功耗RTC唤醒与备份域管理中间件对外接口定义
 * @details 提供RTC唤醒定时器编程、中断事件追踪、备份寄存器安全读写等公共API。
 *          设计契约：本模块非线程安全，应由电源/低功耗管理任务单上下文调用。
 */
#ifndef RTC_LP_SERVICE_H
#define RTC_LP_SERVICE_H

#include <stdint.h>

/** RTC唤醒原因枚举 */
typedef enum
{
    RTC_LP_WAKE_NONE = 0,          /**< 无唤醒事件 */
    RTC_LP_WAKE_TIMER,             /**< RTC定时器唤醒（正常STOP/STANDBY周期唤醒） */
    RTC_LP_WAKE_STANDBY_RESET      /**< STANDBY冷启动复位唤醒 */
} rtc_lp_wakeup_reason_t;

/**
 * @brief RTC低功耗服务初始化（配置LSI时钟、分频器、EXTI22路由与NVIC）
 */
void rtc_lp_service_init(void);

/**
 * @brief 武装RTC唤醒定时器
 * @param period_ms 目标休眠周期(毫秒)，内部自动限制范围并选择最优时钟源
 */
void rtc_lp_arm_ms(uint32_t period_ms);

/**
 * @brief 解除RTC唤醒定时器（关闭中断、清除标志、等待硬件同步）
 */
void rtc_lp_disarm(void);

/**
 * @brief RTC唤醒中断服务函数（由RTC_WKUP_IRQHandler调用）
 */
void rtc_lp_handle_irq(void);

/**
 * @brief 消费并清除唤醒事件标志
 * @return 1: 有待处理唤醒事件 0: 无事件
 */
uint8_t rtc_lp_consume_wakeup_event(void);

/**
 * @brief 获取上次休眠流逝时间（用于RTOS/业务定时器补偿）
 * @return 流逝时间(毫秒)
 */
uint32_t rtc_lp_get_last_elapsed_ms(void);

/**
 * @brief 获取上次编程的目标休眠时间
 * @return 编程时间(毫秒)
 */
uint32_t rtc_lp_get_last_programmed_ms(void);

/**
 * @brief 获取系统唤醒原因
 * @return rtc_lp_wakeup_reason_t 唤醒原因枚举值
 */
rtc_lp_wakeup_reason_t rtc_lp_get_wakeup_reason(void);

/**
 * @brief 判断系统是否从STANDBY冷启动唤醒
 * @return 1: 是 0: 否
 */
uint8_t rtc_lp_woke_from_standby(void);

/**
 * @brief 写入RTC备份寄存器（跨STANDBY数据保存）
 * @param index 寄存器索引(0~4)
 * @param value 写入值
 */
void rtc_lp_backup_write(uint32_t index, uint32_t value);

/**
 * @brief 读取RTC备份寄存器
 * @param index 寄存器索引(0~4)
 * @return 读取值（索引越界返回0）
 */
uint32_t rtc_lp_backup_read(uint32_t index);

#endif /* RTC_LP_SERVICE_H */
