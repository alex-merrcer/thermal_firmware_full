/**
 * @file low_power_runtime.c
 * @brief 低功耗运行时状态机与电源策略核心实现
 * @details 架构说明：
 *          1. 采用协商式休眠机制：进入低功耗前通过 app_service 通知各模块安全挂起
 *          2. 支持 STOP(停机) 与 STANDBY(待机) 两级低功耗策略，由电量/屏灭时长/用户配置动态决策
 *          3. 利用 RTC Backup Register 保存待机上下文，实现低电指数退避唤醒与迟滞恢复
 *          4. 严格遵循 ARM Cortex-M 低功耗进入规范（中断静默 -> PWR清标志 -> DSB/ISB -> WFI/Standby）
 */
#include "low_power_runtime.h"
#include <string.h>
#include "app_display_runtime.h"
#include "battery_monitor.h"
#include "clock_profile_service.h"
#include "esp_host_service.h"
#include "gpio_pm_service.h"
#include "power_manager.h"
#include "redpic1_app.h"
#include "redpic1_thermal.h"
#include "rtc_lp_service.h"
#include "settings_service.h"
#include "stm32f4xx_conf.h"
#include "watchdog_service.h"

/* ======================== 配置与常量定义 ======================== */
/* 服务协商与超时参数 */
#define LOW_POWER_RUNTIME_STOP_PREP_TIMEOUT_MS    400UL   /**< 模块休眠准备协商超时时间 */
#define LOW_POWER_RUNTIME_SERVICE_MARGIN_MS       150UL   /**< 服务提交额外安全裕量 */

/* 手动待机与自动待机策略参数 */
#define LOW_POWER_RUNTIME_MANUAL_STANDBY_WAKE_MS  10000UL /**< 手动待机默认唤醒周期(10s) */
#define LOW_POWER_RUNTIME_STANDBY_IDLE_MS         (30UL * 60UL * 1000UL) /**< 屏灭自动待机阈值(30min) */

/* 电池电压迟滞阈值（防低电反复重启） */
#define LOW_POWER_RUNTIME_STANDBY_LOW_MV          3000U   /**< 进入待机保护的低电阈值 */
#define LOW_POWER_RUNTIME_STANDBY_RECOVER_MV      3300U   /**< 退出待机保护的恢复阈值(300mV迟滞) */

/* 指数退避唤醒策略 */
#define LOW_POWER_RUNTIME_STANDBY_BACKOFF_MAX     5U      /**< 最大退避阶数 */
static const uint32_t STANDBY_BACKOFF_PERIODS_MS[] = {
    60000UL,   /* 1分钟  */
    120000UL,  /* 2分钟  */
    300000UL,  /* 5分钟  */
    600000UL,  /* 10分钟 */
    900000UL,  /* 15分钟 */
    1800000UL  /* 30分钟 */
};

/* RTC备份寄存器上下文映射（Backup Domain Register 0~3） */
#define LP_RTC_CTX_REG_MAGIC      0U
#define LP_RTC_CTX_REG_RETRY      1U
#define LP_RTC_CTX_REG_BATT_MV    2U
#define LP_RTC_CTX_REG_PERIOD_MS  3U
#define LOW_POWER_RUNTIME_CTX_MAGIC 0x4C505231UL          /**< 上下文有效性魔数 "LPR1" */

/* ======================== 类型定义 ======================== */
/** 待机恢复上下文结构体（映射至RTC备份域） */
typedef struct {
    uint32_t magic;           /**< 魔数校验 */
    uint32_t retry_count;     /**< 连续低电待机重试次数 */
    uint32_t last_battery_mv; /**< 进入待机前的电池电压 */
    uint32_t next_period_ms;  /**< 下次唤醒周期 */
} low_power_standby_ctx_t;

/* ======================== 静态全局状态 ======================== */
static lp_runtime_state_t s_runtime_state = LP_RUNTIME_STATE_RUN;
static power_state_t      s_last_power_state = POWER_STATE_ACTIVE_UI;
static uint32_t           s_screen_off_enter_ms = 0U;
static uint8_t            s_stop_host_prepared = 0U;
static volatile uint8_t   s_manual_standby_pending = 0U; /**< volatile防编译器优化跨上下文访问 */

/* ======================== 内部辅助函数 ======================== */

/**
 * @brief 向业务服务层发送休眠准备指令并等待响应
 * @param cmd_id     服务命令ID
 * @param timeout_ms 协商超时时间
 * @return 1: 准备成功 0: 超时或拒绝
 */
static uint8_t low_power_runtime_service_prepare(app_service_cmd_id_t cmd_id, uint32_t timeout_ms)
{
    app_service_cmd_t cmd;
    app_service_rsp_t rsp;
    memset(&cmd, 0, sizeof(cmd));
    memset(&rsp, 0, sizeof(rsp));

    cmd.cmd_id = cmd_id;
    cmd.value  = timeout_ms;

    /* 提交协商请求，超时时间增加安全裕量防止边界竞态 */
    return app_service_submit(&cmd, &rsp, timeout_ms + LOW_POWER_RUNTIME_SERVICE_MARGIN_MS);
}

/**
 * @brief 静默外设中断（防止进入STANDBY时被虚假中断唤醒或导致总线挂起）
 */
static void low_power_runtime_quiesce_irqs_for_standby(void)
{
    /* 定时器中断屏蔽与标志清除 */
    TIM_Cmd(TIM3, DISABLE);
    TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);
    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);

    TIM_Cmd(TIM5, DISABLE);
    TIM_ITConfig(TIM5, TIM_IT_Update, DISABLE);
    TIM_ClearITPendingBit(TIM5, TIM_IT_Update);

    /* 串口中断屏蔽 */
    USART_ITConfig(USART1, USART_IT_RXNE, DISABLE);
    USART_ITConfig(USART1, USART_IT_ERR, DISABLE);

    /* 外部中断挂起位清除（按键/传感器等） */
    EXTI_ClearITPendingBit(EXTI_Line8);
    EXTI_ClearITPendingBit(EXTI_Line9);
    EXTI_ClearITPendingBit(EXTI_Line13);
    EXTI_ClearITPendingBit(EXTI_Line22);

    /* NVIC pending IRQ 清除，防止唤醒后立即进入错误ISR */
    NVIC_ClearPendingIRQ(TIM3_IRQn);
    NVIC_ClearPendingIRQ(TIM5_IRQn);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    NVIC_ClearPendingIRQ(EXTI15_10_IRQn);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
}

/**
 * @brief 根据重试次数计算指数退避唤醒周期
 */
static uint32_t low_power_runtime_next_standby_period_ms(uint32_t retry_count)
{
    if (retry_count >= LOW_POWER_RUNTIME_STANDBY_BACKOFF_MAX) {
        retry_count = LOW_POWER_RUNTIME_STANDBY_BACKOFF_MAX;
    }
    return STANDBY_BACKOFF_PERIODS_MS[retry_count];
}

/**
 * @brief 将上下文写入RTC备份域（STANDBY模式下唯一保留的SRAM）
 */
static void low_power_runtime_store_ctx(uint32_t retry_count, uint32_t battery_mv, uint32_t next_period_ms)
{
    rtc_lp_backup_write(LP_RTC_CTX_REG_MAGIC,     LOW_POWER_RUNTIME_CTX_MAGIC);
    rtc_lp_backup_write(LP_RTC_CTX_REG_RETRY,     retry_count);
    rtc_lp_backup_write(LP_RTC_CTX_REG_BATT_MV,   battery_mv);
    rtc_lp_backup_write(LP_RTC_CTX_REG_PERIOD_MS, next_period_ms);
}

/**
 * @brief 清除RTC备份域上下文
 */
static void low_power_runtime_clear_ctx(void)
{
    rtc_lp_backup_write(LP_RTC_CTX_REG_MAGIC,     0U);
    rtc_lp_backup_write(LP_RTC_CTX_REG_RETRY,     0U);
    rtc_lp_backup_write(LP_RTC_CTX_REG_BATT_MV,   0U);
    rtc_lp_backup_write(LP_RTC_CTX_REG_PERIOD_MS, 0U);
}

/**
 * @brief 从RTC备份域加载上下文并校验魔数
 */
static uint8_t low_power_runtime_load_ctx(low_power_standby_ctx_t *ctx)
{
    if (ctx == NULL) return 0U;

    ctx->magic          = rtc_lp_backup_read(LP_RTC_CTX_REG_MAGIC);
    ctx->retry_count    = rtc_lp_backup_read(LP_RTC_CTX_REG_RETRY);
    ctx->last_battery_mv = rtc_lp_backup_read(LP_RTC_CTX_REG_BATT_MV);
    ctx->next_period_ms = rtc_lp_backup_read(LP_RTC_CTX_REG_PERIOD_MS);

    return (ctx->magic == LOW_POWER_RUNTIME_CTX_MAGIC) ? 1U : 0U;
}

/**
 * @brief 动态更新系统时钟策略（根据电源锁与运行状态降频/升频）
 */
static void low_power_runtime_update_clock_profile(void)
{
    const device_settings_t *settings = settings_service_get();
    power_lock_mask_t lock_mask = power_manager_get_lock_mask();

    /* 强制高性能策略 */
    if (settings->clock_profile_policy == CLOCK_PROFILE_POLICY_HIGH_ONLY) {
        clock_profile_set(CLOCK_PROFILE_HIGH);
        return;
    }

    if (settings->clock_profile_policy == CLOCK_PROFILE_POLICY_MEDIUM_ONLY) {
        clock_profile_set(CLOCK_PROFILE_MEDIUM);
        return;
    }

    /* 存在高功耗业务锁或处于热成像活跃状态时，维持高频 */
    if ((lock_mask & (POWER_LOCK_THERMAL | POWER_LOCK_OTA | POWER_LOCK_ESP_HOST)) != 0U ||
        power_manager_get_state() == POWER_STATE_ACTIVE_THERMAL) {
        clock_profile_set(CLOCK_PROFILE_HIGH);
    } else {
        clock_profile_set(CLOCK_PROFILE_MEDIUM);
    }
}

/**
 * @brief 处理从STOP/STANDBY唤醒后的硬件与状态恢复
 */
static void low_power_runtime_handle_post_wake(void)
{
    uint8_t woke_by_timer = rtc_lp_consume_wakeup_event();

    /* 根据唤醒源选择时钟恢复策略（保留UART休眠状态或全量恢复） */
    if (woke_by_timer && power_manager_get_state() == POWER_STATE_SCREEN_OFF_IDLE) {
        clock_profile_restore_after_stop_keep_uart_sleep();
    } else {
        clock_profile_restore_after_stop();
    }

    gpio_pm_restore_after_stop();
    redpic1_thermal_restore_bus_after_stop();
    rtc_lp_disarm();

    /* 补偿系统休眠期间流逝的时间（维持RTOS/业务定时器准确性） */
    if (woke_by_timer) {
        power_manager_advance_sleep_time(rtc_lp_get_last_elapsed_ms());
    }

    watchdog_service_note_stop_wake();
    s_runtime_state = LP_RUNTIME_STATE_RUN;
}

/**
 * @brief 执行进入STOP停机模式的完整序列
 */
static void low_power_runtime_enter_stop(void)
{
    const device_settings_t *settings = settings_service_get();
    uint32_t wake_period_ms = settings->rtc_stop_wake_ms;
    uint8_t host_ready = 0U;
    esp_host_status_t host_status;

    if (wake_period_ms == 0U) wake_period_ms = 1000U;

    watchdog_service_force_feed();

    /* 首次进入STOP前需与Host/业务层协商 */
    if (s_stop_host_prepared == 0U) {
        host_ready = low_power_runtime_service_prepare(APP_SERVICE_CMD_PREPARE_STOP,
                                                       LOW_POWER_RUNTIME_STOP_PREP_TIMEOUT_MS);
        esp_host_get_status_copy(&host_status);

        /* 协商失败且Host在线，则放弃本次休眠，防止通信中断 */
        if (host_ready == 0U && host_status.online != 0U) {
            s_runtime_state = LP_RUNTIME_STATE_RUN;
            return;
        }
        s_stop_host_prepared = 1U;
    }

    gpio_pm_prepare_stop();
    rtc_lp_arm_ms(wake_period_ms);
    s_runtime_state = LP_RUNTIME_STATE_STOP_IDLE;

    PWR_ClearFlag(PWR_FLAG_WU);
    PWR_EnterSTOPMode(PWR_Regulator_LowPower, PWR_STOPEntry_WFI);

    /* WFI返回后执行唤醒恢复 */
    low_power_runtime_handle_post_wake();
}

/**
 * @brief STANDBY进入公共序列（消除原代码重复逻辑）
 * @param wake_ms     RTC唤醒周期
 * @param is_manual   是否为手动触发（手动触发不保存退避上下文）
 */
static void execute_standby_sequence(uint32_t wake_ms, uint8_t is_manual)
{
    watchdog_service_force_feed();

    /* 协商各模块进入待机准备状态 */
    (void)low_power_runtime_service_prepare(APP_SERVICE_CMD_PREPARE_STANDBY,
                                            LOW_POWER_RUNTIME_STOP_PREP_TIMEOUT_MS);
    (void)app_display_runtime_sleep();
    gpio_pm_prepare_standby();

    /* 上下文管理：手动待机清除记录，自动待机保存退避参数 */
    if (is_manual) {
        low_power_runtime_clear_ctx();
    }

    low_power_runtime_quiesce_irqs_for_standby();
    rtc_lp_arm_ms(wake_ms);
    s_runtime_state = LP_RUNTIME_STATE_STANDBY_PROTECT;

    /* 清除唤醒/待机标志，防止立即唤醒 */
    PWR_ClearFlag(PWR_FLAG_WU);
    PWR_ClearFlag(PWR_FLAG_SB);

    /* ARM Cortex-M 规范：进入低功耗前必须执行内存屏障，确保所有总线传输完成 */
    __disable_irq();
    __DSB();
    __ISB();
    PWR_EnterSTANDBYMode();
    /* 注：执行到此行说明已从STANDBY冷启动复位，后续流程由SystemInit接管 */
}

/**
 * @brief 手动触发待机入口
 */
static void low_power_runtime_enter_manual_standby(void)
{
    execute_standby_sequence(LOW_POWER_RUNTIME_MANUAL_STANDBY_WAKE_MS, 1U);
}

/**
 * @brief 自动低电待机入口（带指数退避上下文保存）
 */
static void low_power_runtime_enter_standby(uint32_t retry_count)
{
    uint32_t battery_mv = battery_monitor_get_mv();
    uint32_t next_period_ms = low_power_runtime_next_standby_period_ms(retry_count);

    /* 保存退避上下文至备份域，供冷启动后读取 */
    low_power_runtime_store_ctx(retry_count, battery_mv, next_period_ms);
    execute_standby_sequence(next_period_ms, 0U);
}

/* ======================== 公开API实现 ======================== */

/**
 * @brief 处理早期启动阶段的低电保护逻辑
 * @details 若从STANDBY冷启动唤醒，且电池电压仍低于恢复阈值(3300mV)，
 *          则直接重新进入待机并增加退避阶数，防止低电死循环重启。
 */
uint8_t low_power_runtime_handle_early_boot(void)
{
    const device_settings_t *settings = settings_service_get();
    low_power_standby_ctx_t ctx;

    if (rtc_lp_woke_from_standby() == 0U) return 0U;
    if (low_power_runtime_load_ctx(&ctx) == 0U) return 0U;

    /* 用户关闭待机策略或处于非ECO模式，清除上下文并正常启动 */
    if (settings->standby_enabled == 0U || settings->power_policy != POWER_POLICY_ECO) {
        low_power_runtime_clear_ctx();
        return 0U;
    }

    /* 电压已恢复至安全阈值以上，清除保护状态 */
    if (battery_monitor_get_mv() >= LOW_POWER_RUNTIME_STANDBY_RECOVER_MV) {
        low_power_runtime_clear_ctx();
        return 0U;
    }

    /* 仍处于低电状态，递增重试次数并重新进入待机保护 */
    low_power_runtime_enter_standby(ctx.retry_count + 1U);
    return 1U; /* 不会执行到此，系统已复位 */
}

void low_power_runtime_init(void)
{
    s_runtime_state = LP_RUNTIME_STATE_RUN;
    s_last_power_state = power_manager_get_state();
    s_screen_off_enter_ms = 0U;
    s_stop_host_prepared = 0U;
    s_manual_standby_pending = 0U;
}

/**
 * @brief 低功耗调度主状态机
 * @details 决策优先级：手动待机 > 屏灭超时+低电自动待机 > STOP停机休眠 > WFI空闲
 */
void low_power_runtime_step(void)
{
    const device_settings_t *settings = settings_service_get();
    uint32_t now_ms = power_manager_get_tick_ms();
    uint32_t screen_off_elapsed_ms = 0U;

    /* 1. 电源状态变化同步 */
    if (power_manager_get_state() != s_last_power_state) {
        s_last_power_state = power_manager_get_state();
        if (s_last_power_state == POWER_STATE_SCREEN_OFF_IDLE) {
            s_screen_off_enter_ms = now_ms;
        }
        s_stop_host_prepared = 0U; /* 状态切换后需重新协商 */
    }

    /* 2. 动态时钟策略调整 */
    low_power_runtime_update_clock_profile();

    /* 3. 手动待机请求处理（优先级最高） */
    if (s_manual_standby_pending != 0U) {
        s_manual_standby_pending = 0U;
        low_power_runtime_enter_manual_standby();
        return;
    }

    /* 4. 非屏灭状态：维持运行，执行WFI空闲等待 */
    if (power_manager_get_state() != POWER_STATE_SCREEN_OFF_IDLE) {
        s_runtime_state = LP_RUNTIME_STATE_RUN;
        __WFI();
        return;
    }

    /* 5. 看门狗安全拦截：若业务层禁止休眠，则放弃本次STOP */
    if (watchdog_service_can_enter_stop() == 0U) {
        s_runtime_state = LP_RUNTIME_STATE_RUN;
        __WFI();
        return;
    }

    /* 6. 自动待机决策（ECO模式 + 待机使能 + 屏灭超时 + 低电阈值） */
    screen_off_elapsed_ms = now_ms - s_screen_off_enter_ms;
    if (settings->power_policy == POWER_POLICY_ECO &&
        settings->standby_enabled != 0U &&
        screen_off_elapsed_ms >= LOW_POWER_RUNTIME_STANDBY_IDLE_MS &&
        battery_monitor_get_mv() < LOW_POWER_RUNTIME_STANDBY_LOW_MV) {
        low_power_runtime_enter_standby(0U);
        return;
    }

    /* 7. 默认策略：进入STOP停机模式（RTC定时唤醒） */
    low_power_runtime_enter_stop();
}

lp_runtime_state_t low_power_runtime_get_state(void)
{
    return s_runtime_state;
}

void low_power_runtime_request_manual_standby(void)
{
    s_manual_standby_pending = 1U;
}
