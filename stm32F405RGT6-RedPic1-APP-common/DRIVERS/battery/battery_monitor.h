#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <stdint.h>

typedef enum
{
    BATTERY_LEVEL_FULL = 0,
    BATTERY_LEVEL_HIGH,
    BATTERY_LEVEL_MEDIUM,
    BATTERY_LEVEL_LOW,
    BATTERY_LEVEL_ALERT
} battery_level_t;

typedef struct
{
    uint16_t adc_ref_mv;
    uint16_t divider_scale_milli;
    int16_t voltage_offset_mv;
    uint16_t percent_empty_mv;
    uint16_t percent_full_mv;
} battery_monitor_calibration_t;

void battery_monitor_init(void);
void battery_monitor_step(void);
void battery_monitor_set_calibration(const battery_monitor_calibration_t *calibration);
battery_monitor_calibration_t battery_monitor_get_calibration(void);
void battery_monitor_set_charging(uint8_t is_charging);
uint8_t battery_monitor_is_charging(void);
uint16_t battery_monitor_get_mv(void);
uint8_t battery_monitor_get_percent(void);
battery_level_t battery_monitor_get_level(void);
uint8_t battery_monitor_is_low(void);

#endif
