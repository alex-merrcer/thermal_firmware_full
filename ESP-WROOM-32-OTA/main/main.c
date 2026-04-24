#include "RTOS_DEMO.h"
#include "esp_task_wdt.h"
#include "OTA_STM32.h"

esp_err_t ret;

void app_main()
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    lcdInit();
    key_init();
    UART0_Init();
    UART2_Init();
    LCD_Fill(0, 0, 160, 128, BLACK);
    LCD_PanelSleep();

    OTA_STM32_Init();
    OTA_STM32_Start();
}
