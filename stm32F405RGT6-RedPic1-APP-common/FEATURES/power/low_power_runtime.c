/**
 * @file low_power_runtime.c
 * @brief 低功耗运行时状态机与电源策略编排模块。
 *
 * 本模块是系统低功耗管理的核心调度器，负责：
 *   1. 根据电源状态（屏灭/低电等）决策进入哪种低功耗模式
 *   2. 协调各业务模块（存储、显示、传感器等）进入/退出低功耗状态
 *   3. 管理STANDBY模式下的指数退避唤醒策略（低电保护）
 *   4. 动态调整系统时钟频率（高性能/中等性能切换）
 *
 * 低功耗模式优先级（从高到低）：
 *   手动待机请求 > 屏灭超时+低电自动待机 > STOP停机休眠 > WFI空闲等待
 *
 * STOP模式：CPU停止，SRAM保留，RTC唤醒，唤醒后从停止点继续执行
 * STANDBY模式：CPU停止，SRAM丢失，唤醒后相当于复位（冷启动）
 */
#include "low_power_runtime.h"
#include <string.h>
#include "app_pm_registry.h"
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
#include "storage_service.h"
#include "stm32f4xx_conf.h"
#include "watchdog_service.h"

/* ===================================================================== */
/*                            常量定义                                     */
/* ===================================================================== */

/** STOP模式准备协商超时时间（毫秒），超时则放弃本次休眠 */
#define LOW_POWER_RUNTIME_STOP_PREP_TIMEOUT_MS    400UL

/** 服务总线协商安全裕量（毫秒），防止边界竞态导致超时 */
#define LOW_POWER_RUNTIME_SERVICE_MARGIN_MS       150UL

/** 手动待机的RTC唤醒周期（毫秒），10秒后自动唤醒 */
#define LOW_POWER_RUNTIME_MANUAL_STANDBY_WAKE_MS  10000UL

/** 自动待机的屏灭空闲时间阈值（毫秒），30分钟无操作才考虑待机 */
#define LOW_POWER_RUNTIME_STANDBY_IDLE_MS         (30UL * 60UL * 1000UL)

/** 自动待机的电池电压阈值（毫伏），低于此值触发低电保护待机 */
#define LOW_POWER_RUNTIME_STANDBY_LOW_MV          3000U

/** 低电保护恢复阈值（毫伏），高于此值才允许正常启动 */
#define LOW_POWER_RUNTIME_STANDBY_RECOVER_MV      3300U

/** 指数退避最大阶数，超过此值使用最大退避周期 */
#define LOW_POWER_RUNTIME_STANDBY_BACKOFF_MAX     5U

/**
 * 指数退避唤醒周期表（毫秒）
 * 低电保护待机时，每次唤醒发现仍低电则增加退避阶数：
 *   第0次: 60秒 → 第1次: 120秒 → 第2次: 5分钟 → ...
 *   第5次: 30分钟（最大值）
 */
static const uint32_t STANDBY_BACKOFF_PERIODS_MS[] = {
    60000UL,    /* 1分钟 */
    120000UL,   /* 2分钟 */
    300000UL,   /* 5分钟 */
    600000UL,   /* 10分钟 */
    900000UL,   /* 15分钟 */
    1800000UL   /* 30分钟 */
};

/* ===================================================================== */
/*                    RTC备份域寄存器映射（STANDBY后唯一保留的存储）           */
/* ===================================================================== */

/** RTC备份寄存器索引：魔数，用于校验上下文是否有效 */
#define LP_RTC_CTX_REG_MAGIC         0U

/** RTC备份寄存器索引：退避重试次数 */
#define LP_RTC_CTX_REG_RETRY         1U

/** RTC备份寄存器索引：上次电池电压（毫伏） */
#define LP_RTC_CTX_REG_BATT_MV       2U

/** RTC备份寄存器索引：下次唤醒周期（毫秒） */
#define LP_RTC_CTX_REG_PERIOD_MS     3U

/** 上下文魔数，用于判断RTC备份域数据是否为有效上下文 */
#define LOW_POWER_RUNTIME_CTX_MAGIC  0x4C505231UL

/**
 * STANDBY退避上下文结构体
 * 存储在RTC备份寄存器中，STANDBY唤醒后（冷启动）读取
 */
typedef struct {
    uint32_t magic;          /**< 魔数，校验用 */
    uint32_t retry_count;    /**< 已重试次数 */
    uint32_t last_battery_mv;/**< 上次电池电压(mV) */
    uint32_t next_period_ms; /**< 下次唤醒周期(ms) */
} low_power_standby_ctx_t;

/* ===================================================================== */
/*                            运行时状态变量                                */
/* ===================================================================== */

/** 当前低功耗运行时状态（运行/STOP空闲/STANDBY保护） */
static lp_runtime_state_t s_runtime_state = LP_RUNTIME_STATE_RUN;

/** 上次同步的电源管理器状态，用于检测状态变化 */
static power_state_t      s_last_power_state = POWER_STATE_ACTIVE_UI;

/** 进入屏灭状态的时间戳（毫秒），用于计算屏灭持续时间 */
static uint32_t           s_screen_off_enter_ms = 0U;

/** STOP模式Host协商标志，0=需要重新协商，1=已协商过 */
static uint8_t            s_stop_host_prepared = 0U;

/** 手动待机请求标志，由low_power_runtime_request_manual_standby()设置 */
static volatile uint8_t   s_manual_standby_pending = 0U;

/** PM客户端注册标志，0=未注册，1=已注册 */
static uint8_t            s_pm_clients_registered = 0U;

/* ===================================================================== */
/*                        PM客户端回调函数                                  */
/* ===================================================================== */

/**
 * @brief 看门狗模块：查询是否允许进入STOP模式
 * @return 1=允许，0=禁止（有业务未完成健康检查）
 */
static uint8_t low_power_runtime_pm_watchdog_can_sleep(void)
{
    return watchdog_service_can_enter_stop();
}

/**
 * @brief 显示模块：STANDBY前的准备（让LCD进入睡眠）
 */
static void low_power_runtime_pm_display_prepare_standby(void)
{
    (void)app_display_runtime_sleep();
}

/**
 * @brief 存储模块：STOP前的准备（同步文件系统、卸载SD卡）
 */
static void low_power_runtime_pm_storage_prepare_stop(void)
{
    storage_service_prepare_for_stop();
}

/**
 * @brief 存储模块：STANDBY前的准备（同STOP准备）
 */
static void low_power_runtime_pm_storage_prepare_standby(void)
{
    storage_service_prepare_for_standby();
}

/**
 * @brief GPIO电源管理模块：STOP前的准备（关闭外设时钟、配置低功耗GPIO）
 */
static void low_power_runtime_pm_gpio_prepare_stop(void)
{
    gpio_pm_prepare_stop();
}

/**
 * @brief GPIO电源管理模块：STOP唤醒后的恢复（恢复外设时钟和GPIO配置）
 */
static void low_power_runtime_pm_gpio_restore_stop(void)
{
    gpio_pm_restore_after_stop();
}

/**
 * @brief GPIO电源管理模块：STANDBY前的准备
 */
static void low_power_runtime_pm_gpio_prepare_standby(void)
{
    gpio_pm_prepare_standby();
}

/**
 * @brief 热成像模块：STOP唤醒后的I2C总线恢复
 */
static void low_power_runtime_pm_thermal_restore_stop(void)
{
    redpic1_thermal_restore_bus_after_stop();
}

/**
 * @brief 看门狗模块：STOP唤醒后的通知（跳过首周期健康检查）
 */
static void low_power_runtime_pm_watchdog_restore_stop(void)
{
    watchdog_service_note_stop_wake();
}

/* ===================================================================== */
/*                      PM客户端注册                                       */
/* ===================================================================== */

/**
 * @brief 注册所有业务模块的PM（电源管理）客户端
 *
 * PM客户端机制允许各模块注册低功耗回调：
 *   - can_sleep: 查询是否允许进入低功耗（返回0则阻止）
 *   - prepare_stop: 进入STOP前的准备回调
 *   - restore_stop: 从STOP唤醒后的恢复回调
 *   - prepare_standby: 进入STANDBY前的准备回调
 *
 * 注册的模块：display、storage、gpio_pm、thermal、watchdog、esp_host、
 *            lcd_dma、rtc、battery、settings
 *
 * 只注册一次（幂等）。
 */
static void low_power_runtime_register_pm_clients(void)
{
    /* 各模块的PM客户端配置（静态常量，只初始化一次） */
    static const app_pm_client_t s_display_pm_client = {
        "display",
        0,                                          /* 无can_sleep检查 */
        0,                                          /* 无prepare_stop */
        0,                                          /* 无restore_stop */
        low_power_runtime_pm_display_prepare_standby /* STANDBY前让LCD睡眠 */
    };
    static const app_pm_client_t s_storage_pm_client = {
        "storage",
        0,                                          /* 无can_sleep检查 */
        low_power_runtime_pm_storage_prepare_stop,   /* STOP前同步文件系统 */
        0,                                          /* 无restore_stop（唤醒后重新挂载） */
        low_power_runtime_pm_storage_prepare_standby /* STANDBY前同步文件系统 */
    };
    static const app_pm_client_t s_gpio_pm_client = {
        "gpio_pm",
        0,                                          /* 无can_sleep检查 */
        low_power_runtime_pm_gpio_prepare_stop,      /* STOP前关闭外设时钟 */
        low_power_runtime_pm_gpio_restore_stop,      /* STOP后恢复外设时钟 */
        low_power_runtime_pm_gpio_prepare_standby    /* STANDBY前关闭外设时钟 */
    };
    static const app_pm_client_t s_thermal_pm_client = {
        "thermal",
        0,                                          /* 无can_sleep检查 */
        0,                                          /* 无prepare_stop */
        low_power_runtime_pm_thermal_restore_stop,   /* STOP后恢复I2C总线 */
        0                                           /* 无prepare_standby */
    };
    static const app_pm_client_t s_watchdog_pm_client = {
        "watchdog",
        low_power_runtime_pm_watchdog_can_sleep,     /* 查询是否允许进入STOP */
        0,                                          /* 无prepare_stop */
        low_power_runtime_pm_watchdog_restore_stop,  /* STOP后通知跳过首周期检查 */
        0                                           /* 无prepare_standby */
    };
    static const app_pm_client_t s_esp_host_pm_client = {
        "esp_host",
        0, 0, 0, 0                                  /* ESP主机模块，暂无回调 */
    };
    static const app_pm_client_t s_lcd_dma_pm_client = {
        "lcd_dma",
        0, 0, 0, 0                                  /* LCD DMA模块，暂无回调 */
    };
    static const app_pm_client_t s_rtc_pm_client = {
        "rtc",
        0, 0, 0, 0                                  /* RTC模块，暂无回调 */
    };
    static const app_pm_client_t s_battery_pm_client = {
        "battery",
        0, 0, 0, 0                                  /* 电池监测模块，暂无回调 */
    };
    static const app_pm_client_t s_settings_pm_client = {
        "settings",
        0, 0, 0, 0                                  /* 设置模块，暂无回调 */
    };
    uint8_t ok = 1U;

    /* 防止重复注册 */
    if (s_pm_clients_registered != 0U)
    {
        return;
    }

    /* 注册所有PM客户端 */
    ok &= app_pm_register_client(&s_display_pm_client);
    ok &= app_pm_register_client(&s_storage_pm_client);
    ok &= app_pm_register_client(&s_gpio_pm_client);
    ok &= app_pm_register_client(&s_thermal_pm_client);
    ok &= app_pm_register_client(&s_watchdog_pm_client);
    ok &= app_pm_register_client(&s_esp_host_pm_client);
    ok &= app_pm_register_client(&s_lcd_dma_pm_client);
    ok &= app_pm_register_client(&s_rtc_pm_client);
    ok &= app_pm_register_client(&s_battery_pm_client);
    ok &= app_pm_register_client(&s_settings_pm_client);

    s_pm_clients_registered = ok;
}

/* ===================================================================== */
/*                        服务总线协商                                      */
/* ===================================================================== */

/**
 * @brief 通过应用服务总线发送低功耗准备命令并等待应答
 *
 * 各业务模块通过服务总线接收低功耗准备命令，在超时时间内完成准备并回复。
 * 如果超时或有模块拒绝，则本次休眠取消。
 *
 * @param cmd_id    命令ID（APP_SERVICE_CMD_PREPARE_STOP 或 PREPARE_STANDBY）
 * @param timeout_ms 协商超时时间（毫秒）
 * @return 1=所有模块准备就绪，0=有模块超时或拒绝
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

/* ===================================================================== */
/*                     STANDBY前中断静默                                    */
/* ===================================================================== */

/**
 * @brief 静默外设中断，防止进入STANDBY时被虚假中断唤醒或导致总线挂起
 *
 * 操作内容：
 *   1. 禁用TIM3/TIM5更新中断并清除挂起位
 *   2. 禁用USART1接收和错误中断
 *   3. 清除外部中断挂起位（EXTI Line 8/9/13/22，对应按键/传感器等）
 *   4. 清除NVIC中所有相关中断的挂起位
 *
 * 为什么需要：STANDBY模式下如果NVIC中有挂起的中断，
 * 可能导致唤醒后立即进入错误的ISR或产生不可预期的行为。
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

/* ===================================================================== */
/*                    指数退避与RTC备份域管理                                */
/* ===================================================================== */

/**
 * @brief 根据重试次数计算指数退避唤醒周期
 *
 * 退避策略：每次低电唤醒后，下次等待时间翻倍增长
 *   retry=0 → 60秒, retry=1 → 120秒, ..., retry=5+ → 30分钟
 *
 * @param retry_count 当前重试次数
 * @return 下次RTC唤醒周期（毫秒）
 */
static uint32_t low_power_runtime_next_standby_period_ms(uint32_t retry_count)
{
    if (retry_count >= LOW_POWER_RUNTIME_STANDBY_BACKOFF_MAX) {
        retry_count = LOW_POWER_RUNTIME_STANDBY_BACKOFF_MAX;
    }
    return STANDBY_BACKOFF_PERIODS_MS[retry_count];
}

/**
 * @brief 将退避上下文写入RTC备份寄存器
 *
 * STANDBY模式下SRAM全部丢失，唯一保留的是RTC备份寄存器。
 * 冷启动后通过读取这些寄存器判断是否从低电保护待机中唤醒。
 *
 * @param retry_count    当前重试次数
 * @param battery_mv     当前电池电压(mV)
 * @param next_period_ms 下次唤醒周期(ms)
 */
static void low_power_runtime_store_ctx(uint32_t retry_count, uint32_t battery_mv, uint32_t next_period_ms)
{
    rtc_lp_backup_write(LP_RTC_CTX_REG_MAGIC,     LOW_POWER_RUNTIME_CTX_MAGIC);
    rtc_lp_backup_write(LP_RTC_CTX_REG_RETRY,     retry_count);
    rtc_lp_backup_write(LP_RTC_CTX_REG_BATT_MV,   battery_mv);
    rtc_lp_backup_write(LP_RTC_CTX_REG_PERIOD_MS, next_period_ms);
}

/**
 * @brief 清除RTC备份域中的退避上下文
 *
 * 正常启动或电压恢复后调用，标记"无待机保护状态"。
 */
static void low_power_runtime_clear_ctx(void)
{
    rtc_lp_backup_write(LP_RTC_CTX_REG_MAGIC,     0U);
    rtc_lp_backup_write(LP_RTC_CTX_REG_RETRY,     0U);
    rtc_lp_backup_write(LP_RTC_CTX_REG_BATT_MV,   0U);
    rtc_lp_backup_write(LP_RTC_CTX_REG_PERIOD_MS, 0U);
}

/**
 * @brief 从RTC备份域加载退避上下文并校验魔数
 *
 * @param ctx 输出参数，加载的上下文数据
 * @return 1=上下文有效（魔数匹配），0=无效
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

/* ===================================================================== */
/*                       时钟策略管理                                       */
/* ===================================================================== */

/**
 * @brief 动态更新系统时钟策略（根据电源锁与运行状态降频/升频）
 *
 * 策略逻辑：
 *   1. 如果用户设置为"强制高性能"，始终使用高频
 *   2. 如果用户设置为"强制中等性能"，始终使用中频
 *   3. 自动模式下：
 *      - 存在高功耗业务锁（热成像/OTA/ESP主机）时，维持高频
 *      - 热成像活跃状态时，维持高频
 *      - 其他情况使用中频（省电）
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

    /* 强制中等性能策略 */
    if (settings->clock_profile_policy == CLOCK_PROFILE_POLICY_MEDIUM_ONLY) {
        clock_profile_set(CLOCK_PROFILE_MEDIUM);
        return;
    }

    /* 自动模式：存在高功耗业务锁或处于热成像活跃状态时，维持高频 */
    if ((lock_mask & (POWER_LOCK_THERMAL | POWER_LOCK_OTA | POWER_LOCK_ESP_HOST)) != 0U ||
        power_manager_get_state() == POWER_STATE_ACTIVE_THERMAL) {
        clock_profile_set(CLOCK_PROFILE_HIGH);
    } else {
        clock_profile_set(CLOCK_PROFILE_MEDIUM);
    }
}

/* ===================================================================== */
/*                    STOP/STANDBY唤醒恢复                                  */
/* ===================================================================== */

/**
 * @brief 处理从STOP/STANDBY唤醒后的硬件与状态恢复
 *
 * 恢复序列：
 *   1. 检查是否由RTC定时器唤醒
 *   2. 恢复系统时钟（根据唤醒源选择是否保留UART休眠状态）
 *   3. 恢复所有PM客户端（GPIO时钟、I2C总线等）
 *   4. 使存储会话失效（唤醒后需重新挂载SD卡）
 *   5. 通知看门狗模块（跳过首周期健康检查）
 *   6. 解除RTC定时器
 *   7. 补偿系统休眠期间流逝的时间（维持FreeRTOS定时器准确性）
 *   8. 强制喂狗（防止看门狗复位）
 */
static void low_power_runtime_handle_post_wake(void)
{
    uint8_t woke_by_timer = rtc_lp_consume_wakeup_event();

    /* 根据唤醒源选择时钟恢复策略 */
    if (woke_by_timer && power_manager_get_state() == POWER_STATE_SCREEN_OFF_IDLE) {
        /* RTC定时器唤醒且处于屏灭空闲状态：保留UART休眠状态（省电） */
        clock_profile_restore_after_stop_keep_uart_sleep();
    } else {
        /* 其他唤醒源：全量恢复所有时钟 */
        clock_profile_restore_after_stop();
    }

    /* 恢复所有PM客户端的外设状态 */
    if (s_pm_clients_registered != 0U) {
        app_pm_restore_stop();  /* 遍历所有注册的PM客户端，调用restore_stop回调 */
    } else {
        /* PM客户端未注册时的降级处理 */
        gpio_pm_restore_after_stop();
        redpic1_thermal_restore_bus_after_stop();
    }

    /* 使存储会话失效，下次使用需重新挂载 */
    storage_service_invalidate_session_after_stop();

    /* 通知看门狗：刚从STOP唤醒，跳过首周期健康检查 */
    watchdog_service_note_stop_wake();

    /* 解除RTC定时器 */
    rtc_lp_disarm();

    /* 补偿系统休眠期间流逝的时间（维持RTOS/业务定时器准确性） */
    if (woke_by_timer) {
        power_manager_advance_sleep_time(rtc_lp_get_last_elapsed_ms());
    }

    /* 强制喂狗，防止看门狗复位 */
    watchdog_service_force_feed();

    /* 切换到运行状态 */
    s_runtime_state = LP_RUNTIME_STATE_RUN;
}

/* ===================================================================== */
/*                     低功耗模式进入函数                                    */
/* ===================================================================== */

/**
 * @brief 执行进入STOP停机模式的完整序列
 *
 * STOP模式特点：
 *   - CPU停止运行，SRAM和寄存器内容保留
 *   - 外设时钟停止（除RTC和IWDG外）
 *   - 唤醒后从停止点继续执行（不需要重新初始化）
 *   - 唤醒时间极快（微秒级）
 *
 * 进入序列：
 *   1. 强制喂狗（确保在休眠期间不会复位）
 *   2. 首次进入时与Host/业务层协商（超时则放弃）
 *   3. 通知PM客户端准备进入STOP
 *   4. 设置RTC唤醒定时器
 *   5. 清除唤醒标志
 *   6. 执行WFI进入STOP模式
 *   7. 唤醒后执行恢复序列
 */
static void low_power_runtime_enter_stop(void)
{
    const device_settings_t *settings = settings_service_get();
    uint32_t wake_period_ms = settings->rtc_stop_wake_ms;
    uint8_t host_ready = 0U;
    esp_host_status_t host_status;

    /* 确保唤醒周期不为0（最小500ms由rtc_lp_arm_ms保证） */
    if (wake_period_ms == 0U) wake_period_ms = 1000U;

    /* 强制喂狗，重置IWDG计数器 */
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

    /* 通知PM客户端准备进入STOP（关闭外设时钟等） */
    if (s_pm_clients_registered != 0U) {
        app_pm_prepare_stop();
    } else {
        gpio_pm_prepare_stop();
    }

    /* 设置RTC唤醒定时器 */
    rtc_lp_arm_ms(wake_period_ms);
    s_runtime_state = LP_RUNTIME_STATE_STOP_IDLE;

    /* 清除唤醒标志，防止立即唤醒 */
    PWR_ClearFlag(PWR_FLAG_WU);

    /* ======== 进入STOP模式（CPU停止，等待RTC唤醒） ======== */
    PWR_EnterSTOPMode(PWR_Regulator_LowPower, PWR_STOPEntry_WFI);

    /* ======== WFI返回后执行唤醒恢复 ======== */
    low_power_runtime_handle_post_wake();
}

/**
 * @brief STANDBY进入公共序列（消除手动/自动待机的重复逻辑）
 *
 * STANDBY模式特点：
 *   - CPU停止，SRAM全部丢失
 *   - 唤醒后相当于复位（冷启动），从main()重新开始
 *   - 唯一保留的是RTC备份寄存器
 *   - 功耗最低（约2~5uA）
 *
 * @param wake_ms   RTC唤醒周期（毫秒）
 * @param is_manual 是否为手动触发（手动触发不保存退避上下文）
 */
static void execute_standby_sequence(uint32_t wake_ms, uint8_t is_manual)
{
    /* 强制喂狗 */
    watchdog_service_force_feed();

    /* 协商各模块进入待机准备状态 */
    (void)low_power_runtime_service_prepare(APP_SERVICE_CMD_PREPARE_STANDBY,
                                            LOW_POWER_RUNTIME_STOP_PREP_TIMEOUT_MS);

    /* 通知PM客户端准备进入STANDBY */
    if (s_pm_clients_registered != 0U) {
        app_pm_prepare_standby();
    } else {
        (void)app_display_runtime_sleep();
        gpio_pm_prepare_standby();
    }

    /* 上下文管理：手动待机清除记录，自动待机保存退避参数 */
    if (is_manual) {
        low_power_runtime_clear_ctx();
    }

    /* 静默外设中断（防止虚假中断唤醒或NVIC挂起位干扰） */
    low_power_runtime_quiesce_irqs_for_standby();

    /* 设置RTC唤醒定时器 */
    rtc_lp_arm_ms(wake_ms);
    s_runtime_state = LP_RUNTIME_STATE_STANDBY_PROTECT;

    /* 清除唤醒/待机标志，防止立即唤醒 */
    PWR_ClearFlag(PWR_FLAG_WU);
    PWR_ClearFlag(PWR_FLAG_SB);

    /* ARM Cortex-M 规范：进入低功耗前必须执行内存屏障，确保所有总线传输完成 */
    __disable_irq();
    __DSB();
    __ISB();

    /* ======== 进入STANDBY模式（CPU停止，SRAM丢失，等待RTC唤醒） ======== */
    PWR_EnterSTANDBYMode();

    /* 注：执行到此行说明已从STANDBY冷启动复位，后续流程由SystemInit接管 */
}

/**
 * @brief 手动触发待机入口
 *
 * 用户通过UI或按键触发的待机请求。
 * 使用固定的10秒唤醒周期，不保存退避上下文。
 */
static void low_power_runtime_enter_manual_standby(void)
{
    execute_standby_sequence(LOW_POWER_RUNTIME_MANUAL_STANDBY_WAKE_MS, 1U);
}

/**
 * @brief 自动低电待机入口（带指数退避上下文保存）
 *
 * 低电保护机制：
 *   1. 检测到电池电压 < 3000mV 且屏灭超过30分钟
 *   2. 保存退避上下文到RTC备份域
 *   3. 进入STANDBY模式
 *   4. 唤醒后检查电压，若仍低电则增加退避阶数重新进入
 *   5. 直到电压恢复到3300mV以上才正常启动
 *
 * @param retry_count 当前退避重试次数（首次为0）
 */
static void low_power_runtime_enter_standby(uint32_t retry_count)
{
    uint32_t battery_mv = battery_monitor_get_mv();
    uint32_t next_period_ms = low_power_runtime_next_standby_period_ms(retry_count);

    /* 保存退避上下文至备份域，供冷启动后读取 */
    low_power_runtime_store_ctx(retry_count, battery_mv, next_period_ms);
    execute_standby_sequence(next_period_ms, 0U);
}

/* ===================================================================== */
/*                           公开API实现                                   */
/* ===================================================================== */

/**
 * @brief 处理早期启动阶段的低电保护逻辑
 *
 * 在main()早期调用（设置服务初始化后、主循环前）。
 * 若从STANDBY冷启动唤醒，且电池电压仍低于恢复阈值(3300mV)，
 * 则直接重新进入待机并增加退避阶数，防止低电死循环重启。
 *
 * @return 1=已重新进入待机（不会返回），0=正常启动
 */
uint8_t low_power_runtime_handle_early_boot(void)
{
    const device_settings_t *settings = settings_service_get();
    low_power_standby_ctx_t ctx;

    /* 非STANDBY唤醒则跳过 */
    if (rtc_lp_woke_from_standby() == 0U) return 0U;

    /* RTC备份域上下文无效则跳过 */
    if (low_power_runtime_load_ctx(&ctx) == 0U) return 0U;

    /* 用户关闭待机策略或处于非ECO模式，清除上下文并正常启动 */
    if (settings->standby_enabled == 0U || settings->power_policy != POWER_POLICY_ECO) {
        low_power_runtime_clear_ctx();
        return 0U;
    }

    /* 电压已恢复至安全阈值以上，清除保护状态并正常启动 */
    if (battery_monitor_get_mv() >= LOW_POWER_RUNTIME_STANDBY_RECOVER_MV) {
        low_power_runtime_clear_ctx();
        return 0U;
    }

    /* 仍处于低电状态，递增重试次数并重新进入待机保护 */
    low_power_runtime_enter_standby(ctx.retry_count + 1U);
    return 1U; /* 不会执行到此，系统已复位 */
}

/**
 * @brief 低功耗运行时模块初始化
 *
 * 注册PM客户端，重置运行时状态。
 * 在主循环启动前调用。
 */
void low_power_runtime_init(void)
{
    low_power_runtime_register_pm_clients();
    s_runtime_state = LP_RUNTIME_STATE_RUN;
    s_last_power_state = power_manager_get_state();
    s_screen_off_enter_ms = 0U;
    s_stop_host_prepared = 0U;
    s_manual_standby_pending = 0U;
}

/**
 * @brief 低功耗调度主状态机（每25ms调用一次）
 *
 * 决策优先级（从高到低）：
 *   1. 同步电源状态变化
 *   2. 动态时钟策略调整
 *   3. 手动待机请求 → 直接进入STANDBY
 *   4. 非屏灭状态 → WFI空闲等待
 *   5. 看门狗禁止休眠 → WFI空闲等待
 *   6. ECO模式 + 待机使能 + 屏灭30分钟 + 低电3000mV → 自动STANDBY
 *   7. 默认 → 进入STOP模式（RTC定时唤醒）
 *
 * 注意：本函数从FreeRTOS任务中调用，WFI会让CPU进入低功耗直到下一个中断。
 * STOP模式下FreeRTOS的tick会停止，唤醒后通过power_manager_advance_sleep_time()补偿。
 */
void low_power_runtime_step(void)
{
    const device_settings_t *settings = settings_service_get();
    uint32_t now_ms = power_manager_get_tick_ms();
    uint32_t screen_off_elapsed_ms = 0U;

    /* 1. 电源状态变化同步（检测屏灭/亮屏切换） */
    if (power_manager_get_state() != s_last_power_state) {
        s_last_power_state = power_manager_get_state();
        if (s_last_power_state == POWER_STATE_SCREEN_OFF_IDLE) {
            s_screen_off_enter_ms = now_ms; /* 记录进入屏灭的时间 */
        }
        s_stop_host_prepared = 0U; /* 状态切换后需重新协商 */
    }

    /* 2. 动态时钟策略调整（高性能/中等性能切换） */
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
        __WFI(); /* 等待中断（CPU低功耗，任何中断唤醒） */
        return;
    }

    /* 5. 看门狗安全拦截：若业务层禁止休眠（有任务未完成健康检查），则放弃本次STOP */
    if (((s_pm_clients_registered != 0U) && (app_pm_can_enter_stop() == 0U)) ||
        ((s_pm_clients_registered == 0U) && (watchdog_service_can_enter_stop() == 0U))) {
        s_runtime_state = LP_RUNTIME_STATE_RUN;
        __WFI();
        return;
    }

    /* 6. 自动待机决策：ECO模式 + 待机使能 + 屏灭超过30分钟 + 电池电压低于3000mV */
    screen_off_elapsed_ms = now_ms - s_screen_off_enter_ms;
    if (settings->power_policy == POWER_POLICY_ECO &&
        settings->standby_enabled != 0U &&
        screen_off_elapsed_ms >= LOW_POWER_RUNTIME_STANDBY_IDLE_MS &&
        battery_monitor_get_mv() < LOW_POWER_RUNTIME_STANDBY_LOW_MV) {
        low_power_runtime_enter_standby(0U);
        return;
    }

    /* 7. 默认策略：进入STOP停机模式（RTC定时唤醒，约5秒后唤醒一次） */
    low_power_runtime_enter_stop();
}

/**
 * @brief 获取当前低功耗运行时状态
 * @return 当前状态枚举值
 */
lp_runtime_state_t low_power_runtime_get_state(void)
{
    return s_runtime_state;
}

/**
 * @brief 请求进入手动待机模式（可从中断或任意上下文调用）
 *
 * 设置待机挂起标志，由low_power_runtime_step()在主循环中处理。
 * 标志声明为volatile，确保多线程/中断安全。
 */
void low_power_runtime_request_manual_standby(void)
{
    s_manual_standby_pending = 1U;
}
