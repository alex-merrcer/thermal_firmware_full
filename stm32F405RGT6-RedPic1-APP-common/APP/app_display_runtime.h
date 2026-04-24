#ifndef APP_DISPLAY_RUNTIME_H
#define APP_DISPLAY_RUNTIME_H

#include <stdint.h>

#ifndef APP_DISPLAY_STAGE3_ENABLE
    #define APP_DISPLAY_STAGE3_ENABLE 1
#endif

typedef void (*app_display_ui_render_fn_t)(uint8_t full_refresh);
typedef enum
{
    APP_DISPLAY_THERMAL_DONE_OK = 0,
    APP_DISPLAY_THERMAL_DONE_CANCELLED,
    APP_DISPLAY_THERMAL_DONE_ERROR
} app_display_thermal_done_status_t;
typedef void (*app_display_thermal_done_fn_t)(uintptr_t token,
                                              app_display_thermal_done_status_t status);
typedef uint8_t (*app_display_thermal_claim_fn_t)(uintptr_t token, uint8_t **gray_frame);

uint8_t app_display_runtime_init(void);
uint8_t app_display_runtime_start(void);
void app_display_runtime_task(void *pvParameters);
void app_display_runtime_lock(void);
void app_display_runtime_unlock(void);
void app_display_runtime_set_thermal_present_done_callback(app_display_thermal_done_fn_t done_fn);
void app_display_runtime_set_thermal_present_claim_callback(app_display_thermal_claim_fn_t claim_fn);
uint8_t app_display_runtime_sleep(void);
uint8_t app_display_runtime_wake(void);
uint8_t app_display_runtime_present_thermal_frame(uint8_t *gray_frame);
uint8_t app_display_runtime_request_thermal_present_async(uint8_t *gray_frame, uintptr_t token);
void app_display_runtime_cancel_thermal_present_async(void);
uint8_t app_display_runtime_request_ui_render(app_display_ui_render_fn_t render_fn,
                                              uint8_t full_refresh);
uint8_t app_display_runtime_is_awake(void);

#endif
