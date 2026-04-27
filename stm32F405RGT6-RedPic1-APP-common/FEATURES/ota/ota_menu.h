#ifndef OTA_MENU_H
#define OTA_MENU_H

#include <stdint.h>

void ota_menu_init(void);
void ota_menu_enter(void);
void ota_menu_exit(void);
uint8_t ota_menu_is_active(void);
void ota_menu_handle_key(uint8_t key_value);

#endif
