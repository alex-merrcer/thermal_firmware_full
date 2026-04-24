#include "sys.h"  



/* 执行 WFI 指令，让内核进入等待中断状态。 */
__asm void WFI_SET(void)
{
	WFI;		  
}
/* 关闭普通可屏蔽中断，但不影响 Fault 和 NMI。 */
__asm void INTX_DISABLE(void)
{
	CPSID   I
	BX      LR	  
}
/* 重新开启普通可屏蔽中断。 */
__asm void INTX_ENABLE(void)
{
	CPSIE   I
	BX      LR  
}
/* 直接用汇编修改主栈指针 MSP。 */
__asm void MSR_MSP(u32 addr) 
{
	MSR MSP, r0 			//set Main Stack value
	BX r14
}
/* C 版本的 MSP 设置接口，便于从普通 C 代码中安全调用。 */
void sys_msr_msp(uint32_t addr)
{
    __set_MSP(addr);    /* 设置栈顶地址 */
}

















