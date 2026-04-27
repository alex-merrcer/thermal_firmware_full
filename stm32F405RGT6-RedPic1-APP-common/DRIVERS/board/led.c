#include "led.h" 


/* 初始化开发板上的状态 LED。
 * 这个工程主功能不是靠 LED 工作，所以这里只保留最基础的输出配置。 */
void LED_Init(void)
{    	 
  GPIO_InitTypeDef  GPIO_InitStructure;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);//使能GPIOF时钟

  //GPIOF9,F10初始化设置
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_11;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;//普通输出模式
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;//推挽输出
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;//低速以降低功耗
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;//无上拉（LED不需要）
  GPIO_Init(GPIOA, &GPIO_InitStructure);//初始化
	
	GPIO_SetBits(GPIOA,GPIO_Pin_11);//GPIOF9,F10设置高，灯灭

}





