/**
 * @file    battery_monitor.c
 * @brief   电池电压监测与电量估算模块
 * @note    本模块通过 ADC 采样电池电压，结合分压比和校准参数计算实际电压，
 *          再通过电压-电量曲线映射百分比，最后经迟滞处理输出稳定的电量等级。
 *
 * @par 采样策略
 *      根据电源状态动态调整采样间隔：
 *      - 启动快速采样:   1000ms（前 3 次）
 *      - 热成像活跃:     10000ms
 *      - UI 活跃:        15000ms
 *      - 熄屏空闲:       20000ms
 *
 * @par 电压滤波
 *      采用非对称自适应滤波：
 *      - 电压下降时快速响应（模拟电池放电特性）
 *      - 电压上升时慢速响应（充电时平滑过渡）
 *      - 根据 |ΔV| 分 4 档调整滤波权重
 *
 * @par 电量等级
 *      5 级电量（带 4% 迟滞）：
 *      - FULL:   ≥85%
 *      - HIGH:   ≥60%
 *      - MEDIUM: ≥35%
 *      - LOW:    ≥15%
 *      - ALERT:  <15%
 *
 * @par 电压-电量映射
 *      使用 32 点查找表进行分段线性插值，
 *      覆盖 3300mV(0%) ~ 4200mV(100%) 的锂电池放电曲线。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "battery_monitor.h"

#include "adc.h"
#include "power_manager.h"

/* =========================================================================
 *  2. 内部宏定义 —— ADC 与采样参数
 * ======================================================================= */

#define BATTERY_ADC_CHANNEL              ADC_Channel_8   /**< ADC 通道（PB0）   */
#define BATTERY_ADC_SAMPLE_COUNT         8U              /**< 每次采样平均次数  */
#define BATTERY_BOOTSTRAP_SAMPLE_COUNT   4U              /**< 启动采样次数      */

/* --- 采样间隔（ms） --- */
#define BATTERY_STARTUP_FAST_SAMPLE_MS   1000UL          /**< 启动快速采样间隔  */
#define BATTERY_STARTUP_FAST_SAMPLE_COUNT 3U             /**< 启动快速采样次数  */
#define BATTERY_ACTIVE_SAMPLE_MS         10000UL         /**< 热成像活跃间隔    */
#define BATTERY_UI_SAMPLE_MS             15000UL         /**< UI 活跃间隔       */
#define BATTERY_SCREEN_OFF_SAMPLE_MS     20000UL         /**< 熄屏空闲间隔      */

/* --- 电量等级阈值（%） --- */
#define BATTERY_LEVEL_FULL_PERCENT       85U             /**< 满电阈值          */
#define BATTERY_LEVEL_HIGH_PERCENT       60U             /**< 高电阈值          */
#define BATTERY_LEVEL_MEDIUM_PERCENT     35U             /**< 中电阈值          */
#define BATTERY_LEVEL_LOW_PERCENT        15U             /**< 低电阈值          */
#define BATTERY_LEVEL_HYSTERESIS_PERCENT 4U              /**< 迟滞百分比        */

/* --- 低压告警阈值（mV） --- */
#define BATTERY_LOW_WARNING_MV           3600U           /**< 低压告警电压      */

/* =========================================================================
 *  3. 内部类型定义
 * ======================================================================= */

/**
 * @brief 电池监测状态结构体
 */
typedef struct
{
    uint16_t        mv;         /**< 滤波后电压（mV）     */
    uint16_t        raw_mv;     /**< 原始采样电压（mV）   */
    uint8_t         percent;    /**< 电量百分比（0~100）  */
    uint8_t         charging;   /**< 充电状态标志         */
    battery_level_t level;      /**< 电量等级枚举         */
} battery_monitor_state_t;

/**
 * @brief 电压-电量查找表条目
 */
typedef struct
{
    uint16_t mv;        /**< 电压阈值（mV）   */
    uint8_t  percent;   /**< 对应电量百分比   */
} battery_percent_point_t;

/* =========================================================================
 *  4. 模块级常量 —— 电压-电量查找表与默认校准
 * ======================================================================= */

/** 锂电池放电曲线查找表（32 点，电压降序排列） */
static const battery_percent_point_t s_battery_percent_curve[] =
{
    { 4200U, 100U },    /* 4.20V → 100% */
    { 4180U,  99U },    /* 4.18V →  99% */
    { 4160U,  96U },    /* 4.16V →  96% */
    { 4140U,  93U },    /* 4.14V →  93% */
    { 4120U,  90U },    /* 4.12V →  90% */
    { 4100U,  87U },    /* 4.10V →  87% */
    { 4080U,  84U },    /* 4.08V →  84% */
    { 4060U,  80U },    /* 4.06V →  80% */
    { 4040U,  76U },    /* 4.04V →  76% */
    { 4020U,  72U },    /* 4.02V →  72% */
    { 4000U,  68U },    /* 4.00V →  68% */
    { 3980U,  63U },    /* 3.98V →  63% */
    { 3960U,  58U },    /* 3.96V →  58% */
    { 3940U,  54U },    /* 3.94V →  54% */
    { 3920U,  50U },    /* 3.92V →  50% */
    { 3900U,  46U },    /* 3.90V →  46% */
    { 3880U,  42U },    /* 3.88V →  42% */
    { 3860U,  38U },    /* 3.86V →  38% */
    { 3840U,  34U },    /* 3.84V →  34% */
    { 3820U,  30U },    /* 3.82V →  30% */
    { 3800U,  26U },    /* 3.80V →  26% */
    { 3780U,  22U },    /* 3.78V →  22% */
    { 3760U,  18U },    /* 3.76V →  18% */
    { 3740U,  15U },    /* 3.74V →  15% */
    { 3720U,  12U },    /* 3.72V →  12% */
    { 3700U,  10U },    /* 3.70V →  10% */
    { 3680U,   8U },    /* 3.68V →   8% */
    { 3660U,   6U },    /* 3.66V →   6% */
    { 3640U,   5U },    /* 3.64V →   5% */
    { 3600U,   3U },    /* 3.60V →   3% */
    { 3500U,   1U },    /* 3.50V →   1% */
    { 3300U,   0U }     /* 3.30V →   0% */
};

/** 默认校准参数（适用于标准 2:1 分压电阻、3.3V 参考） */
static const battery_monitor_calibration_t s_default_calibration =
{
    3300U,      /* adc_ref_mv:            ADC 参考电压 3300mV  */
    2000U,      /* divider_scale_milli:   分压比 ×1000（2:1）  */
    0,          /* voltage_offset_mv:     电压偏移补偿 0mV     */
    3300U,      /* percent_empty_mv:      0% 对应电压 3300mV   */
    4200U       /* percent_full_mv:       100% 对应电压 4200mV  */
};

/* =========================================================================
 *  5. 模块级静态变量
 * ======================================================================= */

static battery_monitor_state_t s_battery_state =           /**< 当前电池状态     */
{
    0U,                     /* mv */
    0U,                     /* raw_mv */
    0U,                     /* percent */
    0U,                     /* charging */
    BATTERY_LEVEL_ALERT     /* level */
};

static battery_monitor_calibration_t s_battery_calibration = /**< 当前校准参数   */
{
    3300U, 2000U, 0, 3300U, 4200U
};

static uint32_t s_last_sample_ms = 0U;                     /**< 上次采样时间戳   */
static uint8_t  s_startup_fast_samples_remaining = 0U;     /**< 启动快速采样剩余次数 */

/* =========================================================================
 *  6. 前向声明
 * ======================================================================= */

static uint16_t battery_monitor_sample_mv(void);
static uint16_t battery_monitor_bootstrap_sample_mv(void);
static uint32_t battery_monitor_enter_critical(void);
static void     battery_monitor_exit_critical(uint32_t primask);
static battery_monitor_state_t battery_monitor_get_state_snapshot(void);
static uint32_t battery_monitor_sample_interval_ms(void);
static uint16_t battery_monitor_filter_mv(uint16_t current_mv, uint16_t sample_mv, uint8_t charging);
static uint8_t  battery_monitor_percent_from_mv(uint16_t mv);
static battery_level_t battery_monitor_level_from_percent(uint8_t percent);
static battery_level_t battery_monitor_apply_hysteresis(uint8_t percent, battery_level_t current_level);
static void     battery_monitor_store_state(uint16_t raw_mv, uint16_t filtered_mv, uint8_t charging);

/* =========================================================================
 *  7. 内部函数实现 —— 临界区与状态快照
 * ======================================================================= */

/**
 * @brief  进入临界区（保存并禁用中断）
 * @return 保存的 PRIMASK 值（用于退出时恢复）
 */
static uint32_t battery_monitor_enter_critical(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    return primask;
}

/**
 * @brief  退出临界区（恢复中断状态）
 * @param  primask — 进入时保存的 PRIMASK 值
 */
static void battery_monitor_exit_critical(uint32_t primask)
{
    __set_PRIMASK(primask);
}

/**
 * @brief  获取电池状态的原子快照
 * @return 状态结构体副本
 */
static battery_monitor_state_t battery_monitor_get_state_snapshot(void)
{
    battery_monitor_state_t snapshot;
    uint32_t primask = battery_monitor_enter_critical();

    snapshot = s_battery_state;
    battery_monitor_exit_critical(primask);
    return snapshot;
}

/* =========================================================================
 *  8. 内部函数实现 —— ADC 采样
 * ======================================================================= */

/**
 * @brief  执行一次 ADC 采样并转换为电压值（mV）
 * @note   公式：V_mV = ADC_raw × Vref × scale / 4095 / 1000 + offset
 *         其中 scale 为分压比 ×1000，Vref 为 ADC 参考电压。
 * @return 采样电压（mV），0 表示无效
 */
static uint16_t battery_monitor_sample_mv(void)
{
    battery_monitor_calibration_t calibration;
    uint32_t adc_average = Get_Adc_Average(BATTERY_ADC_CHANNEL, BATTERY_ADC_SAMPLE_COUNT);
    uint64_t scaled_mv = 0U;
    int32_t adjusted_mv = 0;
    uint32_t primask = battery_monitor_enter_critical();

    calibration = s_battery_calibration;
    battery_monitor_exit_critical(primask);

    /* ADC 值 → 实际电压（mV）：考虑参考电压和分压比 */
    scaled_mv = ((uint64_t)adc_average *
                 (uint64_t)calibration.adc_ref_mv *
                 (uint64_t)calibration.divider_scale_milli) / 4095ULL;
    scaled_mv = (scaled_mv + 500ULL) / 1000ULL;    /* 四舍五入 */

    /* 应用电压偏移补偿 */
    adjusted_mv = (int32_t)scaled_mv + (int32_t)calibration.voltage_offset_mv;
    if (adjusted_mv <= 0)
    {
        return 0U;
    }

    return (uint16_t)adjusted_mv;
}

/**
 * @brief  启动阶段多次采样取均值（去除最高值）
 * @note   采集 4 次样本，去掉最大值后取平均，
 *         减少启动瞬间的电压尖峰影响。
 * @return 启动采样电压（mV），0 表示全部无效
 */
static uint16_t battery_monitor_bootstrap_sample_mv(void)
{
    uint32_t sum_mv = 0U;
    uint16_t max_mv = 0U;
    uint8_t valid_count = 0U;
    uint8_t index = 0U;

    for (index = 0U; index < BATTERY_BOOTSTRAP_SAMPLE_COUNT; ++index)
    {
        uint16_t sample_mv = battery_monitor_sample_mv();

        if (sample_mv == 0U)
        {
            continue;
        }

        sum_mv += sample_mv;
        if (sample_mv > max_mv)
        {
            max_mv = sample_mv;
        }
        ++valid_count;
    }

    if (valid_count == 0U)
    {
        return 0U;
    }

    /* 有效样本 ≥3 时去掉最高值，减少启动尖峰 */
    if (valid_count >= 3U)
    {
        sum_mv -= max_mv;
        --valid_count;
    }

    return (uint16_t)((sum_mv + ((uint32_t)valid_count / 2UL)) / (uint32_t)valid_count);
}

/* =========================================================================
 *  9. 内部函数实现 —— 采样间隔策略
 * ======================================================================= */

/**
 * @brief  根据电源状态返回当前采样间隔
 * @return 采样间隔（ms）
 */
static uint32_t battery_monitor_sample_interval_ms(void)
{
    power_state_t power_state = power_manager_get_state();

    /* 启动快速采样阶段 */
    if (s_startup_fast_samples_remaining != 0U)
    {
        return BATTERY_STARTUP_FAST_SAMPLE_MS;
    }

    /* 根据电源状态选择采样间隔 */
    if (power_state == POWER_STATE_ACTIVE_THERMAL)
    {
        return BATTERY_ACTIVE_SAMPLE_MS;
    }
    if (power_state == POWER_STATE_SCREEN_OFF_IDLE)
    {
        return BATTERY_SCREEN_OFF_SAMPLE_MS;
    }

    return BATTERY_UI_SAMPLE_MS;
}

/* =========================================================================
 *  10. 内部函数实现 —— 电压自适应滤波
 * ======================================================================= */

/**
 * @brief  自适应电压滤波
 * @note   根据电压变化方向和幅度动态调整滤波权重：
 *
 *         电压下降（放电）—— 快速响应：
 *         |ΔV| ≥ 220mV → 直接采用新值
 *         |ΔV| ≥ 100mV → 50% 旧 + 50% 新
 *         |ΔV| ≥  40mV → 67% 旧 + 33% 新
 *         |ΔV| <  40mV → 75% 旧 + 25% 新
 *
 *         电压上升（充电）—— 慢速响应：
 *         |ΔV| ≥ 220mV → 50% 旧 + 50% 新
 *         |ΔV| ≥ 100mV → 75% 旧 + 25% 新
 *         |ΔV| <  100mV → 83% 旧 + 17% 新
 *
 *         电压上升（非充电）—— 中速响应：
 *         |ΔV| ≥ 220mV → 75% 旧 + 25% 新
 *         |ΔV| ≥ 100mV → 83% 旧 + 17% 新
 *         |ΔV| ≥  40mV → 86% 旧 + 14% 新
 *         |ΔV| <  40mV → 88% 旧 + 12% 新
 *
 * @param  current_mv — 当前滤波值（mV）
 * @param  sample_mv  — 新采样值（mV）
 * @param  charging   — 充电状态
 * @return 滤波后电压（mV）
 */
static uint16_t battery_monitor_filter_mv(uint16_t current_mv, uint16_t sample_mv, uint8_t charging)
{
    uint16_t delta_mv = 0U;

    /* 无效值保护 */
    if (sample_mv == 0U)
    {
        return current_mv;
    }
    if (current_mv == 0U)
    {
        return sample_mv;
    }

    /* 电压下降方向（放电）—— 快速响应 */
    if (sample_mv <= current_mv)
    {
        delta_mv = (uint16_t)(current_mv - sample_mv);

        if (delta_mv >= 220U)
        {
            return sample_mv;    /* 大幅下降：直接采用 */
        }
        if (delta_mv >= 100U)
        {
            return (uint16_t)(((uint32_t)current_mv + (uint32_t)sample_mv + 1UL) / 2UL);  /* 50/50 */
        }
        if (delta_mv >= 40U)
        {
            return (uint16_t)(((uint32_t)current_mv * 2UL + (uint32_t)sample_mv + 1UL) / 3UL); /* 67/33 */
        }

        return (uint16_t)(((uint32_t)current_mv * 3UL + (uint32_t)sample_mv + 2UL) / 4UL); /* 75/25 */
    }

    /* 电压上升方向 */
    delta_mv = (uint16_t)(sample_mv - current_mv);

    /* 充电状态 —— 慢速响应 */
    if (charging != 0U)
    {
        if (delta_mv >= 220U)
        {
            return (uint16_t)(((uint32_t)current_mv + (uint32_t)sample_mv + 1UL) / 2UL);  /* 50/50 */
        }
        if (delta_mv >= 100U)
        {
            return (uint16_t)(((uint32_t)current_mv * 3UL + (uint32_t)sample_mv + 2UL) / 4UL); /* 75/25 */
        }

        return (uint16_t)(((uint32_t)current_mv * 5UL + (uint32_t)sample_mv + 3UL) / 6UL); /* 83/17 */
    }

    /* 非充电状态 —— 中速响应 */
    if (delta_mv >= 220U)
    {
        return (uint16_t)(((uint32_t)current_mv * 3UL + (uint32_t)sample_mv + 2UL) / 4UL); /* 75/25 */
    }
    if (delta_mv >= 100U)
    {
        return (uint16_t)(((uint32_t)current_mv * 5UL + (uint32_t)sample_mv + 3UL) / 6UL); /* 83/17 */
    }
    if (delta_mv >= 40U)
    {
        return (uint16_t)(((uint32_t)current_mv * 6UL + (uint32_t)sample_mv + 3UL) / 7UL); /* 86/14 */
    }

    return (uint16_t)(((uint32_t)current_mv * 7UL + (uint32_t)sample_mv + 4UL) / 8UL); /* 88/12 */
}

/* =========================================================================
 *  11. 内部函数实现 —— 电压→电量转换
 * ======================================================================= */

/**
 * @brief  将电压值映射为电量百分比
 * @note   在 32 点查找表中找到所在区间，进行分段线性插值。
 *         插值公式：percent = lower% + (V - lower_V) × (upper% - lower%) / (upper_V - lower_V)
 * @param  mv — 电压值（mV）
 * @return 电量百分比（0~100）
 */
static uint8_t battery_monitor_percent_from_mv(uint16_t mv)
{
    battery_monitor_calibration_t calibration;
    uint32_t primask = battery_monitor_enter_critical();
    uint32_t index = 0U;

    calibration = s_battery_calibration;
    battery_monitor_exit_critical(primask);

    /* 钳位到校准范围 */
    if (mv <= calibration.percent_empty_mv)
    {
        return 0U;
    }
    if (mv >= calibration.percent_full_mv)
    {
        return 100U;
    }

    /* 在查找表中查找所在区间并插值 */
    for (index = 0U; index + 1U < (sizeof(s_battery_percent_curve) / sizeof(s_battery_percent_curve[0])); ++index)
    {
        const battery_percent_point_t *upper = &s_battery_percent_curve[index];
        const battery_percent_point_t *lower = &s_battery_percent_curve[index + 1U];

        if (mv <= upper->mv && mv >= lower->mv)
        {
            uint32_t span_mv      = (uint32_t)(upper->mv - lower->mv);
            uint32_t offset_mv    = (uint32_t)(mv - lower->mv);
            uint32_t span_percent = (uint32_t)(upper->percent - lower->percent);
            uint32_t interpolated = ((uint32_t)lower->percent * span_mv) +
                                    (offset_mv * span_percent) +
                                    (span_mv / 2UL);    /* 四舍五入 */

            if (span_mv == 0U)
            {
                return upper->percent;
            }

            return (uint8_t)(interpolated / span_mv);
        }
    }

    return 0U;
}

/* =========================================================================
 *  12. 内部函数实现 —— 电量等级判定
 * ======================================================================= */

/**
 * @brief  根据百分比直接判定电量等级（无迟滞）
 * @param  percent — 电量百分比
 * @return 电量等级
 */
static battery_level_t battery_monitor_level_from_percent(uint8_t percent)
{
    if (percent >= BATTERY_LEVEL_FULL_PERCENT)
    {
        return BATTERY_LEVEL_FULL;
    }
    if (percent >= BATTERY_LEVEL_HIGH_PERCENT)
    {
        return BATTERY_LEVEL_HIGH;
    }
    if (percent >= BATTERY_LEVEL_MEDIUM_PERCENT)
    {
        return BATTERY_LEVEL_MEDIUM;
    }
    if (percent >= BATTERY_LEVEL_LOW_PERCENT)
    {
        return BATTERY_LEVEL_LOW;
    }

    return BATTERY_LEVEL_ALERT;
}

/**
 * @brief  带迟滞的电量等级判定
 * @note   在等级边界增加 ±4% 的迟滞区间，防止电量波动导致频繁跳级。
 *         例如：从 FULL 降到 HIGH 需要 <81%，从 HIGH 升到 FULL 需要 ≥89%。
 * @param  percent       — 当前电量百分比
 * @param  current_level — 当前电量等级
 * @return 新的电量等级
 */
static battery_level_t battery_monitor_apply_hysteresis(uint8_t percent, battery_level_t current_level)
{
    switch (current_level)
    {
    case BATTERY_LEVEL_FULL:
        /* FULL → HIGH: 电量降至 81% 以下 */
        if (percent < (BATTERY_LEVEL_FULL_PERCENT - BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_HIGH;
        }
        return BATTERY_LEVEL_FULL;

    case BATTERY_LEVEL_HIGH:
        /* HIGH → FULL: 电量升至 89% 以上 */
        if (percent >= (BATTERY_LEVEL_FULL_PERCENT + BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_FULL;
        }
        /* HIGH → MEDIUM: 电量降至 31% 以下 */
        if (percent < (BATTERY_LEVEL_MEDIUM_PERCENT - BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_MEDIUM;
        }
        return BATTERY_LEVEL_HIGH;

    case BATTERY_LEVEL_MEDIUM:
        /* MEDIUM → HIGH: 电量升至 64% 以上 */
        if (percent >= (BATTERY_LEVEL_HIGH_PERCENT + BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_HIGH;
        }
        /* MEDIUM → LOW: 电量降至 11% 以下 */
        if (percent < (BATTERY_LEVEL_LOW_PERCENT - BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_LOW;
        }
        return BATTERY_LEVEL_MEDIUM;

    case BATTERY_LEVEL_LOW:
        /* LOW → MEDIUM: 电量升至 39% 以上 */
        if (percent >= (BATTERY_LEVEL_MEDIUM_PERCENT + BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_MEDIUM;
        }
        /* LOW → ALERT: 电量降至 4% 以下 */
        if (percent < BATTERY_LEVEL_HYSTERESIS_PERCENT)
        {
            return BATTERY_LEVEL_ALERT;
        }
        return BATTERY_LEVEL_LOW;

    case BATTERY_LEVEL_ALERT:
    default:
        /* ALERT → LOW: 电量升至 19% 以上 */
        if (percent >= (BATTERY_LEVEL_LOW_PERCENT + BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_LOW;
        }
        return BATTERY_LEVEL_ALERT;
    }
}

/* =========================================================================
 *  13. 内部函数实现 —— 状态存储
 * ======================================================================= */

/**
 * @brief  更新电池状态（原子写入）
 * @note   计算电量百分比和等级，应用迟滞后写入全局状态。
 *         首次采样（mv==0）时跳过迟滞，直接使用百分比判定。
 * @param  raw_mv      — 原始采样电压（mV）
 * @param  filtered_mv — 滤波后电压（mV）
 * @param  charging    — 充电状态
 */
static void battery_monitor_store_state(uint16_t raw_mv, uint16_t filtered_mv, uint8_t charging)
{
    battery_monitor_state_t next_state;
    battery_monitor_state_t current_state = battery_monitor_get_state_snapshot();
    uint32_t primask = 0U;

    next_state.raw_mv   = raw_mv;
    next_state.mv       = filtered_mv;
    next_state.charging = charging;
    next_state.percent  = battery_monitor_percent_from_mv(filtered_mv);

    /* 首次采样时跳过迟滞（无历史等级参考） */
    next_state.level = battery_monitor_apply_hysteresis(next_state.percent, current_state.level);
    if (current_state.mv == 0U)
    {
        next_state.level = battery_monitor_level_from_percent(next_state.percent);
    }

    /* 原子写入全局状态 */
    primask = battery_monitor_enter_critical();
    s_battery_state = next_state;
    battery_monitor_exit_critical(primask);
}

/* =========================================================================
 *  14. 公共接口实现 —— 初始化
 * ======================================================================= */

/**
 * @brief  初始化电池监测模块
 * @note   初始化 ADC，执行启动采样，设置快速采样计数器。
 */
void battery_monitor_init(void)
{
    uint16_t sample_mv = 0U;

    Adc_Init();
    sample_mv = battery_monitor_bootstrap_sample_mv();
    battery_monitor_store_state(sample_mv, sample_mv, 0U);
    s_last_sample_ms = power_manager_get_tick_ms();
    s_startup_fast_samples_remaining = BATTERY_STARTUP_FAST_SAMPLE_COUNT;
}

/* =========================================================================
 *  15. 公共接口实现 —— 周期采样
 * ======================================================================= */

/**
 * @brief  执行一次电池监测周期
 * @note   检查采样间隔，到期时执行 ADC 采样、滤波和状态更新。
 *         启动阶段前 3 次使用快速间隔（1s），之后根据电源状态切换。
 */
void battery_monitor_step(void)
{
    battery_monitor_state_t current_state = battery_monitor_get_state_snapshot();
    uint32_t now_ms = power_manager_get_tick_ms();
    uint32_t sample_interval_ms = battery_monitor_sample_interval_ms();
    uint16_t sample_mv = 0U;
    uint16_t filtered_mv = 0U;

    /* 未到采样间隔时跳过 */
    if ((now_ms - s_last_sample_ms) < sample_interval_ms)
    {
        return;
    }

    /* 执行采样、滤波、存储 */
    sample_mv = battery_monitor_sample_mv();
    s_last_sample_ms = now_ms;
    filtered_mv = battery_monitor_filter_mv(current_state.mv, sample_mv, current_state.charging);
    battery_monitor_store_state(sample_mv, filtered_mv, current_state.charging);

    /* 启动快速采样计数递减 */
    if (s_startup_fast_samples_remaining != 0U)
    {
        --s_startup_fast_samples_remaining;
    }
}

/* =========================================================================
 *  16. 公共接口实现 —— 校准参数
 * ======================================================================= */

/**
 * @brief  设置电池校准参数
 * @note   设置后自动执行一次启动采样以应用新参数。
 *         calibration 为 0 时恢复默认校准。
 * @param  calibration — 校准参数指针（0=恢复默认）
 */
void battery_monitor_set_calibration(const battery_monitor_calibration_t *calibration)
{
    battery_monitor_calibration_t next_calibration;
    battery_monitor_state_t state = battery_monitor_get_state_snapshot();
    uint16_t sample_mv = 0U;
    uint32_t primask = 0U;

    if (calibration == 0)
    {
        next_calibration = s_default_calibration;
    }
    else
    {
        next_calibration = *calibration;

        /* 参数有效性校验，无效时使用默认值 */
        if (next_calibration.adc_ref_mv == 0U)
        {
            next_calibration.adc_ref_mv = s_default_calibration.adc_ref_mv;
        }
        if (next_calibration.divider_scale_milli == 0U)
        {
            next_calibration.divider_scale_milli = s_default_calibration.divider_scale_milli;
        }
        if (next_calibration.percent_empty_mv >= next_calibration.percent_full_mv)
        {
            next_calibration.percent_empty_mv = s_default_calibration.percent_empty_mv;
            next_calibration.percent_full_mv  = s_default_calibration.percent_full_mv;
        }
    }

    /* 原子更新校准参数 */
    primask = battery_monitor_enter_critical();
    s_battery_calibration = next_calibration;
    battery_monitor_exit_critical(primask);

    /* 重新采样以应用新校准 */
    sample_mv = battery_monitor_bootstrap_sample_mv();
    battery_monitor_store_state(sample_mv, sample_mv, state.charging);
    s_last_sample_ms = power_manager_get_tick_ms();
    s_startup_fast_samples_remaining = BATTERY_STARTUP_FAST_SAMPLE_COUNT;
}

/**
 * @brief  获取当前校准参数
 * @return 校准参数结构体副本
 */
battery_monitor_calibration_t battery_monitor_get_calibration(void)
{
    battery_monitor_calibration_t calibration;
    uint32_t primask = battery_monitor_enter_critical();

    calibration = s_battery_calibration;
    battery_monitor_exit_critical(primask);
    return calibration;
}

/* =========================================================================
 *  17. 公共接口实现 —— 充电状态
 * ======================================================================= */

/**
 * @brief  设置充电状态
 * @param  is_charging — 1=充电中；0=未充电
 */
void battery_monitor_set_charging(uint8_t is_charging)
{
    battery_monitor_state_t state = battery_monitor_get_state_snapshot();

    if (state.charging == ((is_charging != 0U) ? 1U : 0U))
    {
        return;
    }

    battery_monitor_store_state(state.raw_mv, state.mv, (is_charging != 0U) ? 1U : 0U);
}

/**
 * @brief  查询充电状态
 * @retval 1 — 充电中；0 — 未充电
 */
uint8_t battery_monitor_is_charging(void)
{
    return battery_monitor_get_state_snapshot().charging;
}

/* =========================================================================
 *  18. 公共接口实现 —— 电量查询
 * ======================================================================= */

/**
 * @brief  获取滤波后电压
 * @return 电压值（mV）
 */
uint16_t battery_monitor_get_mv(void)
{
    return battery_monitor_get_state_snapshot().mv;
}

/**
 * @brief  获取电量百分比
 * @return 电量百分比（0~100）
 */
uint8_t battery_monitor_get_percent(void)
{
    return battery_monitor_get_state_snapshot().percent;
}

/**
 * @brief  获取电量等级
 * @return 电量等级枚举（FULL/HIGH/MEDIUM/LOW/ALERT）
 */
battery_level_t battery_monitor_get_level(void)
{
    return battery_monitor_get_state_snapshot().level;
}

/**
 * @brief  判断电池是否低压
 * @note   电压低于 3600mV 时视为低压。
 * @retval 1 — 低压；0 — 正常
 */
uint8_t battery_monitor_is_low(void)
{
    return (battery_monitor_get_mv() <= BATTERY_LOW_WARNING_MV) ? 1U : 0U;
}
