#ifndef OTA_STM32_TRANSFER_H
#define OTA_STM32_TRANSFER_H

#include "ota_stm32_internal.h"

void build_transfer_file_name(const char *original_name, char *buffer, size_t buffer_len);
void ota_log_stm32_boot_report_line(const char *line, ota_stm32_boot_report_t *report);
ota_stm32_boot_report_t ota_read_stm32_boot_report(void);
ota_stm32_boot_report_t ota_read_stm32_boot_report_with_timeouts(uint32_t total_timeout_ms,
                                                                 uint32_t idle_timeout_ms);
bool ymodem_send_encrypted_stream(const ota_iap_context_t *context,
                                  const char *transfer_file_name,
                                  size_t start_transfer_offset);

#endif
