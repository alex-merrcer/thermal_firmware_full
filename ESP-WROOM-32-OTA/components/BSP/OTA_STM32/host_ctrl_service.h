#ifndef HOST_CTRL_SERVICE_H
#define HOST_CTRL_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "ota_stm32_internal.h"

void host_ctrl_service_init(void);
void host_ctrl_service_step(void);
bool host_ctrl_service_handle_frame(const ota_ctrl_frame_t *frame);
void host_ctrl_service_request_wifi(uint8_t enabled);
void host_ctrl_service_request_runtime_apply(void);

#endif
