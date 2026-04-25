#include "board_drivers.h"

#include "KEY.h"
#include "UART.h"
#include "lib_lcd7735.h"

void board_drivers_init(void)
{
    lcdInit();
    key_init();
    UART0_Init();
    UART2_Init();
    LCD_Fill(0, 0, 160, 128, BLACK);
    LCD_PanelSleep();
}
