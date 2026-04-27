#include "delay.h"
#include "sys.h"
#include "led.h"
#include "lcd_init.h"
#include "lcd.h"
#include "key.h"
#include "usart.h"
#include "aes.h"
#include "flash_if.h"
#include "iap.h"
#include "misc.h"
#include "stm32f4xx_usart.h"
#include "uart_rx_ring.h"

int main(void)
{
    /* 系统初始化 */
    FLASH_If_Init();
    delay_init(168);
    SystemInit();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    uart_init(115200);
    uart_rx_ring_reset();
    KEY_Init();

    /* Bootloader启动入口 */
    iap_boot_entry();
    
    /* 正常运行不会到达这里(跳转后不返回)
       只有当两个分区都无效时才会执行到这里 */
    while (1)
    {
    }
      
}
