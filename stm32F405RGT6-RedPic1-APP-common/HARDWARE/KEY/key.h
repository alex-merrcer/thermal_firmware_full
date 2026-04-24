#ifndef __KEY_H__
#define __KEY_H__

#include "sys.h"

#define KEY1  GPIO_ReadInputDataBit(GPIOC,GPIO_Pin_13)//��ȡ����0
#define KEY2  GPIO_ReadInputDataBit(GPIOB,GPIO_Pin_8)//��ȡ����1
#define KEY3 	GPIO_ReadInputDataBit(GPIOB,GPIO_Pin_9)//��ȡ����3(WK_UP) 


#define KEY1_PRES 	1	//KEY0����
#define KEY2_PRES	  2	//KEY1����
#define KEY3_PRES   3	//KEY_UP����(��WK_UP/KEY_UP)

void KEY_Init(void);
u8 KEY_Scan(u8 mode);
void KEY_TEST(void);
#endif
