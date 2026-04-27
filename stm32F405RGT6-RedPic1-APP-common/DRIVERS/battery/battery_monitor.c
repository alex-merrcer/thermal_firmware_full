#include "battery_monitor.h"

#include "adc.h"
#include "power_manager.h"

#define BATTERY_ADC_CHANNEL ADC_Channel_8
#define BATTERY_ADC_SAMPLE_COUNT 8U
#define BATTERY_BOOTSTRAP_SAMPLE_COUNT 4U
#define BATTERY_STARTUP_FAST_SAMPLE_MS 1000UL
#define BATTERY_STARTUP_FAST_SAMPLE_COUNT 3U
#define BATTERY_ACTIVE_SAMPLE_MS 10000UL
#define BATTERY_UI_SAMPLE_MS 15000UL
#define BATTERY_SCREEN_OFF_SAMPLE_MS 20000UL
#define BATTERY_LEVEL_FULL_PERCENT 85U
#define BATTERY_LEVEL_HIGH_PERCENT 60U
#define BATTERY_LEVEL_MEDIUM_PERCENT 35U
#define BATTERY_LEVEL_LOW_PERCENT 15U
#define BATTERY_LEVEL_HYSTERESIS_PERCENT 4U
#define BATTERY_LOW_WARNING_MV 3600U

typedef struct
{
    uint16_t mv;
    uint16_t raw_mv;
    uint8_t percent;
    uint8_t charging;
    battery_level_t level;
} battery_monitor_state_t;

typedef struct
{
    uint16_t mv;
    uint8_t percent;
} battery_percent_point_t;

static const battery_percent_point_t s_battery_percent_curve[] =
{
    { 4200U, 100U },
    { 4180U, 99U },
    { 4160U, 96U },
    { 4140U, 93U },
    { 4120U, 90U },
    { 4100U, 87U },
    { 4080U, 84U },
    { 4060U, 80U },
    { 4040U, 76U },
    { 4020U, 72U },
    { 4000U, 68U },
    { 3980U, 63U },
    { 3960U, 58U },
    { 3940U, 54U },
    { 3920U, 50U },
    { 3900U, 46U },
    { 3880U, 42U },
    { 3860U, 38U },
    { 3840U, 34U },
    { 3820U, 30U },
    { 3800U, 26U },
    { 3780U, 22U },
    { 3760U, 18U },
    { 3740U, 15U },
    { 3720U, 12U },
    { 3700U, 10U },
    { 3680U, 8U },
    { 3660U, 6U },
    { 3640U, 5U },
    { 3600U, 3U },
    { 3500U, 1U },
    { 3300U, 0U }
};

static const battery_monitor_calibration_t s_default_calibration =
{
    3300U,
    2000U,
    0,
    3300U,
    4200U
};

static battery_monitor_state_t s_battery_state =
{
    0U,
    0U,
    0U,
    0U,
    BATTERY_LEVEL_ALERT
};
static battery_monitor_calibration_t s_battery_calibration =
{
    3300U,
    2000U,
    0,
    3300U,
    4200U
};
static uint32_t s_last_sample_ms = 0U;
static uint8_t s_startup_fast_samples_remaining = 0U;

static uint16_t battery_monitor_sample_mv(void);
static uint16_t battery_monitor_bootstrap_sample_mv(void);
static uint32_t battery_monitor_enter_critical(void);
static void battery_monitor_exit_critical(uint32_t primask);
static battery_monitor_state_t battery_monitor_get_state_snapshot(void);
static uint32_t battery_monitor_sample_interval_ms(void);
static uint16_t battery_monitor_filter_mv(uint16_t current_mv, uint16_t sample_mv, uint8_t charging);
static uint8_t battery_monitor_percent_from_mv(uint16_t mv);
static battery_level_t battery_monitor_level_from_percent(uint8_t percent);
static battery_level_t battery_monitor_apply_hysteresis(uint8_t percent, battery_level_t current_level);
static void battery_monitor_store_state(uint16_t raw_mv, uint16_t filtered_mv, uint8_t charging);

static uint32_t battery_monitor_enter_critical(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    return primask;
}

static void battery_monitor_exit_critical(uint32_t primask)
{
    __set_PRIMASK(primask);
}

static battery_monitor_state_t battery_monitor_get_state_snapshot(void)
{
    battery_monitor_state_t snapshot;
    uint32_t primask = battery_monitor_enter_critical();

    snapshot = s_battery_state;
    battery_monitor_exit_critical(primask);
    return snapshot;
}

static uint16_t battery_monitor_sample_mv(void)
{
    battery_monitor_calibration_t calibration;
    uint32_t adc_average = Get_Adc_Average(BATTERY_ADC_CHANNEL, BATTERY_ADC_SAMPLE_COUNT);
    uint64_t scaled_mv = 0U;
    int32_t adjusted_mv = 0;
    uint32_t primask = battery_monitor_enter_critical();

    calibration = s_battery_calibration;
    battery_monitor_exit_critical(primask);

    scaled_mv = ((uint64_t)adc_average * (uint64_t)calibration.adc_ref_mv * (uint64_t)calibration.divider_scale_milli) / 4095ULL;
    scaled_mv = (scaled_mv + 500ULL) / 1000ULL;
    adjusted_mv = (int32_t)scaled_mv + (int32_t)calibration.voltage_offset_mv;
    if (adjusted_mv <= 0)
    {
        return 0U;
    }

    return (uint16_t)adjusted_mv;
}

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

    /* Drop the highest startup sample to reduce boot-time optimistic spikes. */
    if (valid_count >= 3U)
    {
        sum_mv -= max_mv;
        --valid_count;
    }

    return (uint16_t)((sum_mv + ((uint32_t)valid_count / 2UL)) / (uint32_t)valid_count);
}

static uint32_t battery_monitor_sample_interval_ms(void)
{
    power_state_t power_state = power_manager_get_state();

    if (s_startup_fast_samples_remaining != 0U)
    {
        return BATTERY_STARTUP_FAST_SAMPLE_MS;
    }

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

static uint16_t battery_monitor_filter_mv(uint16_t current_mv, uint16_t sample_mv, uint8_t charging)
{
    uint16_t delta_mv = 0U;

    if (sample_mv == 0U)
    {
        return current_mv;
    }
    if (current_mv == 0U)
    {
        return sample_mv;
    }

    if (sample_mv <= current_mv)
    {
        delta_mv = (uint16_t)(current_mv - sample_mv);

        if (delta_mv >= 220U)
        {
            return sample_mv;
        }
        if (delta_mv >= 100U)
        {
            return (uint16_t)(((uint32_t)current_mv + (uint32_t)sample_mv + 1UL) / 2UL);
        }
        if (delta_mv >= 40U)
        {
            return (uint16_t)(((uint32_t)current_mv * 2UL + (uint32_t)sample_mv + 1UL) / 3UL);
        }

        return (uint16_t)(((uint32_t)current_mv * 3UL + (uint32_t)sample_mv + 2UL) / 4UL);
    }

    delta_mv = (uint16_t)(sample_mv - current_mv);

    if (charging != 0U)
    {
        if (delta_mv >= 220U)
        {
            return (uint16_t)(((uint32_t)current_mv + (uint32_t)sample_mv + 1UL) / 2UL);
        }
        if (delta_mv >= 100U)
        {
            return (uint16_t)(((uint32_t)current_mv * 3UL + (uint32_t)sample_mv + 2UL) / 4UL);
        }

        return (uint16_t)(((uint32_t)current_mv * 5UL + (uint32_t)sample_mv + 3UL) / 6UL);
    }

    if (delta_mv >= 220U)
    {
        return (uint16_t)(((uint32_t)current_mv * 3UL + (uint32_t)sample_mv + 2UL) / 4UL);
    }
    if (delta_mv >= 100U)
    {
        return (uint16_t)(((uint32_t)current_mv * 5UL + (uint32_t)sample_mv + 3UL) / 6UL);
    }
    if (delta_mv >= 40U)
    {
        return (uint16_t)(((uint32_t)current_mv * 6UL + (uint32_t)sample_mv + 3UL) / 7UL);
    }

    return (uint16_t)(((uint32_t)current_mv * 7UL + (uint32_t)sample_mv + 4UL) / 8UL);
}

static uint8_t battery_monitor_percent_from_mv(uint16_t mv)
{
    battery_monitor_calibration_t calibration;
    uint32_t primask = battery_monitor_enter_critical();
    uint32_t index = 0U;

    calibration = s_battery_calibration;
    battery_monitor_exit_critical(primask);

    if (mv <= calibration.percent_empty_mv)
    {
        return 0U;
    }
    if (mv >= calibration.percent_full_mv)
    {
        return 100U;
    }

    for (index = 0U; index + 1U < (sizeof(s_battery_percent_curve) / sizeof(s_battery_percent_curve[0])); ++index)
    {
        const battery_percent_point_t *upper = &s_battery_percent_curve[index];
        const battery_percent_point_t *lower = &s_battery_percent_curve[index + 1U];

        if (mv <= upper->mv && mv >= lower->mv)
        {
            uint32_t span_mv = (uint32_t)(upper->mv - lower->mv);
            uint32_t offset_mv = (uint32_t)(mv - lower->mv);
            uint32_t span_percent = (uint32_t)(upper->percent - lower->percent);
            uint32_t interpolated = ((uint32_t)lower->percent * span_mv) + (offset_mv * span_percent) + (span_mv / 2UL);

            if (span_mv == 0U)
            {
                return upper->percent;
            }

            return (uint8_t)(interpolated / span_mv);
        }
    }

    return 0U;
}

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

static battery_level_t battery_monitor_apply_hysteresis(uint8_t percent, battery_level_t current_level)
{
    switch (current_level)
    {
    case BATTERY_LEVEL_FULL:
        if (percent < (BATTERY_LEVEL_FULL_PERCENT - BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_HIGH;
        }
        return BATTERY_LEVEL_FULL;

    case BATTERY_LEVEL_HIGH:
        if (percent >= (BATTERY_LEVEL_FULL_PERCENT + BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_FULL;
        }
        if (percent < (BATTERY_LEVEL_MEDIUM_PERCENT - BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_MEDIUM;
        }
        return BATTERY_LEVEL_HIGH;

    case BATTERY_LEVEL_MEDIUM:
        if (percent >= (BATTERY_LEVEL_HIGH_PERCENT + BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_HIGH;
        }
        if (percent < (BATTERY_LEVEL_LOW_PERCENT - BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_LOW;
        }
        return BATTERY_LEVEL_MEDIUM;

    case BATTERY_LEVEL_LOW:
        if (percent >= (BATTERY_LEVEL_MEDIUM_PERCENT + BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_MEDIUM;
        }
        if (percent < BATTERY_LEVEL_HYSTERESIS_PERCENT)
        {
            return BATTERY_LEVEL_ALERT;
        }
        return BATTERY_LEVEL_LOW;

    case BATTERY_LEVEL_ALERT:
    default:
        if (percent >= (BATTERY_LEVEL_LOW_PERCENT + BATTERY_LEVEL_HYSTERESIS_PERCENT))
        {
            return BATTERY_LEVEL_LOW;
        }
        return BATTERY_LEVEL_ALERT;
    }
}

static void battery_monitor_store_state(uint16_t raw_mv, uint16_t filtered_mv, uint8_t charging)
{
    battery_monitor_state_t next_state;
    battery_monitor_state_t current_state = battery_monitor_get_state_snapshot();
    uint32_t primask = 0U;

    next_state.raw_mv = raw_mv;
    next_state.mv = filtered_mv;
    next_state.charging = charging;
    next_state.percent = battery_monitor_percent_from_mv(filtered_mv);
    next_state.level = battery_monitor_apply_hysteresis(next_state.percent, current_state.level);
    if (current_state.mv == 0U)
    {
        next_state.level = battery_monitor_level_from_percent(next_state.percent);
    }

    primask = battery_monitor_enter_critical();
    s_battery_state = next_state;
    battery_monitor_exit_critical(primask);
}

void battery_monitor_init(void)
{
    uint16_t sample_mv = 0U;

    Adc_Init();
    sample_mv = battery_monitor_bootstrap_sample_mv();
    battery_monitor_store_state(sample_mv, sample_mv, 0U);
    s_last_sample_ms = power_manager_get_tick_ms();
    s_startup_fast_samples_remaining = BATTERY_STARTUP_FAST_SAMPLE_COUNT;
}

void battery_monitor_step(void)
{
    battery_monitor_state_t current_state = battery_monitor_get_state_snapshot();
    uint32_t now_ms = power_manager_get_tick_ms();
    uint32_t sample_interval_ms = battery_monitor_sample_interval_ms();
    uint16_t sample_mv = 0U;
    uint16_t filtered_mv = 0U;

    if ((now_ms - s_last_sample_ms) < sample_interval_ms)
    {
        return;
    }

    sample_mv = battery_monitor_sample_mv();
    s_last_sample_ms = now_ms;
    filtered_mv = battery_monitor_filter_mv(current_state.mv, sample_mv, current_state.charging);
    battery_monitor_store_state(sample_mv, filtered_mv, current_state.charging);
    if (s_startup_fast_samples_remaining != 0U)
    {
        --s_startup_fast_samples_remaining;
    }
}

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
            next_calibration.percent_full_mv = s_default_calibration.percent_full_mv;
        }
    }

    primask = battery_monitor_enter_critical();
    s_battery_calibration = next_calibration;
    battery_monitor_exit_critical(primask);

    sample_mv = battery_monitor_bootstrap_sample_mv();
    battery_monitor_store_state(sample_mv, sample_mv, state.charging);
    s_last_sample_ms = power_manager_get_tick_ms();
    s_startup_fast_samples_remaining = BATTERY_STARTUP_FAST_SAMPLE_COUNT;
}

battery_monitor_calibration_t battery_monitor_get_calibration(void)
{
    battery_monitor_calibration_t calibration;
    uint32_t primask = battery_monitor_enter_critical();

    calibration = s_battery_calibration;
    battery_monitor_exit_critical(primask);
    return calibration;
}

void battery_monitor_set_charging(uint8_t is_charging)
{
    battery_monitor_state_t state = battery_monitor_get_state_snapshot();

    if (state.charging == ((is_charging != 0U) ? 1U : 0U))
    {
        return;
    }

    battery_monitor_store_state(state.raw_mv, state.mv, (is_charging != 0U) ? 1U : 0U);
}

uint8_t battery_monitor_is_charging(void)
{
    return battery_monitor_get_state_snapshot().charging;
}

uint16_t battery_monitor_get_mv(void)
{
    return battery_monitor_get_state_snapshot().mv;
}

uint8_t battery_monitor_get_percent(void)
{
    return battery_monitor_get_state_snapshot().percent;
}

battery_level_t battery_monitor_get_level(void)
{
    return battery_monitor_get_state_snapshot().level;
}

uint8_t battery_monitor_is_low(void)
{
    return (battery_monitor_get_mv() <= BATTERY_LOW_WARNING_MV) ? 1U : 0U;
}
