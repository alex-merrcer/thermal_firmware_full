#include "sdio_sdcard.h"
#include "string.h"
#include "sys.h"
#include "usart.h"
//////////////////////////////////////////////////////////////////////////////////

// STM32F407 SDIO驱动
// SDIO 驱动代码

//////////////////////////////////////////////////////////////////////////////////

/* SDIO driver runtime structures */
SDIO_InitTypeDef SDIO_InitStructure;
SDIO_CmdInitTypeDef SDIO_CmdInitStructure;
SDIO_DataInitTypeDef SDIO_DataInitStructure;

SD_Error CmdError(void);
SD_Error CmdResp7Error(void);
SD_Error CmdResp1Error(u8 cmd);
SD_Error CmdResp3Error(void);
SD_Error CmdResp2Error(void);
SD_Error CmdResp6Error(u8 cmd,u16*prca);
SD_Error SDEnWideBus(u8 enx);
SD_Error IsCardProgramming(u8 *pstatus);
SD_Error FindSCR(u16 rca,u32 *pscr);
u8 convert_from_bytes_to_power_of_two(u16 NumberOfBytes);


static u8 CardType=SDIO_STD_CAPACITY_SD_CARD_V1_1;		// SD卡类型（V1.1/V2.0）
static u32 CSD_Tab[4],CID_Tab[4],RCA=0;					// SD卡CSD、CID寄存器数据及相对地址(RCA)
static u8 DeviceMode=SD_DMA_MODE;							// 工作模式，通过SD_SetDeviceMode设置，默认DMA模式(SD_DMA_MODE)
static u8 StopCondition=0; 								// 多块传输停止标志，DMA模式下由中断处理
volatile SD_Error TransferError=SD_OK;						// 数据传输错误标志，DMA中断设置
volatile u8 TransferEnd=0;									// 数据传输完成标志，DMA中断设置
SD_CardInfo SDCardInfo;										// SD卡信息

// SD_ReadDisk/SD_WriteDisk需要4字节对齐的buf，若非对齐则使用临时缓冲区
// 当使用这两个函数时请确保传入的buf已4字节对齐，否则会使用额外的内存
#define SD_DMA_STREAM               DMA2_Stream6
#define SD_DMA_CHANNEL              DMA_Channel_4
#define SD_DMA_TC_FLAG              DMA_FLAG_TCIF6
#define SD_DMA_ALL_FLAGS            (DMA_FLAG_FEIF6  | DMA_FLAG_DMEIF6 | \
                                     DMA_FLAG_TEIF6  | DMA_FLAG_HTIF6  | \
                                     DMA_FLAG_TCIF6)

__align(4) u8 SDIO_DATA_BUFFER[512];


// SDIO寄存器复位
void SDIO_Register_Deinit()
{
	SDIO->POWER=0x00000000;
	SDIO->CLKCR=0x00000000;
	SDIO->ARG=0x00000000;
	SDIO->CMD=0x00000000;
	SDIO->DTIMER=0x00000000;
	SDIO->DLEN=0x00000000;
	SDIO->DCTRL=0x00000000;
	SDIO->ICR=0x00C007FF;
	SDIO->MASK=0x00000000;
}

// 初始化SD卡
// 返回值: 错误代码; (0, 无错误)
SD_Error SD_Init(void)
{
 	GPIO_InitTypeDef  GPIO_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;

	SD_Error errorstatus=SD_OK;
  u8 clkdiv=0;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC|RCC_AHB1Periph_GPIOD|RCC_AHB1Periph_DMA2, ENABLE);// 使能GPIOC,GPIOD,DMA2时钟

  RCC_APB2PeriphClockCmd(RCC_APB2Periph_SDIO, ENABLE);// SDIO时钟使能

	RCC_APB2PeriphResetCmd(RCC_APB2Periph_SDIO, ENABLE);// SDIO复位


  GPIO_InitStructure.GPIO_Pin =GPIO_Pin_8|GPIO_Pin_9|GPIO_Pin_10|GPIO_Pin_11|GPIO_Pin_12; 	// PC8~PC12引脚复用功能
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;// 复用功能
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;// 100M
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;// 上拉
  GPIO_Init(GPIOC, &GPIO_InitStructure);// PC8~PC12引脚复用功能


	GPIO_InitStructure.GPIO_Pin =GPIO_Pin_2;
  GPIO_Init(GPIOD, &GPIO_InitStructure);// PD2引脚复用功能

	 // 配置引脚复用功能映射
	GPIO_PinAFConfig(GPIOC,GPIO_PinSource8,GPIO_AF_SDIO); // PC8,AF12
  GPIO_PinAFConfig(GPIOC,GPIO_PinSource9,GPIO_AF_SDIO);
  GPIO_PinAFConfig(GPIOC,GPIO_PinSource10,GPIO_AF_SDIO);
  GPIO_PinAFConfig(GPIOC,GPIO_PinSource11,GPIO_AF_SDIO);
  GPIO_PinAFConfig(GPIOC,GPIO_PinSource12,GPIO_AF_SDIO);
  GPIO_PinAFConfig(GPIOD,GPIO_PinSource2,GPIO_AF_SDIO);

	RCC_APB2PeriphResetCmd(RCC_APB2Periph_SDIO, DISABLE);// SDIO结束复位

 	// SDIO寄存器默认值初始化
	SDIO_Register_Deinit();

  NVIC_InitStructure.NVIC_IRQChannel = SDIO_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority=0;// 抢占优先级0
	NVIC_InitStructure.NVIC_IRQChannelSubPriority =0;		// 子优先级0
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;			// IRQ通道使能
	NVIC_Init(&NVIC_InitStructure);	// 配置NVIC的SDIO中断

   	errorstatus=SD_PowerON();			// SD卡上电
 	if(errorstatus==SD_OK)errorstatus=SD_InitializeCards();			// 初始化SD卡
  	if(errorstatus==SD_OK)errorstatus=SD_GetCardInfo(&SDCardInfo);	// 获取卡信息
 	if(errorstatus==SD_OK)errorstatus=SD_SelectDeselect((u32)(SDCardInfo.RCA<<16));// 选中SD卡
   	#if (SDIO_DEBUG_FORCE_1BIT == 0)
   	if(errorstatus==SD_OK)errorstatus=SD_EnableWideBusOperation(SDIO_BusWide_4b);
#endif	// 4位宽度，不支持MMC卡；如不支持4位宽度则用1位宽度
  	if((errorstatus==SD_OK)||(SDIO_MULTIMEDIA_CARD==CardType))
	{
		if(SDCardInfo.CardType==SDIO_STD_CAPACITY_SD_CARD_V1_1||SDCardInfo.CardType==SDIO_STD_CAPACITY_SD_CARD_V2_0)
		{
			clkdiv=SDIO_TRANSFER_CLK_DIV+2;	// V1.1/V2.0卡设置时钟为72/4=12Mhz
		}else clkdiv=SDIO_TRANSFER_CLK_DIV;	// SDHC卡设置时钟为72/2=24Mhz
		SDIO_Clock_Set(clkdiv);	// 设置SDIO时钟频率，SDIO_CK时钟=SDIOCLK/[clkdiv+2]; 其中SDIOCLK固定为48Mhz
		//errorstatus=SD_SetDeviceMode(SD_DMA_MODE);	// 设置为DMA模式
		errorstatus=SD_SetDeviceMode(SD_POLLING_MODE);// 设置为查询模式
 	}
	return errorstatus;
}
// SDIO时钟频率设置
// clkdiv: 时钟分频系数
// CK时钟=SDIOCLK/[clkdiv+2];(SDIOCLK时钟固定为48Mhz)
void SDIO_Clock_Set(u8 clkdiv)
{
	u32 tmpreg=SDIO->CLKCR;
  	tmpreg&=0XFFFFFF00;
 	tmpreg|=clkdiv;
	SDIO->CLKCR=tmpreg;
}


// SD卡上电
// 查询所有SDIO接口上的SD卡设备，并为其供电
// 返回值: 错误代码; (0, 无错误)
SD_Error SD_PowerON(void)
{
 	u8 i=0;
	SD_Error errorstatus=SD_OK;
	u32 response=0,count=0,validvoltage=0;
	u32 SDType=SD_STD_CAPACITY;

	 // 初始化时的时钟不能超过400KHz
  SDIO_InitStructure.SDIO_ClockDiv = SDIO_INIT_CLK_DIV;	/* HCLK = 72MHz, SDIOCLK = 72MHz, SDIO_CK = HCLK/(178 + 2) = 400 KHz */
  SDIO_InitStructure.SDIO_ClockEdge = SDIO_ClockEdge_Rising;
  SDIO_InitStructure.SDIO_ClockBypass = SDIO_ClockBypass_Disable;  // 不使用旁路模式，直接由HCLK分频得到SDIO_CK
  SDIO_InitStructure.SDIO_ClockPowerSave = SDIO_ClockPowerSave_Disable;	// 空闲时时钟不关闭
  SDIO_InitStructure.SDIO_BusWide = SDIO_BusWide_1b;	 				// 1位数据宽度
  SDIO_InitStructure.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Disable;// 不使用硬件流控制
  SDIO_Init(&SDIO_InitStructure);

	SDIO_SetPowerState(SDIO_PowerState_ON);	// 上电状态，开启卡时钟
  SDIO->CLKCR|=1<<8;			// SDIOCK使能

 	for(i=0;i<74;i++)
	{

		SDIO_CmdInitStructure.SDIO_Argument = 0x0;// 发送CMD0进入IDLE STAGE模式
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_GO_IDLE_STATE; //cmd0
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_No;  // 无响应
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;  // 使能CPSM，开始发送命令
    SDIO_SendCommand(&SDIO_CmdInitStructure);	  		// 写命令寄存器，开始发送

		errorstatus=CmdError();

		if(errorstatus==SD_OK)break;
 	}
	if(errorstatus)return errorstatus;// 返回错误响应

  SDIO_CmdInitStructure.SDIO_Argument = SD_CHECK_PATTERN;	// 发送CMD8，短响应，检查SD卡接口是否可用
  SDIO_CmdInitStructure.SDIO_CmdIndex = SDIO_SEND_IF_COND;	//cmd8
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;	 //r7
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;			 // 关闭等待中断
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);

  errorstatus=CmdResp7Error();						// 检查R7响应

 	if(errorstatus==SD_OK) 								// R7响应正常
	{
		CardType=SDIO_STD_CAPACITY_SD_CARD_V2_0;		// SD 2.0卡
		SDType=SD_HIGH_CAPACITY;			   			// 高容量卡
	}

	  SDIO_CmdInitStructure.SDIO_Argument = 0x00;// 发送CMD55，短响应
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);		// 发送CMD55，短响应

	 errorstatus=CmdResp1Error(SD_CMD_APP_CMD); 		 	// 检查R1响应

	if(errorstatus==SD_OK)// SD2.0/SD 1.1,以及MMC卡
	{
		// SD卡发送ACMD41 SD_APP_OP_COND,参数为0x80100000
		while((!validvoltage)&&(count<SD_MAX_VOLT_TRIAL))
		{
		  SDIO_CmdInitStructure.SDIO_Argument = 0x00;// 发送CMD55，短响应
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;	  //CMD55
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);			// 发送CMD55，短响应

			errorstatus=CmdResp1Error(SD_CMD_APP_CMD); 	 	// 检查R1响应

 			if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误

      // 发送ACMD41命令，参数中的HCS位用来告诉SD卡是否支持HC卡
      SDIO_CmdInitStructure.SDIO_Argument = SD_VOLTAGE_WINDOW_SD | SDType;	// 发送ACMD41，短响应
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SD_APP_OP_COND;
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;  //r3
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);

			errorstatus=CmdResp3Error(); 					// 检查R3响应

 			if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误
			response=SDIO->RESP1;;			   				// 得到响应
			validvoltage=(((response>>31)==1)?1:0);			// 判断SD卡上电是否完成
			count++;
		}
		if(count>=SD_MAX_VOLT_TRIAL)
		{
			errorstatus=SD_INVALID_VOLTRANGE;
			return errorstatus;
		}
		if(response&=SD_HIGH_CAPACITY)
		{
			CardType=SDIO_HIGH_CAPACITY_SD_CARD;
		}
 	}else// MMC卡
	{
		// MMC卡发送CMD1 SDIO_SEND_OP_COND,参数为0x80FF8000
		while((!validvoltage)&&(count<SD_MAX_VOLT_TRIAL))
		{
			SDIO_CmdInitStructure.SDIO_Argument = SD_VOLTAGE_WINDOW_MMC;// 发送CMD1，短响应
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_OP_COND;
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;  //r3
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);

			errorstatus=CmdResp3Error(); 					// 检查R3响应

 			if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误
			response=SDIO->RESP1;;			   				// 得到响应
			validvoltage=(((response>>31)==1)?1:0);
			count++;
		}
		if(count>=SD_MAX_VOLT_TRIAL)
		{
			errorstatus=SD_INVALID_VOLTRANGE;
			return errorstatus;
		}
		CardType=SDIO_MULTIMEDIA_CARD;
  	}
  	return(errorstatus);
}
// SD卡Power OFF
// 返回值: 错误代码; (0, 无错误)
SD_Error SD_PowerOFF(void)
{

  SDIO_SetPowerState(SDIO_PowerState_OFF);// SDIO电源关闭，时钟停止

  return SD_OK;
}

// SD卡进入低功耗模式
SD_Error SD_EnterLowPowerMode(void)
{
  SD_Error errorstatus = SD_OK;

  if ((RCC->APB2ENR & RCC_APB2Periph_SDIO) == 0U)
  {
    return SD_OK;
  }

  SDIO->MASK  = 0U;
  SDIO->DCTRL = 0U;
  SDIO->ICR   = SDIO_STATIC_FLAGS;

  if (SDIO_GetPowerState() != SDIO_PowerState_OFF && SDCardInfo.RCA != 0U)
  {
    errorstatus = SD_SelectDeselect(0U);
  }

  SDIO->CMD = 0U;
  SDIO->ARG = 0U;
  SDIO->CLKCR &= ~(1U << 8);
  SDIO_SetPowerState(SDIO_PowerState_OFF);

  return errorstatus;
}
// 初始化所有的卡，并进入就绪状态
// 返回值: 错误代码
SD_Error SD_InitializeCards(void)
{
 	SD_Error errorstatus=SD_OK;
	u16 rca = 0x01;

  if (SDIO_GetPowerState() == SDIO_PowerState_OFF)	// 检查电源状态，确保已上电
  {
    errorstatus = SD_REQUEST_NOT_APPLICABLE;
    return(errorstatus);
  }

 	if(SDIO_SECURE_DIGITAL_IO_CARD!=CardType)			// 非SECURE_DIGITAL_IO_CARD
	{
		SDIO_CmdInitStructure.SDIO_Argument = 0x0;// 发送CMD2，长响应，获取CID
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_ALL_SEND_CID;
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Long;
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);// 发送CMD2，长响应，获取CID

		errorstatus=CmdResp2Error(); 					// 检查R2响应

		if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误

 		CID_Tab[0]=SDIO->RESP1;
		CID_Tab[1]=SDIO->RESP2;
		CID_Tab[2]=SDIO->RESP3;
		CID_Tab[3]=SDIO->RESP4;
	}
	if((SDIO_STD_CAPACITY_SD_CARD_V1_1==CardType)||(SDIO_STD_CAPACITY_SD_CARD_V2_0==CardType)||(SDIO_SECURE_DIGITAL_IO_COMBO_CARD==CardType)||(SDIO_HIGH_CAPACITY_SD_CARD==CardType))// SD卡
	{
		SDIO_CmdInitStructure.SDIO_Argument = 0x00;// 发送CMD3，短响应
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_REL_ADDR;	//cmd3
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short; //r6
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);	// 发送CMD3，短响应

		errorstatus=CmdResp6Error(SD_CMD_SET_REL_ADDR,&rca);// 检查R6响应

		if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误
	}
    if (SDIO_MULTIMEDIA_CARD==CardType)
    {

		  SDIO_CmdInitStructure.SDIO_Argument = (u32)(rca<<16);// 发送CMD3，短响应
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_REL_ADDR;	//cmd3
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short; //r6
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);	// 发送CMD3，短响应

      errorstatus=CmdResp2Error(); 					// 检查R2响应

		  if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误
    }
	if (SDIO_SECURE_DIGITAL_IO_CARD!=CardType)			// 非SECURE_DIGITAL_IO_CARD
	{
		RCA = rca;

    SDIO_CmdInitStructure.SDIO_Argument = (uint32_t)(rca << 16);// 发送CMD9+卡RCA，长响应，获取CSD
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_CSD;
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Long;
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);

		errorstatus=CmdResp2Error(); 					// 检查R2响应
		if(errorstatus!=SD_OK)return errorstatus;   	// 响证错误

		CSD_Tab[0]=SDIO->RESP1;
	  CSD_Tab[1]=SDIO->RESP2;
		CSD_Tab[2]=SDIO->RESP3;
		CSD_Tab[3]=SDIO->RESP4;
	}
	return SD_OK;// 初始化成功
}
// 获取卡信息
// cardinfo: 卡信息存储区
// 返回值: 错误代码
SD_Error SD_GetCardInfo(SD_CardInfo *cardinfo)
{
 	SD_Error errorstatus=SD_OK;
	u8 tmp=0;
	cardinfo->CardType=(u8)CardType; 				// 卡类型
	cardinfo->RCA=(u16)RCA;							// 卡RCA
	tmp=(u8)((CSD_Tab[0]&0xFF000000)>>24);
	cardinfo->SD_csd.CSDStruct=(tmp&0xC0)>>6;		// CSD结构
	cardinfo->SD_csd.SysSpecVersion=(tmp&0x3C)>>2;	// 2.0协议版本号，兼容1.0
	cardinfo->SD_csd.Reserved1=tmp&0x03;			// 2位保留
	tmp=(u8)((CSD_Tab[0]&0x00FF0000)>>16);			// 第1个字节
	cardinfo->SD_csd.TAAC=tmp;				   		// 数据读取时间1
	tmp=(u8)((CSD_Tab[0]&0x0000FF00)>>8);	  		// 第2个字节
	cardinfo->SD_csd.NSAC=tmp;		  				// 数据读取时间2
	tmp=(u8)(CSD_Tab[0]&0x000000FF);				// 第3个字节
	cardinfo->SD_csd.MaxBusClkFrec=tmp;		  		// 传输速度
	tmp=(u8)((CSD_Tab[1]&0xFF000000)>>24);			// 第4个字节
	cardinfo->SD_csd.CardComdClasses=tmp<<4;    	// 卡指令类高4位
	tmp=(u8)((CSD_Tab[1]&0x00FF0000)>>16);	 		// 第5个字节
	cardinfo->SD_csd.CardComdClasses|=(tmp&0xF0)>>4;// 卡指令类低4位
	cardinfo->SD_csd.RdBlockLen=tmp&0x0F;	    	// 最大读取数据块长度
	tmp=(u8)((CSD_Tab[1]&0x0000FF00)>>8);			// 第6个字节
	cardinfo->SD_csd.PartBlockRead=(tmp&0x80)>>7;	// 允许部分块读取
	cardinfo->SD_csd.WrBlockMisalign=(tmp&0x40)>>6;	// 写块不对齐
	cardinfo->SD_csd.RdBlockMisalign=(tmp&0x20)>>5;	// 读块不对齐
	cardinfo->SD_csd.DSRImpl=(tmp&0x10)>>4;
	cardinfo->SD_csd.Reserved2=0; 					// 保留
 	if((CardType==SDIO_STD_CAPACITY_SD_CARD_V1_1)||(CardType==SDIO_STD_CAPACITY_SD_CARD_V2_0)||(SDIO_MULTIMEDIA_CARD==CardType))// 标准1.1/2.0卡/MMC卡
	{
		cardinfo->SD_csd.DeviceSize=(tmp&0x03)<<10;	// C_SIZE(12位)
	 	tmp=(u8)(CSD_Tab[1]&0x000000FF); 			// 第7个字节
		cardinfo->SD_csd.DeviceSize|=(tmp)<<2;
 		tmp=(u8)((CSD_Tab[2]&0xFF000000)>>24);		// 第8个字节
		cardinfo->SD_csd.DeviceSize|=(tmp&0xC0)>>6;
 		cardinfo->SD_csd.MaxRdCurrentVDDMin=(tmp&0x38)>>3;
		cardinfo->SD_csd.MaxRdCurrentVDDMax=(tmp&0x07);
 		tmp=(u8)((CSD_Tab[2]&0x00FF0000)>>16);		// 第9个字节
		cardinfo->SD_csd.MaxWrCurrentVDDMin=(tmp&0xE0)>>5;
		cardinfo->SD_csd.MaxWrCurrentVDDMax=(tmp&0x1C)>>2;
		cardinfo->SD_csd.DeviceSizeMul=(tmp&0x03)<<1;// C_SIZE_MULT
 		tmp=(u8)((CSD_Tab[2]&0x0000FF00)>>8);	  	// 第10个字节
		cardinfo->SD_csd.DeviceSizeMul|=(tmp&0x80)>>7;
 		cardinfo->CardCapacity=(cardinfo->SD_csd.DeviceSize+1);// 计算卡容量
		cardinfo->CardCapacity*=(1<<(cardinfo->SD_csd.DeviceSizeMul+2));
		cardinfo->CardBlockSize=1<<(cardinfo->SD_csd.RdBlockLen);// 块大小
		cardinfo->CardCapacity*=cardinfo->CardBlockSize;
	}else if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)	// 高容量卡
	{
 		tmp=(u8)(CSD_Tab[1]&0x000000FF); 		// 第7个字节
		cardinfo->SD_csd.DeviceSize=(tmp&0x3F)<<16;// C_SIZE
 		tmp=(u8)((CSD_Tab[2]&0xFF000000)>>24); 	// 第8个字节
 		cardinfo->SD_csd.DeviceSize|=(tmp<<8);
 		tmp=(u8)((CSD_Tab[2]&0x00FF0000)>>16);	// 第9个字节
 		cardinfo->SD_csd.DeviceSize|=(tmp);
 		tmp=(u8)((CSD_Tab[2]&0x0000FF00)>>8); 	// 第10个字节
 		cardinfo->CardCapacity=(long long)(cardinfo->SD_csd.DeviceSize+1)*512*1024;// 计算卡容量
		cardinfo->CardBlockSize=512; 			// 高容量卡块大小固定512字节
	}
	cardinfo->SD_csd.EraseGrSize=(tmp&0x40)>>6;
	cardinfo->SD_csd.EraseGrMul=(tmp&0x3F)<<1;
	tmp=(u8)(CSD_Tab[2]&0x000000FF);			// 第11个字节
	cardinfo->SD_csd.EraseGrMul|=(tmp&0x80)>>7;
	cardinfo->SD_csd.WrProtectGrSize=(tmp&0x7F);
 	tmp=(u8)((CSD_Tab[3]&0xFF000000)>>24);		// 第12个字节
	cardinfo->SD_csd.WrProtectGrEnable=(tmp&0x80)>>7;
	cardinfo->SD_csd.ManDeflECC=(tmp&0x60)>>5;
	cardinfo->SD_csd.WrSpeedFact=(tmp&0x1C)>>2;
	cardinfo->SD_csd.MaxWrBlockLen=(tmp&0x03)<<2;
	tmp=(u8)((CSD_Tab[3]&0x00FF0000)>>16);		// 第13个字节
	cardinfo->SD_csd.MaxWrBlockLen|=(tmp&0xC0)>>6;
	cardinfo->SD_csd.WriteBlockPaPartial=(tmp&0x20)>>5;
	cardinfo->SD_csd.Reserved3=0;
	cardinfo->SD_csd.ContentProtectAppli=(tmp&0x01);
	tmp=(u8)((CSD_Tab[3]&0x0000FF00)>>8);		// 第14个字节
	cardinfo->SD_csd.FileFormatGrouop=(tmp&0x80)>>7;
	cardinfo->SD_csd.CopyFlag=(tmp&0x40)>>6;
	cardinfo->SD_csd.PermWrProtect=(tmp&0x20)>>5;
	cardinfo->SD_csd.TempWrProtect=(tmp&0x10)>>4;
	cardinfo->SD_csd.FileFormat=(tmp&0x0C)>>2;
	cardinfo->SD_csd.ECC=(tmp&0x03);
	tmp=(u8)(CSD_Tab[3]&0x000000FF);			// 第15个字节
	cardinfo->SD_csd.CSD_CRC=(tmp&0xFE)>>1;
	cardinfo->SD_csd.Reserved4=1;
	tmp=(u8)((CID_Tab[0]&0xFF000000)>>24);		// 第0个字节
	cardinfo->SD_cid.ManufacturerID=tmp;
	tmp=(u8)((CID_Tab[0]&0x00FF0000)>>16);		// 第1个字节
	cardinfo->SD_cid.OEM_AppliID=tmp<<8;
	tmp=(u8)((CID_Tab[0]&0x000000FF00)>>8);		// 第2个字节
	cardinfo->SD_cid.OEM_AppliID|=tmp;
	tmp=(u8)(CID_Tab[0]&0x000000FF);			// 第3个字节
	cardinfo->SD_cid.ProdName1=tmp<<24;
	tmp=(u8)((CID_Tab[1]&0xFF000000)>>24); 		// 第4个字节
	cardinfo->SD_cid.ProdName1|=tmp<<16;
	tmp=(u8)((CID_Tab[1]&0x00FF0000)>>16);	   	// 第5个字节
	cardinfo->SD_cid.ProdName1|=tmp<<8;
	tmp=(u8)((CID_Tab[1]&0x0000FF00)>>8);		// 第6个字节
	cardinfo->SD_cid.ProdName1|=tmp;
	tmp=(u8)(CID_Tab[1]&0x000000FF);	  		// 第7个字节
	cardinfo->SD_cid.ProdName2=tmp;
	tmp=(u8)((CID_Tab[2]&0xFF000000)>>24); 		// 第8个字节
	cardinfo->SD_cid.ProdRev=tmp;
	tmp=(u8)((CID_Tab[2]&0x00FF0000)>>16);		// 第9个字节
	cardinfo->SD_cid.ProdSN=tmp<<24;
	tmp=(u8)((CID_Tab[2]&0x0000FF00)>>8); 		// 第10个字节
	cardinfo->SD_cid.ProdSN|=tmp<<16;
	tmp=(u8)(CID_Tab[2]&0x000000FF);   			// 第11个字节
	cardinfo->SD_cid.ProdSN|=tmp<<8;
	tmp=(u8)((CID_Tab[3]&0xFF000000)>>24); 		// 第12个字节
	cardinfo->SD_cid.ProdSN|=tmp;
	tmp=(u8)((CID_Tab[3]&0x00FF0000)>>16);	 	// 第13个字节
	cardinfo->SD_cid.Reserved1|=(tmp&0xF0)>>4;
	cardinfo->SD_cid.ManufactDate=(tmp&0x0F)<<8;
	tmp=(u8)((CID_Tab[3]&0x0000FF00)>>8);		// 第14个字节
	cardinfo->SD_cid.ManufactDate|=tmp;
	tmp=(u8)(CID_Tab[3]&0x000000FF);			// 第15个字节
	cardinfo->SD_cid.CID_CRC=(tmp&0xFE)>>1;
	cardinfo->SD_cid.Reserved2=1;
	return errorstatus;
}
// 设置SDIO总线宽度（MMC卡不支持4bit模式）
// wmode: 位宽模式。0,1位数据宽度; 1,4位数据宽度; 2,8位数据宽度
// 返回值: SD卡错误码

// 设置SDIO总线宽度（MMC卡不支持4bit模式）
//   @arg SDIO_BusWide_8b: 8-bit data transfer (Only for MMC)
//   @arg SDIO_BusWide_4b: 4-bit data transfer
//   @arg SDIO_BusWide_1b: 1-bit data transfer (默认)
// 返回值: SD卡错误码


SD_Error SD_EnableWideBusOperation(u32 WideMode)
{
  	SD_Error errorstatus=SD_OK;
  if (SDIO_MULTIMEDIA_CARD == CardType)
  {
    errorstatus = SD_UNSUPPORTED_FEATURE;
    return(errorstatus);
  }

 	else if((SDIO_STD_CAPACITY_SD_CARD_V1_1==CardType)||(SDIO_STD_CAPACITY_SD_CARD_V2_0==CardType)||(SDIO_HIGH_CAPACITY_SD_CARD==CardType))
	{
		 if (SDIO_BusWide_8b == WideMode)   // 2.0 sd卡不支持8位
    {
      errorstatus = SD_UNSUPPORTED_FEATURE;
      return(errorstatus);
    }
 		else
		{
			errorstatus=SDEnWideBus(WideMode);
 			if(SD_OK==errorstatus)
			{
				SDIO->CLKCR&=~(3<<11);		// 清除之前的位宽设置
				SDIO->CLKCR|=WideMode;// 1位/4位总线宽度
				SDIO->CLKCR|=0<<14;			// 不使用硬件流控制
			}
		}
	}
	return errorstatus;
}
// 设置SD卡工作模式
// Mode:
// 返回值: 错误代码
SD_Error SD_SetDeviceMode(u32 Mode)
{
	SD_Error errorstatus = SD_OK;
 	if((Mode==SD_DMA_MODE)||(Mode==SD_POLLING_MODE))DeviceMode=Mode;
	else errorstatus=SD_INVALID_PARAMETER;
	return errorstatus;
}
// 选中卡
// 发送CMD7，选择卡并使其进入传输状态(rca)
// addr: 卡RCA地址
SD_Error SD_SelectDeselect(u32 addr)
{

  SDIO_CmdInitStructure.SDIO_Argument =  addr;// 发送CMD7，选择卡，短响应
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEL_DESEL_CARD;
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);// 发送CMD7，选择卡，短响应

 	return CmdResp1Error(SD_CMD_SEL_DESEL_CARD);
}
// SD卡读取单个块
// buf: 数据缓存区（必须4字节对齐!）
// addr: 读取地址
// blksize: 块大小
SD_Error SD_ReadBlock(u8 *buf,long long addr,u16 blksize)
{
	SD_Error errorstatus=SD_OK;
	u8 power;
  u32 count=0,*tempbuff=(u32*)buf;// 转换为32位指针
	u32 timeout=SDIO_DATATIMEOUT;
  if(NULL==buf)
		return SD_INVALID_PARAMETER;
  SDIO->DCTRL=0x0;	// 数据控制寄存器清零(关闭DMA)

	if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)// 高容量卡
	{
		blksize=512;
		addr>>=9;
	}
  	SDIO_DataInitStructure.SDIO_DataBlockSize= SDIO_DataBlockSize_1b ;// DPSM数据块大小设置
	  SDIO_DataInitStructure.SDIO_DataLength= 0 ;
	  SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	  SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	  SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	  SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
    SDIO_DataConfig(&SDIO_DataInitStructure);


	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;// 卡被锁定
	if((blksize>0)&&(blksize<=2048)&&((blksize&(blksize-1))==0))
	{
		power=convert_from_bytes_to_power_of_two(blksize);


		SDIO_CmdInitStructure.SDIO_Argument =  blksize;
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN;
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);// 发送CMD16+设置数据长度为blksize，短响应


		errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);	// 检查R1响应

		if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误

	}else return SD_INVALID_PARAMETER;

	  SDIO_DataInitStructure.SDIO_DataBlockSize= power<<4 ;// DPSM数据块大小设置
	  SDIO_DataInitStructure.SDIO_DataLength= blksize ;
	  SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	  SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	  SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToSDIO;
	  SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
    SDIO_DataConfig(&SDIO_DataInitStructure);

	  SDIO_CmdInitStructure.SDIO_Argument =  addr;
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_READ_SINGLE_BLOCK;
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);// 发送CMD17+从addr地址读取数据，短响应

	errorstatus=CmdResp1Error(SD_CMD_READ_SINGLE_BLOCK);// 检查R1响应
	if(errorstatus!=SD_OK)return errorstatus;   		// 响应错误
 	if(DeviceMode==SD_POLLING_MODE)						// 查询模式,读取数据
	{
 		INTX_DISABLE();// 关闭中断（POLLING模式下,需要屏蔽SDIO数据传输完成中断!!!）
		while(!(SDIO->STA&((1<<5)|(1<<1)|(1<<3)|(1<<10)|(1<<9))))// 检查: CRC/超时/完成/开始位错误
		{
			if(SDIO_GetFlagStatus(SDIO_FLAG_RXFIFOHF) != RESET)						// 接收FIFO半满，可读取数据
			{
				for(count=0;count<8;count++)			// 循环读取FIFO数据
				{
					*(tempbuff+count)=SDIO->FIFO;
				}
				tempbuff+=8;
				timeout=0X7FFFFF; 	// 读取数据超时重载
			}else 	// 超时处理
			{
				if(timeout==0)return SD_DATA_TIMEOUT;
				timeout--;
			}
		}
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		// 数据超时错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	// 清除超时标志
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	// 数据CRC错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		// 清除错误标志
			return SD_DATA_CRC_FAIL;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR) != RESET) 	// 接收fifo溢出错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_RXOVERR);		// 清除错误标志
			return SD_RX_OVERRUN;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	// 接收开始位错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);// 清除错误标志
			return SD_START_BIT_ERR;
		}
		while(SDIO_GetFlagStatus(SDIO_FLAG_RXDAVL) != RESET)	// FIFO还有数据可读
		{
			*tempbuff=SDIO->FIFO;	// 循环读取FIFO数据
			tempbuff++;
		}
		INTX_ENABLE();// 开启中断
		SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记

	}else if(DeviceMode==SD_DMA_MODE)
	{
 		TransferError=SD_OK;
		StopCondition=0;			// 单块传输，不需要发送停止命令
		TransferEnd=0;				// 传输完成标志置零，等待传输完成中断设置
		SDIO->MASK|=(1<<1)|(1<<3)|(1<<8)|(1<<5)|(1<<9);	// 配置需要的中断
	 	SDIO->DCTRL|=1<<3;		 	// SDIO DMA使能
 	    SD_DMA_Config((u32*)buf,blksize,DMA_DIR_PeripheralToMemory);
 		while((DMA_GetFlagStatus(SD_DMA_STREAM, SD_DMA_TC_FLAG) == RESET) &&
              (TransferEnd==0)&&(TransferError==SD_OK)&&timeout)timeout--;// 等待传输完成
		if(timeout==0)return SD_DATA_TIMEOUT;// 超时
		if(TransferError!=SD_OK)errorstatus=TransferError;
    }
 	return errorstatus;
}
__align(4) u32 *tempbuff;
// SD卡读取多个块
// buf: 数据缓存区
// addr: 读取地址
// blksize: 块大小
// nblks: 要读取的块数
// 返回值: 错误代码
SD_Error SD_ReadMultiBlocks(u8 *buf,long long addr,u16 blksize,u32 nblks)
{
  SD_Error errorstatus=SD_OK;
	u8 power;
  u32 count=0;
	u32 timeout=SDIO_DATATIMEOUT;
	tempbuff=(u32*)buf;// 转换为32位指针

  SDIO->DCTRL=0x0;		// 数据控制寄存器清零(关闭DMA)
	if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)// 高容量卡
	{
		blksize=512;
		addr>>=9;
	}

	  SDIO_DataInitStructure.SDIO_DataBlockSize= 0; ;// DPSM数据块大小设置
	  SDIO_DataInitStructure.SDIO_DataLength= 0 ;
	  SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	  SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	  SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	  SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
    SDIO_DataConfig(&SDIO_DataInitStructure);

	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;// 卡被锁定
	if((blksize>0)&&(blksize<=2048)&&((blksize&(blksize-1))==0))
	{
		power=convert_from_bytes_to_power_of_two(blksize);

	  SDIO_CmdInitStructure.SDIO_Argument =  blksize;// 发送CMD16+设置数据长度为blksize，短响应
		SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN;
		SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		SDIO_SendCommand(&SDIO_CmdInitStructure);

		errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);	// 检查R1响应

		if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误

	}else return SD_INVALID_PARAMETER;

	if(nblks>1)											// 多块读取
	{
 	  	if(nblks*blksize>SD_MAX_DATA_LENGTH)return SD_INVALID_PARAMETER;// 判断是否超出最大数据长度

		   SDIO_DataInitStructure.SDIO_DataBlockSize= power<<4; ;// nblks*blksize,512块大小，最大值65535
			 SDIO_DataInitStructure.SDIO_DataLength= nblks*blksize ;
			 SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
			 SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
			 SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToSDIO;
			 SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
			 SDIO_DataConfig(&SDIO_DataInitStructure);

       SDIO_CmdInitStructure.SDIO_Argument =  addr;// 发送CMD18+从addr地址读取数据，短响应
	     SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_READ_MULT_BLOCK;
		   SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		   SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		   SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		   SDIO_SendCommand(&SDIO_CmdInitStructure);

		errorstatus=CmdResp1Error(SD_CMD_READ_MULT_BLOCK);// 检查R1响应

		if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误

 		if(DeviceMode==SD_POLLING_MODE)
		{
			INTX_DISABLE();// 关闭中断（POLLING模式下,需要屏蔽SDIO数据传输完成中断!!!）
			while(!(SDIO->STA&((1<<5)|(1<<1)|(1<<3)|(1<<8)|(1<<9))))// 检查: CRC/超时/完成/开始位错误
			{
				if(SDIO_GetFlagStatus(SDIO_FLAG_RXFIFOHF) != RESET)						// 接收FIFO半满，可读取数据
				{
					for(count=0;count<8;count++)			// 循环读取FIFO数据
					{
						*(tempbuff+count)=SDIO->FIFO;
					}
					tempbuff+=8;
					timeout=0X7FFFFF; 	// 读取数据超时重载
				}else 	// 超时处理
				{
					if(timeout==0)return SD_DATA_TIMEOUT;
					timeout--;
				}
			}
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		// 数据超时错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	// 清除超时标志
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	// 数据CRC错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		// 清除错误标志
			return SD_DATA_CRC_FAIL;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR) != RESET) 	// 接收fifo溢出错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_RXOVERR);		// 清除错误标志
			return SD_RX_OVERRUN;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	// 接收开始位错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);// 清除错误标志
			return SD_START_BIT_ERR;
		}

		while(SDIO_GetFlagStatus(SDIO_FLAG_RXDAVL) != RESET)	// FIFO还有数据可读
		{
			*tempbuff=SDIO->FIFO;	// 循环读取FIFO数据
			tempbuff++;
		}
	 		if(SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) != RESET)		// 接收完成
			{
				if((SDIO_STD_CAPACITY_SD_CARD_V1_1==CardType)||(SDIO_STD_CAPACITY_SD_CARD_V2_0==CardType)||(SDIO_HIGH_CAPACITY_SD_CARD==CardType))
				{
					SDIO_CmdInitStructure.SDIO_Argument =  0;// 发送CMD12+停止传输
				  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_STOP_TRANSMISSION;
					SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
					SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
					SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
					SDIO_SendCommand(&SDIO_CmdInitStructure);

					errorstatus=CmdResp1Error(SD_CMD_STOP_TRANSMISSION);// 检查R1响应

					if(errorstatus!=SD_OK)return errorstatus;
				}
 			}
			INTX_ENABLE();// 开启中断
	 		SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
 		}else if(DeviceMode==SD_DMA_MODE)
		{
	   		TransferError=SD_OK;
			StopCondition=1;			// 多块读取，需要发送停止命令
			TransferEnd=0;				// 传输完成标志置零，等待传输完成中断设置
			SDIO->MASK|=(1<<1)|(1<<3)|(1<<8)|(1<<5)|(1<<9);	// 配置需要的中断
		 	SDIO->DCTRL|=1<<3;		 						// SDIO DMA使能
 	    SD_DMA_Config((u32*)buf,nblks*blksize,DMA_DIR_PeripheralToMemory);
		 		while((DMA_GetFlagStatus(SD_DMA_STREAM, SD_DMA_TC_FLAG) == RESET) &&
                 timeout)timeout--;// 等待传输完成
			if(timeout==0)return SD_DATA_TIMEOUT;// 超时
			while((TransferEnd==0)&&(TransferError==SD_OK));
			if(TransferError!=SD_OK)errorstatus=TransferError;
		}
  	}
	return errorstatus;
}
// SD卡写入1个块
// buf: 数据缓存区
// addr: 写入地址
// blksize: 块大小
// 返回值: 错误代码
SD_Error SD_WriteBlock(u8 *buf,long long addr,  u16 blksize)
{
	SD_Error errorstatus = SD_OK;

	u8  power=0,cardstate=0;

	u32 timeout=0,bytestransferred=0;

	u32 cardstatus=0,count=0,restwords=0;

	u32	tlen=blksize;						// 总长度(字节)

	u32*tempbuff=(u32*)buf;

 	if(buf==NULL)return SD_INVALID_PARAMETER;// 参数错误

  SDIO->DCTRL=0x0;							// 数据控制寄存器清零(关闭DMA)

	SDIO_DataInitStructure.SDIO_DataBlockSize= 0; ;// DPSM数据块大小设置
	SDIO_DataInitStructure.SDIO_DataLength= 0 ;
	SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
  SDIO_DataConfig(&SDIO_DataInitStructure);


	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;// 卡被锁定
 	if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)	// 高容量卡
	{
		blksize=512;
		addr>>=9;
	}
	if((blksize>0)&&(blksize<=2048)&&((blksize&(blksize-1))==0))
	{
		power=convert_from_bytes_to_power_of_two(blksize);

		SDIO_CmdInitStructure.SDIO_Argument = blksize;// 发送CMD16+设置数据长度为blksize，短响应
		SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN;
		SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		SDIO_SendCommand(&SDIO_CmdInitStructure);

		errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);	// 检查R1响应

		if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误

	}else return SD_INVALID_PARAMETER;

			SDIO_CmdInitStructure.SDIO_Argument = (u32)RCA<<16;// 发送CMD13，查询卡的状态，短响应
		  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_STATUS;
			SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
			SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
			SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
			SDIO_SendCommand(&SDIO_CmdInitStructure);

	  errorstatus=CmdResp1Error(SD_CMD_SEND_STATUS);		// 检查R1响应

	if(errorstatus!=SD_OK)return errorstatus;
	cardstatus=SDIO->RESP1;
	timeout=SD_DATATIMEOUT;
   	while(((cardstatus&0x00000100)==0)&&(timeout>0)) 	// 检查READY_FOR_DATA位是否置位
	{
		timeout--;

		SDIO_CmdInitStructure.SDIO_Argument = (u32)RCA<<16;// 发送CMD13，查询卡的状态，短响应
		SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_STATUS;
		SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		SDIO_SendCommand(&SDIO_CmdInitStructure);

		errorstatus=CmdResp1Error(SD_CMD_SEND_STATUS);	// 检查R1响应

		if(errorstatus!=SD_OK)return errorstatus;

		cardstatus=SDIO->RESP1;
	}
	if(timeout==0)return SD_ERROR;

			SDIO_CmdInitStructure.SDIO_Argument = addr;// 发送CMD24，写单块命令，短响应
			SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_WRITE_SINGLE_BLOCK;
			SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
			SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
			SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
			SDIO_SendCommand(&SDIO_CmdInitStructure);

	errorstatus=CmdResp1Error(SD_CMD_WRITE_SINGLE_BLOCK);// 检查R1响应

	if(errorstatus!=SD_OK)return errorstatus;

	StopCondition=0;									// 单块写不需要发送停止命令

	SDIO_DataInitStructure.SDIO_DataBlockSize= power<<4; ;	//blksize, 块大小设置
	SDIO_DataInitStructure.SDIO_DataLength= blksize ;
	SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
  SDIO_DataConfig(&SDIO_DataInitStructure);


	timeout=SDIO_DATATIMEOUT;

	if (DeviceMode == SD_POLLING_MODE)
	{
		INTX_DISABLE();// 关闭中断（POLLING模式下,需要屏蔽SDIO数据传输完成中断!!!）
		while(!(SDIO->STA&((1<<10)|(1<<4)|(1<<1)|(1<<3)|(1<<9))))// 数据块发送完成/下溢/CRC错误/超时/开始位错误
		{
			if(SDIO_GetFlagStatus(SDIO_FLAG_TXFIFOHE) != RESET)							// 发送FIFO半空，可写入数据
			{
				if((tlen-bytestransferred)<SD_HALFFIFOBYTES)// 不够32字节了
				{
					restwords=((tlen-bytestransferred)%4==0)?((tlen-bytestransferred)/4):((tlen-bytestransferred)/4+1);

					for(count=0;count<restwords;count++,tempbuff++,bytestransferred+=4)
					{
						SDIO->FIFO=*tempbuff;
					}
				}else
				{
					for(count=0;count<8;count++)
					{
						SDIO->FIFO=*(tempbuff+count);
					}
					tempbuff+=8;
					bytestransferred+=32;
				}
				timeout=0X3FFFFFFF;	// 写数据超时重载
			}else
			{
				if(timeout==0)return SD_DATA_TIMEOUT;
				timeout--;
			}
		}
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		// 数据超时错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	// 清除超时标志
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	// 数据CRC错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		// 清除错误标志
			return SD_DATA_CRC_FAIL;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_TXUNDERR) != RESET) 	// 发送fifo下溢错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_TXUNDERR);		// 清除错误标志
			return SD_TX_UNDERRUN;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	// 接收开始位错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);// 清除错误标志
			return SD_START_BIT_ERR;
		}

		INTX_ENABLE();// 开启中断
		SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
	}else if(DeviceMode==SD_DMA_MODE)
	{
   		TransferError=SD_OK;
		StopCondition=0;			// 单块写，不需要发送停止命令
		TransferEnd=0;				// 传输完成标志置零，等待传输完成中断设置
		SDIO->MASK|=(1<<1)|(1<<3)|(1<<8)|(1<<4)|(1<<9);	// 配置需要的中断
		SD_DMA_Config((u32*)buf,blksize,DMA_DIR_MemoryToPeripheral);				// SDIO DMA配置
 	 	SDIO->DCTRL|=1<<3;								// SDIO DMA使能
 		while((DMA_GetFlagStatus(SD_DMA_STREAM, SD_DMA_TC_FLAG) == RESET) &&
                 timeout)timeout--;// 等待传输完成
		if(timeout==0)
		{
  			SD_Init();	 					// 重新初始化SD卡，恢复到写入前的状态
			return SD_DATA_TIMEOUT;			// 超时
 		}
		timeout=SDIO_DATATIMEOUT;
		while((TransferEnd==0)&&(TransferError==SD_OK)&&timeout)timeout--;
 		if(timeout==0)return SD_DATA_TIMEOUT;			// 超时
  		if(TransferError!=SD_OK)return TransferError;
 	}
 	SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
 	errorstatus=IsCardProgramming(&cardstate);
 	while((errorstatus==SD_OK)&&((cardstate==SD_CARD_PROGRAMMING)||(cardstate==SD_CARD_RECEIVING)))
	{
		errorstatus=IsCardProgramming(&cardstate);
	}
	return errorstatus;
}
// SD卡写入多个块
// buf: 数据缓存区
// addr: 写入地址
// blksize: 块大小
// nblks: 要写入的块数
// 返回值: 错误代码
SD_Error SD_WriteMultiBlocks(u8 *buf,long long addr,u16 blksize,u32 nblks)
{
	SD_Error errorstatus = SD_OK;
	u8  power = 0, cardstate = 0;
	u32 timeout=0,bytestransferred=0;
	u32 count = 0, restwords = 0;
	u32 tlen=nblks*blksize;				// 总长度(字节)
	u32 *tempbuff = (u32*)buf;
  if(buf==NULL)return SD_INVALID_PARAMETER; // 参数错误
  SDIO->DCTRL=0x0;							// 数据控制寄存器清零(关闭DMA)

	SDIO_DataInitStructure.SDIO_DataBlockSize= 0; ;	// DPSM数据块大小设置
	SDIO_DataInitStructure.SDIO_DataLength= 0 ;
	SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
  SDIO_DataConfig(&SDIO_DataInitStructure);

	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;// 卡被锁定
 	if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)// 高容量卡
	{
		blksize=512;
		addr>>=9;
	}
	if((blksize>0)&&(blksize<=2048)&&((blksize&(blksize-1))==0))
	{
		power=convert_from_bytes_to_power_of_two(blksize);

		SDIO_CmdInitStructure.SDIO_Argument = blksize;	// 发送CMD16+设置数据长度为blksize，短响应
		SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN;
		SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		SDIO_SendCommand(&SDIO_CmdInitStructure);

		errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);	// 检查R1响应

		if(errorstatus!=SD_OK)return errorstatus;   	// 响应错误

	}else return SD_INVALID_PARAMETER;
	if(nblks>1)
	{
		if(nblks*blksize>SD_MAX_DATA_LENGTH)return SD_INVALID_PARAMETER;
     	if((SDIO_STD_CAPACITY_SD_CARD_V1_1==CardType)||(SDIO_STD_CAPACITY_SD_CARD_V2_0==CardType)||(SDIO_HIGH_CAPACITY_SD_CARD==CardType))
    	{
			// 预设置块数
				SDIO_CmdInitStructure.SDIO_Argument = (u32)RCA<<16;		// 发送ACMD55，短响应
				SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;
				SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
				SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
				SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
				SDIO_SendCommand(&SDIO_CmdInitStructure);

			errorstatus=CmdResp1Error(SD_CMD_APP_CMD);		// 检查R1响应

			if(errorstatus!=SD_OK)return errorstatus;

				SDIO_CmdInitStructure.SDIO_Argument =nblks;		// 发送CMD23，设置块数量，短响应
				SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCK_COUNT;
				SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
				SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
				SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
				SDIO_SendCommand(&SDIO_CmdInitStructure);

				errorstatus=CmdResp1Error(SD_CMD_SET_BLOCK_COUNT);// 检查R1响应

			if(errorstatus!=SD_OK)return errorstatus;

		}

				SDIO_CmdInitStructure.SDIO_Argument =addr;	// 发送CMD25，多块写命令，短响应
				SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_WRITE_MULT_BLOCK;
				SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
				SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
				SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
				SDIO_SendCommand(&SDIO_CmdInitStructure);

 		errorstatus=CmdResp1Error(SD_CMD_WRITE_MULT_BLOCK);	// 检查R1响应

		if(errorstatus!=SD_OK)return errorstatus;

        SDIO_DataInitStructure.SDIO_DataBlockSize= power<<4; ;	//blksize, 块大小设置
				SDIO_DataInitStructure.SDIO_DataLength= nblks*blksize ;
				SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
				SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
				SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
				SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
				SDIO_DataConfig(&SDIO_DataInitStructure);

		if(DeviceMode==SD_POLLING_MODE)
	    {
			timeout=SDIO_DATATIMEOUT;
			INTX_DISABLE();// 关闭中断（POLLING模式下,需要屏蔽SDIO数据传输完成中断!!!）
			while(!(SDIO->STA&((1<<4)|(1<<1)|(1<<8)|(1<<3)|(1<<9))))// 下溢/CRC/完成/超时/开始位错误
			{
				if(SDIO_GetFlagStatus(SDIO_FLAG_TXFIFOHE) != RESET)							// 发送FIFO半空，可写入数据
				{
					if((tlen-bytestransferred)<SD_HALFFIFOBYTES)// 不够32字节了
					{
						restwords=((tlen-bytestransferred)%4==0)?((tlen-bytestransferred)/4):((tlen-bytestransferred)/4+1);
						for(count=0;count<restwords;count++,tempbuff++,bytestransferred+=4)
						{
							SDIO->FIFO=*tempbuff;
						}
					}else 										// 发送FIFO半空，可写入32字节数据
					{
						for(count=0;count<SD_HALFFIFO;count++)
						{
							SDIO->FIFO=*(tempbuff+count);
						}
						tempbuff+=SD_HALFFIFO;
						bytestransferred+=SD_HALFFIFOBYTES;
					}
					timeout=0X3FFFFFFF;	// 写数据超时重载
				}else
				{
					if(timeout==0)return SD_DATA_TIMEOUT;
					timeout--;
				}
			}
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		// 数据超时错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	// 清除超时标志
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	// 数据CRC错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		// 清除错误标志
			return SD_DATA_CRC_FAIL;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_TXUNDERR) != RESET) 	// 发送fifo下溢错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_TXUNDERR);		// 清除错误标志
			return SD_TX_UNDERRUN;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	// 接收开始位错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);// 清除错误标志
			return SD_START_BIT_ERR;
		}

			if(SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) != RESET)		// 发送完成
			{
				if((SDIO_STD_CAPACITY_SD_CARD_V1_1==CardType)||(SDIO_STD_CAPACITY_SD_CARD_V2_0==CardType)||(SDIO_HIGH_CAPACITY_SD_CARD==CardType))
				{
					SDIO_CmdInitStructure.SDIO_Argument =0;// 发送CMD12+停止传输
					SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_STOP_TRANSMISSION;
					SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
					SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
					SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
					SDIO_SendCommand(&SDIO_CmdInitStructure);

					errorstatus=CmdResp1Error(SD_CMD_STOP_TRANSMISSION);// 检查R1响应
					if(errorstatus!=SD_OK)return errorstatus;
				}
			}
			INTX_ENABLE();// 开启中断
	 		SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
	    }else if(DeviceMode==SD_DMA_MODE)
		{
	   	TransferError=SD_OK;
			StopCondition=1;			// 多块写，需要发送停止命令
			TransferEnd=0;				// 传输完成标志置零，等待传输完成中断设置
			SDIO->MASK|=(1<<1)|(1<<3)|(1<<8)|(1<<4)|(1<<9);	// 配置需要的中断
			SD_DMA_Config((u32*)buf,nblks*blksize,DMA_DIR_MemoryToPeripheral);		// SDIO DMA配置
	 	 	SDIO->DCTRL|=1<<3;								// SDIO DMA使能
			timeout=SDIO_DATATIMEOUT;
	 		while((DMA_GetFlagStatus(SD_DMA_STREAM, SD_DMA_TC_FLAG) == RESET) &&
                 timeout)timeout--;// 等待传输完成
			if(timeout==0)	 								// 超时
			{
  				SD_Init();	 					// 重新初始化SD卡，恢复到写入前的状态
	 			return SD_DATA_TIMEOUT;			// 超时
	 		}
			timeout=SDIO_DATATIMEOUT;
			while((TransferEnd==0)&&(TransferError==SD_OK)&&timeout)timeout--;
	 		if(timeout==0)return SD_DATA_TIMEOUT;			// 超时
	 		if(TransferError!=SD_OK)return TransferError;
		}
  	}
 	SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
 	errorstatus=IsCardProgramming(&cardstate);
 	while((errorstatus==SD_OK)&&((cardstate==SD_CARD_PROGRAMMING)||(cardstate==SD_CARD_RECEIVING)))
	{
		errorstatus=IsCardProgramming(&cardstate);
	}
	return errorstatus;
}
// SDIO中断处理函数
void SDIO_IRQHandler(void)
{
 	SD_ProcessIRQSrc();// 处理所有SDIO中断
}
// SDIO中断处理回调函数
// 处理SDIO传输过程中的各种中断事件
// 返回值: 错误代码
SD_Error SD_ProcessIRQSrc(void)
{
	if(SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) != RESET)// 数据传输完成中断
	{
		if (StopCondition==1)
		{
				SDIO_CmdInitStructure.SDIO_Argument =0;// 发送CMD12+停止传输
				SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_STOP_TRANSMISSION;
				SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
				SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
				SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
				SDIO_SendCommand(&SDIO_CmdInitStructure);

			TransferError=CmdResp1Error(SD_CMD_STOP_TRANSMISSION);
		}else TransferError = SD_OK;
 		SDIO->ICR|=1<<8;// 清除数据完成中断标志
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));// 关闭相关中断
 		TransferEnd = 1;
		return(TransferError);
	}
 	if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)// 数据CRC错误
	{
		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		// 清除错误标志
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));// 关闭相关中断
	    TransferError = SD_DATA_CRC_FAIL;
	    return(SD_DATA_CRC_FAIL);
	}
 	if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)// 数据超时错误
	{
		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT);  			// 清除超时标志
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));// 关闭相关中断
	    TransferError = SD_DATA_TIMEOUT;
	    return(SD_DATA_TIMEOUT);
	}
  	if(SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR) != RESET)// FIFO接收溢出
	{
		SDIO_ClearFlag(SDIO_FLAG_RXOVERR);  			// 清除溢出标志
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));// 关闭相关中断
	    TransferError = SD_RX_OVERRUN;
	    return(SD_RX_OVERRUN);
	}
   	if(SDIO_GetFlagStatus(SDIO_FLAG_TXUNDERR) != RESET)// FIFO发送下溢
	{
		SDIO_ClearFlag(SDIO_FLAG_TXUNDERR);  			// 清除下溢标志
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));// 关闭相关中断
	    TransferError = SD_TX_UNDERRUN;
	    return(SD_TX_UNDERRUN);
	}
	if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET)// 开始位错误
	{
		SDIO_ClearFlag(SDIO_FLAG_STBITERR);  		// 清除错误标志
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));// 关闭相关中断
	    TransferError = SD_START_BIT_ERR;
	    return(SD_START_BIT_ERR);
	}
	return(SD_OK);
}

// 检查CMD0的执行状态
// 返回值: SD卡错误码
SD_Error CmdError(void)
{
	SD_Error errorstatus = SD_OK;
	u32 timeout=SDIO_CMD0TIMEOUT;
	while(timeout--)
	{
		if(SDIO_GetFlagStatus(SDIO_FLAG_CMDSENT) != RESET)break;	// 命令已发送(无响应)
	}
	if(timeout==0)return SD_CMD_RSP_TIMEOUT;
	SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
	return errorstatus;
}
// 检查R7响应的执行状态
// 返回值: SD卡错误码
SD_Error CmdResp7Error(void)
{
	SD_Error errorstatus=SD_OK;
	u32 status;
	u32 timeout=SDIO_CMD0TIMEOUT;
 	while(timeout--)
	{
		status=SDIO->STA;
		if(status&((1<<0)|(1<<2)|(1<<6)))break;// CRC错误/命令响应超时/命令响应接收完成(至少有一个)
	}
 	if((timeout==0)||(status&(1<<2)))	// 响应超时
	{
		errorstatus=SD_CMD_RSP_TIMEOUT;	// 当前卡不是2.0卡，或者不支持CMD8命令，返回超时错误
		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT); 			// 清除命令响应超时标志
		return errorstatus;
	}
	if(status&1<<6)						// 成功接收到响应
	{
		errorstatus=SD_OK;
		SDIO_ClearFlag(SDIO_FLAG_CMDREND); 				// 清除命令响应完成标志
 	}
	return errorstatus;
}
// 检查R1响应的执行状态
// cmd: 命令码
// 返回值: SD卡错误码
SD_Error CmdResp1Error(u8 cmd)
{
   	u32 status;
	 u32 timeout=SDIO_CMD0TIMEOUT;
	while(timeout--)
	{
		status=SDIO->STA;
		if(status&((1<<0)|(1<<2)|(1<<6)))break;
	}
	if(timeout==0U){SDIO_ClearFlag(SDIO_STATIC_FLAGS);return SD_CMD_RSP_TIMEOUT;}
	if(SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET)					// 响应超时
	{
 		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT); 				// 清除命令响应超时标志
		return SD_CMD_RSP_TIMEOUT;
	}
 	if(SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET)					// CRC错误
	{
 		SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL); 				// 清除错误标志
		return SD_CMD_CRC_FAIL;
	}
	if(SDIO->RESPCMD!=cmd)return SD_ILLEGAL_CMD;// 响应命令不匹配
  SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
	return (SD_Error)(SDIO->RESP1&SD_OCR_ERRORBITS);// 返回卡错误响应
}
// 检查R3响应的执行状态
// 返回值: 错误代码
SD_Error CmdResp3Error(void)
{
	u32 status;
	u32 timeout=SDIO_CMD0TIMEOUT;
	while(timeout--)
	{
		status=SDIO->STA;
		if(status&((1<<0)|(1<<2)|(1<<6)))break;// CRC错误/命令响应超时/命令响应接收完成(至少有一个)
	}
 	if(SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET)					// 响应超时
	{
		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT);			// 清除命令响应超时标志
		return SD_CMD_RSP_TIMEOUT;
	}
   SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
 	return SD_OK;
}
// 检查R2响应的执行状态
// 返回值: 错误代码
SD_Error CmdResp2Error(void)
{
	SD_Error errorstatus=SD_OK;
	u32 status;
	u32 timeout=SDIO_CMD0TIMEOUT;
 	while(timeout--)
	{
		status=SDIO->STA;
		if(status&((1<<0)|(1<<2)|(1<<6)))break;// CRC错误/命令响应超时/命令响应接收完成(至少有一个)
	}
  	if((timeout==0)||(status&(1<<2)))	// 响应超时
	{
		errorstatus=SD_CMD_RSP_TIMEOUT;
		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT); 		// 清除命令响应超时标志
		return errorstatus;
	}
	if(SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET)						// CRC错误
	{
		errorstatus=SD_CMD_CRC_FAIL;
		SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL);		// 清除错误标志
 	}
	SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
 	return errorstatus;
}
// 检查R6响应的执行状态
// cmd: 期望的命令码
// prca: 返回卡相对地址RCA
// 返回值: 错误代码
SD_Error CmdResp6Error(u8 cmd,u16*prca)
{
	SD_Error errorstatus=SD_OK;
	u32 status;
	u32 rspr1;
	u32 timeout=SDIO_CMD0TIMEOUT;
	while(timeout--)
	{
		status=SDIO->STA;
		if(status&((1<<0)|(1<<2)|(1<<6)))break;
	}
	if(timeout==0U){SDIO_ClearFlag(SDIO_STATIC_FLAGS);return SD_CMD_RSP_TIMEOUT;}
	if(SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET)					// 响应超时
	{
 		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT);			// 清除命令响应超时标志
		return SD_CMD_RSP_TIMEOUT;
	}
	if(SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET)						// CRC错误
	{
		SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL);					// 清除错误标志
 		return SD_CMD_CRC_FAIL;
	}
	if(SDIO->RESPCMD!=cmd)				// 检查响应命令是否匹配
	{
 		return SD_ILLEGAL_CMD;
	}
	SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
	rspr1=SDIO->RESP1;					// 得到响应
	if(SD_ALLZERO==(rspr1&(SD_R6_GENERAL_UNKNOWN_ERROR|SD_R6_ILLEGAL_CMD|SD_R6_COM_CRC_FAILED)))
	{
		*prca=(u16)(rspr1>>16);			// 右移16位得到rca
		return errorstatus;
	}
   	if(rspr1&SD_R6_GENERAL_UNKNOWN_ERROR)return SD_GENERAL_UNKNOWN_ERROR;
   	if(rspr1&SD_R6_ILLEGAL_CMD)return SD_ILLEGAL_CMD;
   	if(rspr1&SD_R6_COM_CRC_FAILED)return SD_COM_CRC_FAILED;
	return errorstatus;
}

// SDIO使能宽总线模式
// enx: 0,不使能; 1,使能
// 返回值: 错误代码
SD_Error SDEnWideBus(u8 enx)
{
	SD_Error errorstatus = SD_OK;
 	u32 scr[2]={0,0};
	u8 arg=0X00;
	if(enx)arg=0X02;
	else arg=0X00;
 	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;// SD卡处于LOCKED状态
 	errorstatus=FindSCR(RCA,scr);						// 获取SCR寄存器数据
 	if(errorstatus!=SD_OK)return errorstatus;
	if((scr[1]&SD_WIDE_BUS_SUPPORT)!=SD_ALLZERO)		// 支持宽总线
	{
		  SDIO_CmdInitStructure.SDIO_Argument = (uint32_t) RCA << 16;// 发送CMD55+RCA，短响应
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);

	 	errorstatus=CmdResp1Error(SD_CMD_APP_CMD);

	 	if(errorstatus!=SD_OK)return errorstatus;

		  SDIO_CmdInitStructure.SDIO_Argument = arg;// 发送ACMD6，短响应，参数: 10,4位; 00,1位
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_SD_SET_BUSWIDTH;
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);

     errorstatus=CmdResp1Error(SD_CMD_APP_SD_SET_BUSWIDTH);

		return errorstatus;
	}else return SD_REQUEST_NOT_APPLICABLE;				// 不支持宽总线模式
}
// 检查卡是否正在执行写操作
// pstatus: 当前卡状态
// 返回值: 错误代码
SD_Error IsCardProgramming(u8 *pstatus)
{
 	vu32 respR1 = 0, status = 0;

  SDIO_CmdInitStructure.SDIO_Argument = (uint32_t) RCA << 16; // 卡相对地址
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_STATUS;// 发送CMD13
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);

	status=SDIO->STA;

	while(!(status&((1<<0)|(1<<6)|(1<<2))))status=SDIO->STA;// 等待响应接收完成
   	if(SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET)			// CRC检测失败
	{
	  SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL);	// 清除错误标志
		return SD_CMD_CRC_FAIL;
	}
   	if(SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET)			// 命令超时
	{
		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT);			// 清除超时标志
		return SD_CMD_RSP_TIMEOUT;
	}
 	if(SDIO->RESPCMD!=SD_CMD_SEND_STATUS)return SD_ILLEGAL_CMD;
	SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
	respR1=SDIO->RESP1;
	*pstatus=(u8)((respR1>>9)&0x0000000F);
	return SD_OK;
}
// 获取卡状态
// pcardstatus: 卡状态
// 返回值: 错误代码
SD_Error SD_SendStatus(uint32_t *pcardstatus)
{
	SD_Error errorstatus = SD_OK;
	if(pcardstatus==NULL)
	{
		errorstatus=SD_INVALID_PARAMETER;
		return errorstatus;
	}

	SDIO_CmdInitStructure.SDIO_Argument = (uint32_t) RCA << 16;// 发送CMD13，短响应
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_STATUS;
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);

	errorstatus=CmdResp1Error(SD_CMD_SEND_STATUS);	// 检查响应
	if(errorstatus!=SD_OK)return errorstatus;
	*pcardstatus=SDIO->RESP1;// 读取响应值
	return errorstatus;
}
// 返回SD卡状态
// 返回值: SD卡状态
SDCardState SD_GetState(void)
{
	u32 resp1=0;
	if(SD_SendStatus(&resp1)!=SD_OK)return SD_CARD_ERROR;
	else return (SDCardState)((resp1>>9) & 0x0F);
}
// 查找SD卡SCR寄存器值
// rca: 卡相对地址
// pscr: 数据缓存区（存储SCR内容）
// 返回值: 错误代码
SD_Error FindSCR(u16 rca,u32 *pscr)
{
	u32 index = 0;
	SD_Error errorstatus = SD_OK;
	u32 tempscr[2]={0,0};

	SDIO_CmdInitStructure.SDIO_Argument = (uint32_t)8;	 // 发送CMD16，短响应，设置Block Size为8字节
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN; //	 cmd16
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;  //r1
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);

 	errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);

 	if(errorstatus!=SD_OK)return errorstatus;

  SDIO_CmdInitStructure.SDIO_Argument = (uint32_t) RCA << 16;
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;// 发送CMD55，短响应
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);

 	errorstatus=CmdResp1Error(SD_CMD_APP_CMD);
 	if(errorstatus!=SD_OK)return errorstatus;

  SDIO_DataInitStructure.SDIO_DataTimeOut = SD_DATATIMEOUT;
  SDIO_DataInitStructure.SDIO_DataLength = 8;  // 8字节数据长度，block大小为8字节(SCR为64位)
  SDIO_DataInitStructure.SDIO_DataBlockSize = SDIO_DataBlockSize_8b  ;  // 块大小8byte
  SDIO_DataInitStructure.SDIO_TransferDir = SDIO_TransferDir_ToSDIO;
  SDIO_DataInitStructure.SDIO_TransferMode = SDIO_TransferMode_Block;
  SDIO_DataInitStructure.SDIO_DPSM = SDIO_DPSM_Enable;
  SDIO_DataConfig(&SDIO_DataInitStructure);

  SDIO_CmdInitStructure.SDIO_Argument = 0x0;
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SD_APP_SEND_SCR;	// 发送ACMD51，短响应，参数为0
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;  //r1
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);

 	errorstatus=CmdResp1Error(SD_CMD_SD_APP_SEND_SCR);
 	if(errorstatus!=SD_OK)return errorstatus;
 	while(!(SDIO->STA&(SDIO_FLAG_RXOVERR|SDIO_FLAG_DCRCFAIL|SDIO_FLAG_DTIMEOUT|SDIO_FLAG_DBCKEND|SDIO_FLAG_STBITERR)))
	{
		if(SDIO_GetFlagStatus(SDIO_FLAG_RXDAVL) != RESET)// 接收FIFO数据可用
		{
			*(tempscr+index)=SDIO->FIFO;	// 读取FIFO数据
			index++;
			if(index>=2)break;
		}
	}
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		// 数据超时错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	// 清除超时标志
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	// 数据CRC错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		// 清除错误标志
			return SD_DATA_CRC_FAIL;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR) != RESET) 	// 接收fifo溢出错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_RXOVERR);		// 清除错误标志
			return SD_RX_OVERRUN;
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	// 接收开始位错误
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);// 清除错误标志
			return SD_START_BIT_ERR;
		}
   SDIO_ClearFlag(SDIO_STATIC_FLAGS);// 清除所有标记
	// 数据顺序是8位反序的，需要进行字节序转换
	*(pscr+1)=((tempscr[0]&SD_0TO7BITS)<<24)|((tempscr[0]&SD_8TO15BITS)<<8)|((tempscr[0]&SD_16TO23BITS)>>8)|((tempscr[0]&SD_24TO31BITS)>>24);
	*(pscr)=((tempscr[1]&SD_0TO7BITS)<<24)|((tempscr[1]&SD_8TO15BITS)<<8)|((tempscr[1]&SD_16TO23BITS)>>8)|((tempscr[1]&SD_24TO31BITS)>>24);
 	return errorstatus;
}
// 获取NumberOfBytes以2为底的指数
// NumberOfBytes: 字节数
// 返回值: 以2为底的指数值
u8 convert_from_bytes_to_power_of_two(u16 NumberOfBytes)
{
	u8 count=0;
	while(NumberOfBytes!=1)
	{
		NumberOfBytes>>=1;
		count++;
	}
	return count;
}

// 配置SDIO DMA
// mbuf: 存储器地址
// bufsize: 传输的数据量
// dir: 方向; DMA_DIR_MemoryToPeripheral  存储器->SDIO(写数据); DMA_DIR_PeripheralToMemory SDIO-->存储器(读数据);
void SD_DMA_Config(u32*mbuf,u32 bufsize,u32 dir)
{

  DMA_InitTypeDef  DMA_InitStructure;

	while (DMA_GetCmdStatus(SD_DMA_STREAM) != DISABLE){}
  DMA_DeInit(SD_DMA_STREAM);// 重置DMA流

  DMA_ClearFlag(SD_DMA_STREAM, SD_DMA_ALL_FLAGS);


  DMA_InitStructure.DMA_Channel = SD_DMA_CHANNEL;  // 通道选择
  DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)&SDIO->FIFO;// DMA外设地址
  DMA_InitStructure.DMA_Memory0BaseAddr = (u32)mbuf;// DMA 存储器地址
  DMA_InitStructure.DMA_DIR = dir;// 存储器到外设方向
  DMA_InitStructure.DMA_BufferSize = 0;// 数据传输量
  DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;// 外设地址不自增
  DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;// 存储器地址自增
  DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;// 外设数据宽度: 32位
  DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Word;// 存储器数据宽度: 32位
  DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;// 普通模式
  DMA_InitStructure.DMA_Priority = DMA_Priority_VeryHigh;// 最高优先级
  DMA_InitStructure.DMA_FIFOMode = DMA_FIFOMode_Enable;   // FIFO使能
  DMA_InitStructure.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;// 全FIFO
  DMA_InitStructure.DMA_MemoryBurst = DMA_MemoryBurst_INC4;// 外设突发4次传输
  DMA_InitStructure.DMA_PeripheralBurst = DMA_PeripheralBurst_INC4;// 存储器突发4次传输
  DMA_Init(SD_DMA_STREAM, &DMA_InitStructure);
	DMA_FlowControllerConfig(SD_DMA_STREAM,DMA_FlowCtrl_Peripheral);// 初始化DMA Stream

  DMA_ClearFlag(SD_DMA_STREAM, SD_DMA_ALL_FLAGS);

  DMA_Cmd(SD_DMA_STREAM ,ENABLE);// 使能DMA传输

}


// 读SD卡
// buf: 数据缓存区
// sector: 扇区地址
// cnt: 扇区数
// 返回值: 错误代码; 0, 正常; 其他, 错误代码;
u8 SD_ReadDisk(u8*buf,u32 sector,u8 cnt)
{
	u8 sta=SD_OK;
	long long lsector=sector;
	u8 n;
	lsector<<=9;
	if((u32)buf%4!=0)
	{
	 	for(n=0;n<cnt;n++)
		{
		 	sta=SD_ReadBlock(SDIO_DATA_BUFFER,lsector+512*n,512);// 单个sector读取
			memcpy(buf,SDIO_DATA_BUFFER,512);
			buf+=512;
		}
	}else
	{
		if(cnt==1)sta=SD_ReadBlock(buf,lsector,512);    	// 单个sector读取
		else sta=SD_ReadMultiBlocks(buf,lsector,512,cnt);// 多个sector读取
	}
	return sta;
}
// 写SD卡
// buf: 数据缓存区
// sector: 扇区地址
// cnt: 扇区数
// 返回值: 错误代码; 0, 正常; 其他, 错误代码;
u8 SD_WriteDisk(u8*buf,u32 sector,u8 cnt)
{
	u8 sta=SD_OK;
	u8 n;
	long long lsector=sector;
	lsector<<=9;
	if((u32)buf%4!=0)
	{
	 	for(n=0;n<cnt;n++)
		{
			memcpy(SDIO_DATA_BUFFER,buf,512);
		 	sta=SD_WriteBlock(SDIO_DATA_BUFFER,lsector+512*n,512);// 单个sector写入
			buf+=512;
		}
	}else
	{
		if(cnt==1)sta=SD_WriteBlock(buf,lsector,512);    	// 单个sector写入
		else sta=SD_WriteMultiBlocks(buf,lsector,512,cnt);	// 多个sector写入
	}
	return sta;
}






