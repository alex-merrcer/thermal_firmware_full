#include "sdio_sdcard.h"
#include "string.h"	 
#include "sys.h"	 
#include "usart.h"	 
//////////////////////////////////////////////////////////////////////////////////	 

//STM32F407瀵偓閸欐垶婢?
//SDIO 妞瑰崬濮╂禒锝囩垳	   

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


static u8 CardType=SDIO_STD_CAPACITY_SD_CARD_V1_1;		//SD閸楋紕琚崹瀣剁礄姒涙顓绘稉?.x閸椻槄绱?
static u32 CSD_Tab[4],CID_Tab[4],RCA=0;					//SD閸楊搲SD,CID娴犮儱寮烽惄绋款嚠閸︽澘娼?RCA)閺佺増宓?
static u8 DeviceMode=SD_DMA_MODE;		   				//瀹搞儰缍斿Ο鈥崇础,濞夈劍鍓?瀹搞儰缍斿Ο鈥崇础韫囧懘銆忛柅姘崇箖SD_SetDeviceMode,閸氬孩澧犵粻妤佹殶.鏉╂瑩鍣烽崣顏呮Ц鐎规矮绠熸稉鈧稉顏堢帛鐠併倗娈戝Ο鈥崇础(SD_DMA_MODE)
static u8 StopCondition=0; 								//閺勵垰鎯侀崣鎴︹偓浣镐粻濮濐澀绱舵潏鎾寸垼韫囨ぞ缍?DMA婢舵艾娼＄拠璇插晸閻ㄥ嫭妞傞崐娆戞暏閸? 
volatile SD_Error TransferError=SD_OK;					//閺佺増宓佹导鐘虹翻闁挎瑨顕ら弽鍥х箶,DMA鐠囪鍟撻弮鏈靛▏閻?    
volatile u8 TransferEnd=0;								//娴肩姾绶紒鎾存将閺嶅洤绻?DMA鐠囪鍟撻弮鏈靛▏閻?
SD_CardInfo SDCardInfo;									//SD閸椻€蹭繆閹?

//SD_ReadDisk/SD_WriteDisk閸戣姤鏆熸稉鎾舵暏buf,瑜版捁绻栨稉銈勯嚋閸戣姤鏆熼惃鍕殶閹诡喚绱︾€涙ê灏崷鏉挎絻娑撳秵妲?鐎涙濡€靛綊缍堥惃鍕閸?
//闂団偓鐟曚胶鏁ら崚鎷岊嚉閺佹壆绮?绾喕绻氶弫鐗堝祦缂傛挸鐡ㄩ崠鍝勬勾閸р偓閺?鐎涙濡€靛綊缍堥惃?
__align(4) u8 SDIO_DATA_BUFFER[512];						  
 
 
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

//閸掓繂顫愰崠鏈閸?
//鏉╂柨娲栭崐?闁挎瑨顕ゆ禒锝囩垳;(0,閺冪娀鏁婄拠?
SD_Error SD_Init(void)
{
 	GPIO_InitTypeDef  GPIO_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;
	
	SD_Error errorstatus=SD_OK;	 
  u8 clkdiv=0;
	
  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC|RCC_AHB1Periph_GPIOD|RCC_AHB1Periph_DMA2, ENABLE);//娴ｈ儻鍏楪PIOC,GPIOD DMA2閺冨爼鎸?
	
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_SDIO, ENABLE);//SDIO閺冨爼鎸撴担鑳厴
	
	RCC_APB2PeriphResetCmd(RCC_APB2Periph_SDIO, ENABLE);//SDIO婢跺秳缍?
	
	
  GPIO_InitStructure.GPIO_Pin =GPIO_Pin_8|GPIO_Pin_9|GPIO_Pin_10|GPIO_Pin_11|GPIO_Pin_12; 	//PC8,9,10,11,12婢跺秶鏁ら崝鐔诲厴鏉堟挸鍤?
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;//婢跺秶鏁ら崝鐔诲厴
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;//100M
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;//娑撳﹥濯?
  GPIO_Init(GPIOC, &GPIO_InitStructure);// PC8,9,10,11,12婢跺秶鏁ら崝鐔诲厴鏉堟挸鍤?

	
	GPIO_InitStructure.GPIO_Pin =GPIO_Pin_2;
  GPIO_Init(GPIOD, &GPIO_InitStructure);//PD2婢跺秶鏁ら崝鐔诲厴鏉堟挸鍤?
	
	 //瀵洝鍓兼径宥囨暏閺勭姴鐨犵拋鍓х枂
	GPIO_PinAFConfig(GPIOC,GPIO_PinSource8,GPIO_AF_SDIO); //PC8,AF12
  GPIO_PinAFConfig(GPIOC,GPIO_PinSource9,GPIO_AF_SDIO);
  GPIO_PinAFConfig(GPIOC,GPIO_PinSource10,GPIO_AF_SDIO);
  GPIO_PinAFConfig(GPIOC,GPIO_PinSource11,GPIO_AF_SDIO);
  GPIO_PinAFConfig(GPIOC,GPIO_PinSource12,GPIO_AF_SDIO);	
  GPIO_PinAFConfig(GPIOD,GPIO_PinSource2,GPIO_AF_SDIO);	
	
	RCC_APB2PeriphResetCmd(RCC_APB2Periph_SDIO, DISABLE);//SDIO缂佹挻娼径宥勭秴
		
 	//SDIO婢舵牞顔曠€靛嫬鐡ㄩ崳銊啎缂冾喕璐熸妯款吇閸?			   
	SDIO_Register_Deinit();
	
  NVIC_InitStructure.NVIC_IRQChannel = SDIO_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority=0;//閹躲垹宕版导妯哄帥缁?
	NVIC_InitStructure.NVIC_IRQChannelSubPriority =0;		//鐎涙劒绱崗鍫㈤獓3
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;			//IRQ闁岸浜炬担鑳厴
	NVIC_Init(&NVIC_InitStructure);	//閺嶈宓侀幐鍥х暰閻ㄥ嫬寮弫鏉垮灥婵瀵睼IC鐎靛嫬鐡ㄩ崳銊ｂ偓?
	
   	errorstatus=SD_PowerON();			//SD閸椻€茬瑐閻?
 	if(errorstatus==SD_OK)errorstatus=SD_InitializeCards();			//閸掓繂顫愰崠鏈閸?													  
  	if(errorstatus==SD_OK)errorstatus=SD_GetCardInfo(&SDCardInfo);	//閼惧嘲褰囬崡鈥蹭繆閹?
 	if(errorstatus==SD_OK)errorstatus=SD_SelectDeselect((u32)(SDCardInfo.RCA<<16));//闁鑵慡D閸?  
   	#if (SDIO_DEBUG_FORCE_1BIT == 0)
   	if(errorstatus==SD_OK)errorstatus=SD_EnableWideBusOperation(SDIO_BusWide_4b);
#endif	//4娴ｅ秴顔旀惔?婵″倹鐏夐弰鐤C閸?閸掓瑤绗夐懗鐣屾暏4娴ｅ秵膩瀵?
  	if((errorstatus==SD_OK)||(SDIO_MULTIMEDIA_CARD==CardType))
	{  		    
		if(SDCardInfo.CardType==SDIO_STD_CAPACITY_SD_CARD_V1_1||SDCardInfo.CardType==SDIO_STD_CAPACITY_SD_CARD_V2_0)
		{
			clkdiv=SDIO_TRANSFER_CLK_DIV+2;	//V1.1/V2.0閸椻槄绱濈拋鍓х枂閺堚偓妤?8/4=12Mhz
		}else clkdiv=SDIO_TRANSFER_CLK_DIV;	//SDHC缁涘鍙炬禒鏍у幢閿涘矁顔曠純顔芥付妤?8/2=24Mhz
		SDIO_Clock_Set(clkdiv);	//鐠佸墽鐤嗛弮鍫曟寭妫版垹宸?SDIO閺冨爼鎸撶拋锛勭暬閸忣剙绱?SDIO_CK閺冨爼鎸?SDIOCLK/[clkdiv+2];閸忔湹鑵?SDIOCLK閸ュ搫鐣炬稉?8Mhz 
		//errorstatus=SD_SetDeviceMode(SD_DMA_MODE);	//鐠佸墽鐤嗘稉绡婱A濡€崇础
		errorstatus=SD_SetDeviceMode(SD_POLLING_MODE);//鐠佸墽鐤嗘稉鐑樼叀鐠囥垺膩瀵?
 	}
	return errorstatus;		 
}
//SDIO閺冨爼鎸撻崚婵嗩潗閸栨牞顔曠純?
//clkdiv:閺冨爼鎸撻崚鍡涱暥缁粯鏆?
//CK閺冨爼鎸?SDIOCLK/[clkdiv+2];(SDIOCLK閺冨爼鎸撻崶鍝勭暰娑?8Mhz)
void SDIO_Clock_Set(u8 clkdiv)
{
	u32 tmpreg=SDIO->CLKCR; 
  	tmpreg&=0XFFFFFF00; 
 	tmpreg|=clkdiv;   
	SDIO->CLKCR=tmpreg;
} 


//閸椻€茬瑐閻?
//閺屻儴顕楅幍鈧張濉朌IO閹恒儱褰涙稉濠勬畱閸椔ゎ啎婢?楠炶埖鐓＄拠銏犲従閻㈤潧甯囬崪宀勫帳缂冾喗妞傞柦?
//鏉╂柨娲栭崐?闁挎瑨顕ゆ禒锝囩垳;(0,閺冪娀鏁婄拠?
SD_Error SD_PowerON(void)
{
 	u8 i=0;
	SD_Error errorstatus=SD_OK;
	u32 response=0,count=0,validvoltage=0;
	u32 SDType=SD_STD_CAPACITY;
	
	 /*閸掓繂顫愰崠鏍ㄦ閻ㄥ嫭妞傞柦鐔剁瑝閼宠棄銇囨禍?00KHz*/ 
  SDIO_InitStructure.SDIO_ClockDiv = SDIO_INIT_CLK_DIV;	/* HCLK = 72MHz, SDIOCLK = 72MHz, SDIO_CK = HCLK/(178 + 2) = 400 KHz */
  SDIO_InitStructure.SDIO_ClockEdge = SDIO_ClockEdge_Rising;
  SDIO_InitStructure.SDIO_ClockBypass = SDIO_ClockBypass_Disable;  //娑撳秳濞囬悽鈺瀥pass濡€崇础閿涘瞼娲块幒銉ф暏HCLK鏉╂稖顢戦崚鍡涱暥瀵版鍩孲DIO_CK
  SDIO_InitStructure.SDIO_ClockPowerSave = SDIO_ClockPowerSave_Disable;	// 缁屾椽妫介弮鏈电瑝閸忔娊妫撮弮鍫曟寭閻㈠灚绨?
  SDIO_InitStructure.SDIO_BusWide = SDIO_BusWide_1b;	 				//1娴ｅ秵鏆熼幑顔惧殠
  SDIO_InitStructure.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Disable;//绾兛娆㈠ù?
  SDIO_Init(&SDIO_InitStructure);

	SDIO_SetPowerState(SDIO_PowerState_ON);	//娑撳﹦鏁搁悩鑸碘偓?瀵偓閸氼垰宕遍弮鍫曟寭   
  SDIO->CLKCR|=1<<8;			//SDIOCK娴ｈ儻鍏? 
 
 	for(i=0;i<74;i++)
	{
 
		SDIO_CmdInitStructure.SDIO_Argument = 0x0;//閸欐垿鈧竼MD0鏉╂稑鍙咺DLE STAGE濡€崇础閸涙垝鎶?
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_GO_IDLE_STATE; //cmd0
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_No;  //閺冪姴鎼锋惔?
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;  //閸掓PSM閸︺劌绱戞慨瀣絺闁礁鎳℃禒銈勭閸撳秶鐡戝鍛殶閹诡喕绱舵潏鎾剁波閺夌喆鈧?
    SDIO_SendCommand(&SDIO_CmdInitStructure);	  		//閸愭瑥鎳℃禒銈堢箻閸涙垝鎶ょ€靛嫬鐡ㄩ崳?
		
		errorstatus=CmdError();
		
		if(errorstatus==SD_OK)break;
 	}
 	if(errorstatus)return errorstatus;//鏉╂柨娲栭柨娆掝嚖閻樿埖鈧?
	
  SDIO_CmdInitStructure.SDIO_Argument = SD_CHECK_PATTERN;	//閸欐垿鈧竼MD8,閻厼鎼锋惔?濡偓閺岊櫃D閸椻剝甯撮崣锝囧閹?
  SDIO_CmdInitStructure.SDIO_CmdIndex = SDIO_SEND_IF_COND;	//cmd8
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;	 //r7
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;			 //閸忔娊妫寸粵澶婄窡娑擃厽鏌?
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);
	
  errorstatus=CmdResp7Error();						//缁涘绶烺7閸濆秴绨?
	
 	if(errorstatus==SD_OK) 								//R7閸濆秴绨插锝呯埗
	{
		CardType=SDIO_STD_CAPACITY_SD_CARD_V2_0;		//SD 2.0閸?
		SDType=SD_HIGH_CAPACITY;			   			//妤傛ê顔愰柌蹇撳幢
	}
	  
	  SDIO_CmdInitStructure.SDIO_Argument = 0x00;//閸欐垿鈧竼MD55,閻厼鎼锋惔?
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);		//閸欐垿鈧竼MD55,閻厼鎼锋惔? 
	
	 errorstatus=CmdResp1Error(SD_CMD_APP_CMD); 		 	//缁涘绶烺1閸濆秴绨?  
	
	if(errorstatus==SD_OK)//SD2.0/SD 1.1,閸氾箑鍨稉绡桵C閸?
	{																  
		//SD閸?閸欐垿鈧竸CMD41 SD_APP_OP_COND,閸欏倹鏆熸稉?0x80100000 
		while((!validvoltage)&&(count<SD_MAX_VOLT_TRIAL))
		{	   										   
		  SDIO_CmdInitStructure.SDIO_Argument = 0x00;//閸欐垿鈧竼MD55,閻厼鎼锋惔?
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;	  //CMD55
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);			//閸欐垿鈧竼MD55,閻厼鎼锋惔? 
			
			errorstatus=CmdResp1Error(SD_CMD_APP_CMD); 	 	//缁涘绶烺1閸濆秴绨? 
			
 			if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖

      //acmd41閿涘苯鎳℃禒銈呭棘閺佹壆鏁遍弨顖涘瘮閻ㄥ嫮鏁搁崢瀣瘱閸ユ潙寮稨CS娴ｅ秶绮嶉幋鎰剁礉HCS娴ｅ秶鐤嗘稉鈧弶銉ュ隘閸掑棗宕遍弰鐤璂Sc鏉╂ɑ妲竤dhc
      SDIO_CmdInitStructure.SDIO_Argument = SD_VOLTAGE_WINDOW_SD | SDType;	//閸欐垿鈧竸CMD41,閻厼鎼锋惔?
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SD_APP_OP_COND;
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;  //r3
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);
			
			errorstatus=CmdResp3Error(); 					//缁涘绶烺3閸濆秴绨?  
			
 			if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖 
			response=SDIO->RESP1;;			   				//瀵版鍩岄崫宥呯安
			validvoltage=(((response>>31)==1)?1:0);			//閸掋倖鏌嘢D閸椻€茬瑐閻㈠灚妲搁崥锕€鐣幋?
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
 	}else//MMC閸?
	{
		//MMC閸?閸欐垿鈧竼MD1 SDIO_SEND_OP_COND,閸欏倹鏆熸稉?0x80FF8000 
		while((!validvoltage)&&(count<SD_MAX_VOLT_TRIAL))
		{	   										   				   
			SDIO_CmdInitStructure.SDIO_Argument = SD_VOLTAGE_WINDOW_MMC;//閸欐垿鈧竼MD1,閻厼鎼锋惔?   
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_OP_COND;
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;  //r3
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);
			
			errorstatus=CmdResp3Error(); 					//缁涘绶烺3閸濆秴绨?  
			
 			if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖  
			response=SDIO->RESP1;;			   				//瀵版鍩岄崫宥呯安
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
//SD閸?Power OFF
//鏉╂柨娲栭崐?闁挎瑨顕ゆ禒锝囩垳;(0,閺冪娀鏁婄拠?
SD_Error SD_PowerOFF(void)
{
 
  SDIO_SetPowerState(SDIO_PowerState_OFF);//SDIO閻㈠灚绨崗鎶芥４,閺冨爼鎸撻崑婊勵剾	

  return SD_OK;	  
}   
//閸掓繂顫愰崠鏍ㄥ閺堝娈戦崡?楠炴儼顔€閸椔ょ箻閸忋儱姘ㄧ紒顏嗗Ц閹?
//鏉╂柨娲栭崐?闁挎瑨顕ゆ禒锝囩垳
SD_Error SD_InitializeCards(void)
{
 	SD_Error errorstatus=SD_OK;
	u16 rca = 0x01;
	
  if (SDIO_GetPowerState() == SDIO_PowerState_OFF)	//濡偓閺屻儳鏁稿┃鎰Ц閹?绾喕绻氭稉杞扮瑐閻㈢數濮搁幀?
  {
    errorstatus = SD_REQUEST_NOT_APPLICABLE;
    return(errorstatus);
  }

 	if(SDIO_SECURE_DIGITAL_IO_CARD!=CardType)			//闂堟炕ECURE_DIGITAL_IO_CARD
	{
		SDIO_CmdInitStructure.SDIO_Argument = 0x0;//閸欐垿鈧竼MD2,閸欐牕绶盋ID,闂€鍨惙鎼?
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_ALL_SEND_CID;
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Long;
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);//閸欐垿鈧竼MD2,閸欐牕绶盋ID,闂€鍨惙鎼?
		
		errorstatus=CmdResp2Error(); 					//缁涘绶烺2閸濆秴绨?
		
		if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖		 
		
 		CID_Tab[0]=SDIO->RESP1;
		CID_Tab[1]=SDIO->RESP2;
		CID_Tab[2]=SDIO->RESP3;
		CID_Tab[3]=SDIO->RESP4;
	}
	if((SDIO_STD_CAPACITY_SD_CARD_V1_1==CardType)||(SDIO_STD_CAPACITY_SD_CARD_V2_0==CardType)||(SDIO_SECURE_DIGITAL_IO_COMBO_CARD==CardType)||(SDIO_HIGH_CAPACITY_SD_CARD==CardType))//閸掋倖鏌囬崡锛勮閸?
	{
		SDIO_CmdInitStructure.SDIO_Argument = 0x00;//閸欐垿鈧竼MD3,閻厼鎼锋惔?
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_REL_ADDR;	//cmd3
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short; //r6
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);	//閸欐垿鈧竼MD3,閻厼鎼锋惔?
		
		errorstatus=CmdResp6Error(SD_CMD_SET_REL_ADDR,&rca);//缁涘绶烺6閸濆秴绨?
		
		if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖		    
	}   
    if (SDIO_MULTIMEDIA_CARD==CardType)
    {

		  SDIO_CmdInitStructure.SDIO_Argument = (u32)(rca<<16);//閸欐垿鈧竼MD3,閻厼鎼锋惔?
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_REL_ADDR;	//cmd3
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short; //r6
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);	//閸欐垿鈧竼MD3,閻厼鎼锋惔?	
			
      errorstatus=CmdResp2Error(); 					//缁涘绶烺2閸濆秴绨?  
			
		  if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖	 
    }
	if (SDIO_SECURE_DIGITAL_IO_CARD!=CardType)			//闂堟炕ECURE_DIGITAL_IO_CARD
	{
		RCA = rca;
		
    SDIO_CmdInitStructure.SDIO_Argument = (uint32_t)(rca << 16);//閸欐垿鈧竼MD9+閸楊摌CA,閸欐牕绶盋SD,闂€鍨惙鎼?
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_CSD;
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Long;
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);
		
		errorstatus=CmdResp2Error(); 					//缁涘绶烺2閸濆秴绨?  
		if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖		    
  		
		CSD_Tab[0]=SDIO->RESP1;
	  CSD_Tab[1]=SDIO->RESP2;
		CSD_Tab[2]=SDIO->RESP3;						
		CSD_Tab[3]=SDIO->RESP4;					    
	}
	return SD_OK;//閸椻€冲灥婵瀵查幋鎰
} 
//瀵版鍩岄崡鈥蹭繆閹?
//cardinfo:閸椻€蹭繆閹垰鐡ㄩ崒銊ュ隘
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?
SD_Error SD_GetCardInfo(SD_CardInfo *cardinfo)
{
 	SD_Error errorstatus=SD_OK;
	u8 tmp=0;	   
	cardinfo->CardType=(u8)CardType; 				//閸楋紕琚崹?
	cardinfo->RCA=(u16)RCA;							//閸楊摌CA閸?
	tmp=(u8)((CSD_Tab[0]&0xFF000000)>>24);
	cardinfo->SD_csd.CSDStruct=(tmp&0xC0)>>6;		//CSD缂佹挻鐎?
	cardinfo->SD_csd.SysSpecVersion=(tmp&0x3C)>>2;	//2.0閸楀繗顔呮潻妯荤梾鐎规矮绠熸潻娆撳劥閸?娑撹桨绻氶悾?,鎼存棁顕氶弰顖氭倵缂侇厼宕楃拋顔肩暰娑斿娈?
	cardinfo->SD_csd.Reserved1=tmp&0x03;			//2娑擃亙绻氶悾娆庣秴  
	tmp=(u8)((CSD_Tab[0]&0x00FF0000)>>16);			//缁?娑擃亜鐡ч懞?
	cardinfo->SD_csd.TAAC=tmp;				   		//閺佺増宓佺拠缁樻闂?
	tmp=(u8)((CSD_Tab[0]&0x0000FF00)>>8);	  		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_csd.NSAC=tmp;		  				//閺佺増宓佺拠缁樻闂?
	tmp=(u8)(CSD_Tab[0]&0x000000FF);				//缁?娑擃亜鐡ч懞?
	cardinfo->SD_csd.MaxBusClkFrec=tmp;		  		//娴肩姾绶柅鐔峰	   
	tmp=(u8)((CSD_Tab[1]&0xFF000000)>>24);			//缁?娑擃亜鐡ч懞?
	cardinfo->SD_csd.CardComdClasses=tmp<<4;    	//閸椻剝瀵氭禒銈囪妤傛ê娲撴担?
	tmp=(u8)((CSD_Tab[1]&0x00FF0000)>>16);	 		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_csd.CardComdClasses|=(tmp&0xF0)>>4;//閸椻剝瀵氭禒銈囪娴ｅ骸娲撴担?
	cardinfo->SD_csd.RdBlockLen=tmp&0x0F;	    	//閺堚偓婢堆嗩嚢閸欐牗鏆熼幑顕€鏆辨惔?
	tmp=(u8)((CSD_Tab[1]&0x0000FF00)>>8);			//缁?娑擃亜鐡ч懞?
	cardinfo->SD_csd.PartBlockRead=(tmp&0x80)>>7;	//閸忎浇顔忛崚鍡楁健鐠?
	cardinfo->SD_csd.WrBlockMisalign=(tmp&0x40)>>6;	//閸愭瑥娼￠柨娆庣秴
	cardinfo->SD_csd.RdBlockMisalign=(tmp&0x20)>>5;	//鐠囪娼￠柨娆庣秴
	cardinfo->SD_csd.DSRImpl=(tmp&0x10)>>4;
	cardinfo->SD_csd.Reserved2=0; 					//娣囨繄鏆€
 	if((CardType==SDIO_STD_CAPACITY_SD_CARD_V1_1)||(CardType==SDIO_STD_CAPACITY_SD_CARD_V2_0)||(SDIO_MULTIMEDIA_CARD==CardType))//閺嶅洤鍣?.1/2.0閸?MMC閸?
	{
		cardinfo->SD_csd.DeviceSize=(tmp&0x03)<<10;	//C_SIZE(12娴?
	 	tmp=(u8)(CSD_Tab[1]&0x000000FF); 			//缁?娑擃亜鐡ч懞?
		cardinfo->SD_csd.DeviceSize|=(tmp)<<2;
 		tmp=(u8)((CSD_Tab[2]&0xFF000000)>>24);		//缁?娑擃亜鐡ч懞?
		cardinfo->SD_csd.DeviceSize|=(tmp&0xC0)>>6;
 		cardinfo->SD_csd.MaxRdCurrentVDDMin=(tmp&0x38)>>3;
		cardinfo->SD_csd.MaxRdCurrentVDDMax=(tmp&0x07);
 		tmp=(u8)((CSD_Tab[2]&0x00FF0000)>>16);		//缁?娑擃亜鐡ч懞?
		cardinfo->SD_csd.MaxWrCurrentVDDMin=(tmp&0xE0)>>5;
		cardinfo->SD_csd.MaxWrCurrentVDDMax=(tmp&0x1C)>>2;
		cardinfo->SD_csd.DeviceSizeMul=(tmp&0x03)<<1;//C_SIZE_MULT
 		tmp=(u8)((CSD_Tab[2]&0x0000FF00)>>8);	  	//缁?0娑擃亜鐡ч懞?
		cardinfo->SD_csd.DeviceSizeMul|=(tmp&0x80)>>7;
 		cardinfo->CardCapacity=(cardinfo->SD_csd.DeviceSize+1);//鐠侊紕鐣婚崡鈥愁啇闁?
		cardinfo->CardCapacity*=(1<<(cardinfo->SD_csd.DeviceSizeMul+2));
		cardinfo->CardBlockSize=1<<(cardinfo->SD_csd.RdBlockLen);//閸ф銇囩亸?
		cardinfo->CardCapacity*=cardinfo->CardBlockSize;
	}else if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)	//妤傛ê顔愰柌蹇撳幢
	{
 		tmp=(u8)(CSD_Tab[1]&0x000000FF); 		//缁?娑擃亜鐡ч懞?
		cardinfo->SD_csd.DeviceSize=(tmp&0x3F)<<16;//C_SIZE
 		tmp=(u8)((CSD_Tab[2]&0xFF000000)>>24); 	//缁?娑擃亜鐡ч懞?
 		cardinfo->SD_csd.DeviceSize|=(tmp<<8);
 		tmp=(u8)((CSD_Tab[2]&0x00FF0000)>>16);	//缁?娑擃亜鐡ч懞?
 		cardinfo->SD_csd.DeviceSize|=(tmp);
 		tmp=(u8)((CSD_Tab[2]&0x0000FF00)>>8); 	//缁?0娑擃亜鐡ч懞?
 		cardinfo->CardCapacity=(long long)(cardinfo->SD_csd.DeviceSize+1)*512*1024;//鐠侊紕鐣婚崡鈥愁啇闁?
		cardinfo->CardBlockSize=512; 			//閸ф銇囩亸蹇撴祼鐎规矮璐?12鐎涙濡?
	}	  
	cardinfo->SD_csd.EraseGrSize=(tmp&0x40)>>6;
	cardinfo->SD_csd.EraseGrMul=(tmp&0x3F)<<1;	   
	tmp=(u8)(CSD_Tab[2]&0x000000FF);			//缁?1娑擃亜鐡ч懞?
	cardinfo->SD_csd.EraseGrMul|=(tmp&0x80)>>7;
	cardinfo->SD_csd.WrProtectGrSize=(tmp&0x7F);
 	tmp=(u8)((CSD_Tab[3]&0xFF000000)>>24);		//缁?2娑擃亜鐡ч懞?
	cardinfo->SD_csd.WrProtectGrEnable=(tmp&0x80)>>7;
	cardinfo->SD_csd.ManDeflECC=(tmp&0x60)>>5;
	cardinfo->SD_csd.WrSpeedFact=(tmp&0x1C)>>2;
	cardinfo->SD_csd.MaxWrBlockLen=(tmp&0x03)<<2;	 
	tmp=(u8)((CSD_Tab[3]&0x00FF0000)>>16);		//缁?3娑擃亜鐡ч懞?
	cardinfo->SD_csd.MaxWrBlockLen|=(tmp&0xC0)>>6;
	cardinfo->SD_csd.WriteBlockPaPartial=(tmp&0x20)>>5;
	cardinfo->SD_csd.Reserved3=0;
	cardinfo->SD_csd.ContentProtectAppli=(tmp&0x01);  
	tmp=(u8)((CSD_Tab[3]&0x0000FF00)>>8);		//缁?4娑擃亜鐡ч懞?
	cardinfo->SD_csd.FileFormatGrouop=(tmp&0x80)>>7;
	cardinfo->SD_csd.CopyFlag=(tmp&0x40)>>6;
	cardinfo->SD_csd.PermWrProtect=(tmp&0x20)>>5;
	cardinfo->SD_csd.TempWrProtect=(tmp&0x10)>>4;
	cardinfo->SD_csd.FileFormat=(tmp&0x0C)>>2;
	cardinfo->SD_csd.ECC=(tmp&0x03);  
	tmp=(u8)(CSD_Tab[3]&0x000000FF);			//缁?5娑擃亜鐡ч懞?
	cardinfo->SD_csd.CSD_CRC=(tmp&0xFE)>>1;
	cardinfo->SD_csd.Reserved4=1;		 
	tmp=(u8)((CID_Tab[0]&0xFF000000)>>24);		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.ManufacturerID=tmp;		    
	tmp=(u8)((CID_Tab[0]&0x00FF0000)>>16);		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.OEM_AppliID=tmp<<8;	  
	tmp=(u8)((CID_Tab[0]&0x000000FF00)>>8);		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.OEM_AppliID|=tmp;	    
	tmp=(u8)(CID_Tab[0]&0x000000FF);			//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdName1=tmp<<24;				  
	tmp=(u8)((CID_Tab[1]&0xFF000000)>>24); 		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdName1|=tmp<<16;	  
	tmp=(u8)((CID_Tab[1]&0x00FF0000)>>16);	   	//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdName1|=tmp<<8;		 
	tmp=(u8)((CID_Tab[1]&0x0000FF00)>>8);		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdName1|=tmp;		   
	tmp=(u8)(CID_Tab[1]&0x000000FF);	  		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdName2=tmp;			  
	tmp=(u8)((CID_Tab[2]&0xFF000000)>>24); 		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdRev=tmp;		 
	tmp=(u8)((CID_Tab[2]&0x00FF0000)>>16);		//缁?娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdSN=tmp<<24;	   
	tmp=(u8)((CID_Tab[2]&0x0000FF00)>>8); 		//缁?0娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdSN|=tmp<<16;	   
	tmp=(u8)(CID_Tab[2]&0x000000FF);   			//缁?1娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdSN|=tmp<<8;		   
	tmp=(u8)((CID_Tab[3]&0xFF000000)>>24); 		//缁?2娑擃亜鐡ч懞?
	cardinfo->SD_cid.ProdSN|=tmp;			     
	tmp=(u8)((CID_Tab[3]&0x00FF0000)>>16);	 	//缁?3娑擃亜鐡ч懞?
	cardinfo->SD_cid.Reserved1|=(tmp&0xF0)>>4;
	cardinfo->SD_cid.ManufactDate=(tmp&0x0F)<<8;    
	tmp=(u8)((CID_Tab[3]&0x0000FF00)>>8);		//缁?4娑擃亜鐡ч懞?
	cardinfo->SD_cid.ManufactDate|=tmp;		 	  
	tmp=(u8)(CID_Tab[3]&0x000000FF);			//缁?5娑擃亜鐡ч懞?
	cardinfo->SD_cid.CID_CRC=(tmp&0xFE)>>1;
	cardinfo->SD_cid.Reserved2=1;	 
	return errorstatus;
}
//鐠佸墽鐤哠DIO閹崵鍤庣€硅棄瀹?MMC閸椻€茬瑝閺€顖涘瘮4bit濡€崇础)
//wmode:娴ｅ秴顔斿Ο鈥崇础.0,1娴ｅ秵鏆熼幑顔碱啍鎼?1,4娴ｅ秵鏆熼幑顔碱啍鎼?2,8娴ｅ秵鏆熼幑顔碱啍鎼?
//鏉╂柨娲栭崐?SD閸楋繝鏁婄拠顖滃Ц閹?

//鐠佸墽鐤哠DIO閹崵鍤庣€硅棄瀹?MMC閸椻€茬瑝閺€顖涘瘮4bit濡€崇础)
//   @arg SDIO_BusWide_8b: 8-bit data transfer (Only for MMC)
//   @arg SDIO_BusWide_4b: 4-bit data transfer
//   @arg SDIO_BusWide_1b: 1-bit data transfer (姒涙顓?
//鏉╂柨娲栭崐?SD閸楋繝鏁婄拠顖滃Ц閹?


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
		 if (SDIO_BusWide_8b == WideMode)   //2.0 sd娑撳秵鏁幐?bits
    {
      errorstatus = SD_UNSUPPORTED_FEATURE;
      return(errorstatus);
    }
 		else   
		{
			errorstatus=SDEnWideBus(WideMode);
 			if(SD_OK==errorstatus)
			{
				SDIO->CLKCR&=~(3<<11);		//濞撳懘娅庢稊瀣閻ㄥ嫪缍呯€瑰€燁啎缂?   
				SDIO->CLKCR|=WideMode;//1娴?4娴ｅ秵鈧崵鍤庣€硅棄瀹?
				SDIO->CLKCR|=0<<14;			//娑撳秴绱戦崥顖溾€栨禒鑸电ウ閹貉冨煑
			}
		}  
	}
	return errorstatus; 
}
//鐠佸墽鐤哠D閸椻€充紣娴ｆ粍膩瀵?
//Mode:
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?
SD_Error SD_SetDeviceMode(u32 Mode)
{
	SD_Error errorstatus = SD_OK;
 	if((Mode==SD_DMA_MODE)||(Mode==SD_POLLING_MODE))DeviceMode=Mode;
	else errorstatus=SD_INVALID_PARAMETER;
	return errorstatus;	    
}
//闁宕?
//閸欐垿鈧竼MD7,闁瀚ㄩ惄绋款嚠閸︽澘娼?rca)娑撶ddr閻ㄥ嫬宕?閸欐牗绉烽崗鏈电铂閸?婵″倹鐏夋稉?,閸掓瑩鍏樻稉宥夆偓澶嬪.
//addr:閸楋紕娈慠CA閸︽澘娼?
SD_Error SD_SelectDeselect(u32 addr)
{

  SDIO_CmdInitStructure.SDIO_Argument =  addr;//閸欐垿鈧竼MD7,闁瀚ㄩ崡?閻厼鎼锋惔?
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEL_DESEL_CARD;
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);//閸欐垿鈧竼MD7,闁瀚ㄩ崡?閻厼鎼锋惔?
	
 	return CmdResp1Error(SD_CMD_SEL_DESEL_CARD);	  
}
//SD閸椔ゎ嚢閸欐牔绔存稉顏勬健 
//buf:鐠囩粯鏆熼幑顔剧处鐎涙ê灏?韫囧懘銆?鐎涙濡€靛綊缍?!)
//addr:鐠囪褰囬崷鏉挎絻
//blksize:閸ф銇囩亸?
SD_Error SD_ReadBlock(u8 *buf,long long addr,u16 blksize)
{	  
	SD_Error errorstatus=SD_OK;
	u8 power;
  u32 count=0,*tempbuff=(u32*)buf;//鏉烆剚宕叉稉绨?2閹稿洭鎷?
	u32 timeout=SDIO_DATATIMEOUT;   
  if(NULL==buf)
		return SD_INVALID_PARAMETER; 
  SDIO->DCTRL=0x0;	//閺佺増宓侀幒褍鍩楃€靛嫬鐡ㄩ崳銊︾闂?閸忕煱MA) 
  
	if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)//婢堆冾啇闁插繐宕?
	{
		blksize=512;
		addr>>=9;
	}   
  	SDIO_DataInitStructure.SDIO_DataBlockSize= SDIO_DataBlockSize_1b ;//濞撳懘娅嶥PSM閻樿埖鈧焦婧€闁板秶鐤?
	  SDIO_DataInitStructure.SDIO_DataLength= 0 ;
	  SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	  SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	  SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	  SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
    SDIO_DataConfig(&SDIO_DataInitStructure);
	
	
	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;//閸楋繝鏀ｆ禍?
	if((blksize>0)&&(blksize<=2048)&&((blksize&(blksize-1))==0))
	{
		power=convert_from_bytes_to_power_of_two(blksize);	
		
   
		SDIO_CmdInitStructure.SDIO_Argument =  blksize;
    SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN;
    SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
    SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
    SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&SDIO_CmdInitStructure);//閸欐垿鈧竼MD16+鐠佸墽鐤嗛弫鐗堝祦闂€鍨娑撶lksize,閻厼鎼锋惔?
		
		
		errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);	//缁涘绶烺1閸濆秴绨?
		
		if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖	
		
	}else return SD_INVALID_PARAMETER;	  	 
	
	  SDIO_DataInitStructure.SDIO_DataBlockSize= power<<4 ;//濞撳懘娅嶥PSM閻樿埖鈧焦婧€闁板秶鐤?
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
    SDIO_SendCommand(&SDIO_CmdInitStructure);//閸欐垿鈧竼MD17+娴犲穬ddr閸︽澘娼冮崙楦款嚢閸欐牗鏆熼幑?閻厼鎼锋惔?
	
	errorstatus=CmdResp1Error(SD_CMD_READ_SINGLE_BLOCK);//缁涘绶烺1閸濆秴绨?  
	if(errorstatus!=SD_OK)return errorstatus;   		//閸濆秴绨查柨娆掝嚖	 
 	if(DeviceMode==SD_POLLING_MODE)						//閺屻儴顕楀Ο鈥崇础,鏉烆喛顕楅弫鐗堝祦	 
	{
 		INTX_DISABLE();//閸忔娊妫撮幀璁宠厬閺?POLLING濡€崇础,娑撱儳顩︽稉顓熸焽閹垫挻鏌嘢DIO鐠囪鍟撻幙宥勭稊!!!)
		while(!(SDIO->STA&((1<<5)|(1<<1)|(1<<3)|(1<<10)|(1<<9))))//閺冪姳绗傚┃?CRC/鐡掑懏妞?鐎瑰本鍨?閺嶅洤绻?/鐠у嘲顫愭担宥夋晩鐠?
		{
			if(SDIO_GetFlagStatus(SDIO_FLAG_RXFIFOHF) != RESET)						//閹恒儲鏁归崠鍝勫磹濠?鐞涖劎銇氶懛鍐茬毌鐎涙ü绨?娑擃亜鐡?
			{
				for(count=0;count<8;count++)			//瀵邦亞骞嗙拠璇插絿閺佺増宓?
				{
					*(tempbuff+count)=SDIO->FIFO;
				}
				tempbuff+=8;	 
				timeout=0X7FFFFF; 	//鐠囩粯鏆熼幑顔藉閸戠儤妞傞梻?
			}else 	//婢跺嫮鎮婄搾鍛
			{
				if(timeout==0)return SD_DATA_TIMEOUT;
				timeout--;
			}
		} 
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		//閺佺増宓佺搾鍛闁挎瑨顕?
		{										   
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	//閺佺増宓侀崸妗烺C闁挎瑨顕?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_CRC_FAIL;		   
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR) != RESET) 	//閹恒儲鏁筬ifo娑撳﹥瀛╅柨娆掝嚖
		{
	 		SDIO_ClearFlag(SDIO_FLAG_RXOVERR);		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_RX_OVERRUN;		 
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	//閹恒儲鏁圭挧宄邦潗娴ｅ秹鏁婄拠?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);//濞撳懘鏁婄拠顖涚垼韫?
			return SD_START_BIT_ERR;		 
		}   
		while(SDIO_GetFlagStatus(SDIO_FLAG_RXDAVL) != RESET)	//FIFO闁插矂娼?鏉╂ê鐡ㄩ崷銊ュ讲閻劍鏆熼幑?
		{
			*tempbuff=SDIO->FIFO;	//瀵邦亞骞嗙拠璇插絿閺佺増宓?
			tempbuff++;
		}
		INTX_ENABLE();//瀵偓閸氼垱鈧鑵戦弬?
		SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
	 
	}else if(DeviceMode==SD_DMA_MODE)
	{
 		TransferError=SD_OK;
		StopCondition=0;			//閸楁洖娼＄拠?娑撳秹娓剁憰浣稿絺闁礁浠犲顫炊鏉堟挻瀵氭禒?
		TransferEnd=0;				//娴肩姾绶紒鎾存将閺嶅洨鐤嗘担宥忕礉閸︺劋鑵戦弬顓熸箛閸旓紕鐤?
		SDIO->MASK|=(1<<1)|(1<<3)|(1<<8)|(1<<5)|(1<<9);	//闁板秶鐤嗛棁鈧憰浣烘畱娑擃厽鏌?
	 	SDIO->DCTRL|=1<<3;		 	//SDIO DMA娴ｈ儻鍏?
 	    SD_DMA_Config((u32*)buf,blksize,DMA_DIR_PeripheralToMemory); 
 		while(((DMA2->LISR&(1<<27))==RESET)&&(TransferEnd==0)&&(TransferError==SD_OK)&&timeout)timeout--;//缁涘绶熸导鐘虹翻鐎瑰本鍨?
		if(timeout==0)return SD_DATA_TIMEOUT;//鐡掑懏妞?
		if(TransferError!=SD_OK)errorstatus=TransferError;  
    }   
 	return errorstatus; 
}
//SD閸椔ゎ嚢閸欐牕顦挎稉顏勬健 
//buf:鐠囩粯鏆熼幑顔剧处鐎涙ê灏?
//addr:鐠囪褰囬崷鏉挎絻
//blksize:閸ф銇囩亸?
//nblks:鐟曚浇顕伴崣鏍畱閸ф鏆?
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?
__align(4) u32 *tempbuff;
SD_Error SD_ReadMultiBlocks(u8 *buf,long long addr,u16 blksize,u32 nblks)
{
  SD_Error errorstatus=SD_OK;
	u8 power;
  u32 count=0;
	u32 timeout=SDIO_DATATIMEOUT;  
	tempbuff=(u32*)buf;//鏉烆剚宕叉稉绨?2閹稿洭鎷?
	
  SDIO->DCTRL=0x0;		//閺佺増宓侀幒褍鍩楃€靛嫬鐡ㄩ崳銊︾闂?閸忕煱MA)   
	if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)//婢堆冾啇闁插繐宕?
	{
		blksize=512;
		addr>>=9;
	}  
	
	  SDIO_DataInitStructure.SDIO_DataBlockSize= 0; ;//濞撳懘娅嶥PSM閻樿埖鈧焦婧€闁板秶鐤?
	  SDIO_DataInitStructure.SDIO_DataLength= 0 ;
	  SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	  SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	  SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	  SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
    SDIO_DataConfig(&SDIO_DataInitStructure);
	
	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;//閸楋繝鏀ｆ禍?
	if((blksize>0)&&(blksize<=2048)&&((blksize&(blksize-1))==0))
	{
		power=convert_from_bytes_to_power_of_two(blksize);	    
		
	  SDIO_CmdInitStructure.SDIO_Argument =  blksize;//閸欐垿鈧竼MD16+鐠佸墽鐤嗛弫鐗堝祦闂€鍨娑撶lksize,閻厼鎼锋惔?
		SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN;
		SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		SDIO_SendCommand(&SDIO_CmdInitStructure);
		
		errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);	//缁涘绶烺1閸濆秴绨? 
		
		if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖	 
		
	}else return SD_INVALID_PARAMETER;	  
	
	if(nblks>1)											//婢舵艾娼＄拠? 
	{									    
 	  	if(nblks*blksize>SD_MAX_DATA_LENGTH)return SD_INVALID_PARAMETER;//閸掋倖鏌囬弰顖氭儊鐡掑懓绻冮張鈧径褎甯撮弨鍫曟毐鎼?
		
		   SDIO_DataInitStructure.SDIO_DataBlockSize= power<<4; ;//nblks*blksize,512閸ф銇囩亸?閸椻€冲煂閹貉冨煑閸?
			 SDIO_DataInitStructure.SDIO_DataLength= nblks*blksize ;
			 SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
			 SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
			 SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToSDIO;
			 SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
			 SDIO_DataConfig(&SDIO_DataInitStructure);

       SDIO_CmdInitStructure.SDIO_Argument =  addr;//閸欐垿鈧竼MD18+娴犲穬ddr閸︽澘娼冮崙楦款嚢閸欐牗鏆熼幑?閻厼鎼锋惔?
	     SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_READ_MULT_BLOCK;
		   SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		   SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		   SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		   SDIO_SendCommand(&SDIO_CmdInitStructure);	
		
		errorstatus=CmdResp1Error(SD_CMD_READ_MULT_BLOCK);//缁涘绶烺1閸濆秴绨?
		
		if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖	 
		
 		if(DeviceMode==SD_POLLING_MODE)
		{
			INTX_DISABLE();//閸忔娊妫撮幀璁宠厬閺?POLLING濡€崇础,娑撱儳顩︽稉顓熸焽閹垫挻鏌嘢DIO鐠囪鍟撻幙宥勭稊!!!)
			while(!(SDIO->STA&((1<<5)|(1<<1)|(1<<3)|(1<<8)|(1<<9))))//閺冪姳绗傚┃?CRC/鐡掑懏妞?鐎瑰本鍨?閺嶅洤绻?/鐠у嘲顫愭担宥夋晩鐠?
			{
				if(SDIO_GetFlagStatus(SDIO_FLAG_RXFIFOHF) != RESET)						//閹恒儲鏁归崠鍝勫磹濠?鐞涖劎銇氶懛鍐茬毌鐎涙ü绨?娑擃亜鐡?
				{
					for(count=0;count<8;count++)			//瀵邦亞骞嗙拠璇插絿閺佺増宓?
					{
						*(tempbuff+count)=SDIO->FIFO;
					}
					tempbuff+=8;	 
					timeout=0X7FFFFF; 	//鐠囩粯鏆熼幑顔藉閸戠儤妞傞梻?
				}else 	//婢跺嫮鎮婄搾鍛
				{
					if(timeout==0)return SD_DATA_TIMEOUT;
					timeout--;
				}
			}  
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		//閺佺増宓佺搾鍛闁挎瑨顕?
		{										   
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	//閺佺増宓侀崸妗烺C闁挎瑨顕?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_CRC_FAIL;		   
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR) != RESET) 	//閹恒儲鏁筬ifo娑撳﹥瀛╅柨娆掝嚖
		{
	 		SDIO_ClearFlag(SDIO_FLAG_RXOVERR);		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_RX_OVERRUN;		 
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	//閹恒儲鏁圭挧宄邦潗娴ｅ秹鏁婄拠?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);//濞撳懘鏁婄拠顖涚垼韫?
			return SD_START_BIT_ERR;		 
		}   
	    
		while(SDIO_GetFlagStatus(SDIO_FLAG_RXDAVL) != RESET)	//FIFO闁插矂娼?鏉╂ê鐡ㄩ崷銊ュ讲閻劍鏆熼幑?
		{
			*tempbuff=SDIO->FIFO;	//瀵邦亞骞嗙拠璇插絿閺佺増宓?
			tempbuff++;
		}
	 		if(SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) != RESET)		//閹恒儲鏁圭紒鎾存将
			{
				if((SDIO_STD_CAPACITY_SD_CARD_V1_1==CardType)||(SDIO_STD_CAPACITY_SD_CARD_V2_0==CardType)||(SDIO_HIGH_CAPACITY_SD_CARD==CardType))
				{				
					SDIO_CmdInitStructure.SDIO_Argument =  0;//閸欐垿鈧竼MD12+缂佹挻娼导鐘虹翻
				  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_STOP_TRANSMISSION;
					SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
					SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
					SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
					SDIO_SendCommand(&SDIO_CmdInitStructure);	
					
					errorstatus=CmdResp1Error(SD_CMD_STOP_TRANSMISSION);//缁涘绶烺1閸濆秴绨?  
					
					if(errorstatus!=SD_OK)return errorstatus;	 
				}
 			}
			INTX_ENABLE();//瀵偓閸氼垱鈧鑵戦弬?
	 		SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
 		}else if(DeviceMode==SD_DMA_MODE)
		{
	   		TransferError=SD_OK;
			StopCondition=1;			//婢舵艾娼＄拠?闂団偓鐟曚礁褰傞柅浣镐粻濮濐澀绱舵潏鎾村瘹娴?
			TransferEnd=0;				//娴肩姾绶紒鎾存将閺嶅洨鐤嗘担宥忕礉閸︺劋鑵戦弬顓熸箛閸旓紕鐤?
			SDIO->MASK|=(1<<1)|(1<<3)|(1<<8)|(1<<5)|(1<<9);	//闁板秶鐤嗛棁鈧憰浣烘畱娑擃厽鏌?
		 	SDIO->DCTRL|=1<<3;		 						//SDIO DMA娴ｈ儻鍏?
	 	    SD_DMA_Config((u32*)buf,nblks*blksize,DMA_DIR_PeripheralToMemory); 
	 		while(((DMA2->LISR&(1<<27))==RESET)&&timeout)timeout--;//缁涘绶熸导鐘虹翻鐎瑰本鍨?
			if(timeout==0)return SD_DATA_TIMEOUT;//鐡掑懏妞?
			while((TransferEnd==0)&&(TransferError==SD_OK)); 
			if(TransferError!=SD_OK)errorstatus=TransferError;  	 
		}		 
  	}
	return errorstatus;
}			    																  
//SD閸椻€冲晸1娑擃亜娼?
//buf:閺佺増宓佺紓鎾崇摠閸?
//addr:閸愭瑥婀撮崸鈧?
//blksize:閸ф銇囩亸?  
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?
SD_Error SD_WriteBlock(u8 *buf,long long addr,  u16 blksize)
{
	SD_Error errorstatus = SD_OK;
	
	u8  power=0,cardstate=0;
	
	u32 timeout=0,bytestransferred=0;
	
	u32 cardstatus=0,count=0,restwords=0;
	
	u32	tlen=blksize;						//閹鏆辨惔?鐎涙濡?
	
	u32*tempbuff=(u32*)buf;					
	
 	if(buf==NULL)return SD_INVALID_PARAMETER;//閸欏倹鏆熼柨娆掝嚖  
	
  SDIO->DCTRL=0x0;							//閺佺増宓侀幒褍鍩楃€靛嫬鐡ㄩ崳銊︾闂?閸忕煱MA)
	
	SDIO_DataInitStructure.SDIO_DataBlockSize= 0; ;//濞撳懘娅嶥PSM閻樿埖鈧焦婧€闁板秶鐤?
	SDIO_DataInitStructure.SDIO_DataLength= 0 ;
	SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
  SDIO_DataConfig(&SDIO_DataInitStructure);
	
	
	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;//閸楋繝鏀ｆ禍?
 	if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)	//婢堆冾啇闁插繐宕?
	{
		blksize=512;
		addr>>=9;
	}    
	if((blksize>0)&&(blksize<=2048)&&((blksize&(blksize-1))==0))
	{
		power=convert_from_bytes_to_power_of_two(blksize);	
		
		SDIO_CmdInitStructure.SDIO_Argument = blksize;//閸欐垿鈧竼MD16+鐠佸墽鐤嗛弫鐗堝祦闂€鍨娑撶lksize,閻厼鎼锋惔?	
		SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN;
		SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		SDIO_SendCommand(&SDIO_CmdInitStructure);	
		
		errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);	//缁涘绶烺1閸濆秴绨? 
		
		if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖	 
		
	}else return SD_INVALID_PARAMETER;	
	
			SDIO_CmdInitStructure.SDIO_Argument = (u32)RCA<<16;//閸欐垿鈧竼MD13,閺屻儴顕楅崡锛勬畱閻樿埖鈧?閻厼鎼锋惔?	
		  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_STATUS;
			SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
			SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
			SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
			SDIO_SendCommand(&SDIO_CmdInitStructure);	

	  errorstatus=CmdResp1Error(SD_CMD_SEND_STATUS);		//缁涘绶烺1閸濆秴绨? 
	
	if(errorstatus!=SD_OK)return errorstatus;
	cardstatus=SDIO->RESP1;													  
	timeout=SD_DATATIMEOUT;
   	while(((cardstatus&0x00000100)==0)&&(timeout>0)) 	//濡偓閺岊櫂EADY_FOR_DATA娴ｅ秵妲搁崥锔剧枂娴?
	{
		timeout--;  
		
		SDIO_CmdInitStructure.SDIO_Argument = (u32)RCA<<16;//閸欐垿鈧竼MD13,閺屻儴顕楅崡锛勬畱閻樿埖鈧?閻厼鎼锋惔?
		SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_STATUS;
		SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		SDIO_SendCommand(&SDIO_CmdInitStructure);	
		
		errorstatus=CmdResp1Error(SD_CMD_SEND_STATUS);	//缁涘绶烺1閸濆秴绨?  
		
		if(errorstatus!=SD_OK)return errorstatus;		
		
		cardstatus=SDIO->RESP1;													  
	}
	if(timeout==0)return SD_ERROR;

			SDIO_CmdInitStructure.SDIO_Argument = addr;//閸欐垿鈧竼MD24,閸愭瑥宕熼崸妤佸瘹娴?閻厼鎼锋惔?	
			SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_WRITE_SINGLE_BLOCK;
			SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
			SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
			SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
			SDIO_SendCommand(&SDIO_CmdInitStructure);	
	
	errorstatus=CmdResp1Error(SD_CMD_WRITE_SINGLE_BLOCK);//缁涘绶烺1閸濆秴绨? 
	
	if(errorstatus!=SD_OK)return errorstatus;   	 
	
	StopCondition=0;									//閸楁洖娼￠崘?娑撳秹娓剁憰浣稿絺闁礁浠犲顫炊鏉堟挻瀵氭禒?

	SDIO_DataInitStructure.SDIO_DataBlockSize= power<<4; ;	//blksize, 閹貉冨煑閸ｃ劌鍩岄崡?
	SDIO_DataInitStructure.SDIO_DataLength= blksize ;
	SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
  SDIO_DataConfig(&SDIO_DataInitStructure);
	
	
	timeout=SDIO_DATATIMEOUT;
	
	if (DeviceMode == SD_POLLING_MODE)
	{
		INTX_DISABLE();//閸忔娊妫撮幀璁宠厬閺?POLLING濡€崇础,娑撱儳顩︽稉顓熸焽閹垫挻鏌嘢DIO鐠囪鍟撻幙宥勭稊!!!)
		while(!(SDIO->STA&((1<<10)|(1<<4)|(1<<1)|(1<<3)|(1<<9))))//閺佺増宓侀崸妤€褰傞柅浣瑰灇閸?娑撳瀛?CRC/鐡掑懏妞?鐠у嘲顫愭担宥夋晩鐠?
		{
			if(SDIO_GetFlagStatus(SDIO_FLAG_TXFIFOHE) != RESET)							//閸欐垿鈧礁灏崡濠勨敄,鐞涖劎銇氶懛鍐茬毌鐎涙ü绨?娑擃亜鐡?
			{
				if((tlen-bytestransferred)<SD_HALFFIFOBYTES)//娑撳秴顧?2鐎涙濡禍?
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
				timeout=0X3FFFFFFF;	//閸愭瑦鏆熼幑顔藉閸戠儤妞傞梻?
			}else
			{
				if(timeout==0)return SD_DATA_TIMEOUT;
				timeout--;
			}
		} 
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		//閺佺増宓佺搾鍛闁挎瑨顕?
		{										   
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	//閺佺増宓侀崸妗烺C闁挎瑨顕?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_CRC_FAIL;		   
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_TXUNDERR) != RESET) 	//閹恒儲鏁筬ifo娑撳瀛╅柨娆掝嚖
		{
	 		SDIO_ClearFlag(SDIO_FLAG_TXUNDERR);		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_TX_UNDERRUN;		 
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	//閹恒儲鏁圭挧宄邦潗娴ｅ秹鏁婄拠?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);//濞撳懘鏁婄拠顖涚垼韫?
			return SD_START_BIT_ERR;		 
		}   
	      
		INTX_ENABLE();//瀵偓閸氼垱鈧鑵戦弬?
		SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠? 
	}else if(DeviceMode==SD_DMA_MODE)
	{
   		TransferError=SD_OK;
		StopCondition=0;			//閸楁洖娼￠崘?娑撳秹娓剁憰浣稿絺闁礁浠犲顫炊鏉堟挻瀵氭禒?
		TransferEnd=0;				//娴肩姾绶紒鎾存将閺嶅洨鐤嗘担宥忕礉閸︺劋鑵戦弬顓熸箛閸旓紕鐤?
		SDIO->MASK|=(1<<1)|(1<<3)|(1<<8)|(1<<4)|(1<<9);	//闁板秶鐤嗘禍褏鏁撻弫鐗堝祦閹恒儲鏁圭€瑰本鍨氭稉顓熸焽
		SD_DMA_Config((u32*)buf,blksize,DMA_DIR_MemoryToPeripheral);				//SDIO DMA闁板秶鐤?
 	 	SDIO->DCTRL|=1<<3;								//SDIO DMA娴ｈ儻鍏?  
 		while(((DMA2->LISR&(1<<27))==RESET)&&timeout)timeout--;//缁涘绶熸导鐘虹翻鐎瑰本鍨?
		if(timeout==0)
		{
  			SD_Init();	 					//闁插秵鏌婇崚婵嗩潗閸栨湥D閸?閸欘垯浜掔憴锝呭枀閸愭瑥鍙嗗缁樻簚閻ㄥ嫰妫舵０?
			return SD_DATA_TIMEOUT;			//鐡掑懏妞? 
 		}
		timeout=SDIO_DATATIMEOUT;
		while((TransferEnd==0)&&(TransferError==SD_OK)&&timeout)timeout--;
 		if(timeout==0)return SD_DATA_TIMEOUT;			//鐡掑懏妞? 
  		if(TransferError!=SD_OK)return TransferError;
 	}  
 	SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
 	errorstatus=IsCardProgramming(&cardstate);
 	while((errorstatus==SD_OK)&&((cardstate==SD_CARD_PROGRAMMING)||(cardstate==SD_CARD_RECEIVING)))
	{
		errorstatus=IsCardProgramming(&cardstate);
	}   
	return errorstatus;
}
//SD閸椻€冲晸婢舵矮閲滈崸?
//buf:閺佺増宓佺紓鎾崇摠閸?
//addr:閸愭瑥婀撮崸鈧?
//blksize:閸ф銇囩亸?
//nblks:鐟曚礁鍟撻崗銉ф畱閸ф鏆?
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?											   
SD_Error SD_WriteMultiBlocks(u8 *buf,long long addr,u16 blksize,u32 nblks)
{
	SD_Error errorstatus = SD_OK;
	u8  power = 0, cardstate = 0;
	u32 timeout=0,bytestransferred=0;
	u32 count = 0, restwords = 0;
	u32 tlen=nblks*blksize;				//閹鏆辨惔?鐎涙濡?
	u32 *tempbuff = (u32*)buf;  
  if(buf==NULL)return SD_INVALID_PARAMETER; //閸欏倹鏆熼柨娆掝嚖  
  SDIO->DCTRL=0x0;							//閺佺増宓侀幒褍鍩楃€靛嫬鐡ㄩ崳銊︾闂?閸忕煱MA)   
	
	SDIO_DataInitStructure.SDIO_DataBlockSize= 0; ;	//濞撳懘娅嶥PSM閻樿埖鈧焦婧€闁板秶鐤?
	SDIO_DataInitStructure.SDIO_DataLength= 0 ;
	SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
	SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
	SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
	SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
  SDIO_DataConfig(&SDIO_DataInitStructure);
	
	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;//閸楋繝鏀ｆ禍?
 	if(CardType==SDIO_HIGH_CAPACITY_SD_CARD)//婢堆冾啇闁插繐宕?
	{
		blksize=512;
		addr>>=9;
	}    
	if((blksize>0)&&(blksize<=2048)&&((blksize&(blksize-1))==0))
	{
		power=convert_from_bytes_to_power_of_two(blksize);
		
		SDIO_CmdInitStructure.SDIO_Argument = blksize;	//閸欐垿鈧竼MD16+鐠佸墽鐤嗛弫鐗堝祦闂€鍨娑撶lksize,閻厼鎼锋惔?
		SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN;
		SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
		SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
		SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
		SDIO_SendCommand(&SDIO_CmdInitStructure);	
		
		errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);	//缁涘绶烺1閸濆秴绨? 
		
		if(errorstatus!=SD_OK)return errorstatus;   	//閸濆秴绨查柨娆掝嚖	 
		
	}else return SD_INVALID_PARAMETER;	 
	if(nblks>1)
	{					  
		if(nblks*blksize>SD_MAX_DATA_LENGTH)return SD_INVALID_PARAMETER;   
     	if((SDIO_STD_CAPACITY_SD_CARD_V1_1==CardType)||(SDIO_STD_CAPACITY_SD_CARD_V2_0==CardType)||(SDIO_HIGH_CAPACITY_SD_CARD==CardType))
    	{
			//閹绘劙鐝幀褑鍏?
				SDIO_CmdInitStructure.SDIO_Argument = (u32)RCA<<16;		//閸欐垿鈧竸CMD55,閻厼鎼锋惔?	
				SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;
				SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
				SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
				SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
				SDIO_SendCommand(&SDIO_CmdInitStructure);	
				
			errorstatus=CmdResp1Error(SD_CMD_APP_CMD);		//缁涘绶烺1閸濆秴绨?
				
			if(errorstatus!=SD_OK)return errorstatus;				 
				
				SDIO_CmdInitStructure.SDIO_Argument =nblks;		//閸欐垿鈧竼MD23,鐠佸墽鐤嗛崸妤佹殶闁?閻厼鎼锋惔?	 
				SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCK_COUNT;
				SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
				SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
				SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
				SDIO_SendCommand(&SDIO_CmdInitStructure);
			  
				errorstatus=CmdResp1Error(SD_CMD_SET_BLOCK_COUNT);//缁涘绶烺1閸濆秴绨?
				
			if(errorstatus!=SD_OK)return errorstatus;		
		    
		} 

				SDIO_CmdInitStructure.SDIO_Argument =addr;	//閸欐垿鈧竼MD25,婢舵艾娼￠崘娆愬瘹娴?閻厼鎼锋惔?	  
				SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_WRITE_MULT_BLOCK;
				SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
				SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
				SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
				SDIO_SendCommand(&SDIO_CmdInitStructure);	

 		errorstatus=CmdResp1Error(SD_CMD_WRITE_MULT_BLOCK);	//缁涘绶烺1閸濆秴绨?  		   
	
		if(errorstatus!=SD_OK)return errorstatus;

        SDIO_DataInitStructure.SDIO_DataBlockSize= power<<4; ;	//blksize, 閹貉冨煑閸ｃ劌鍩岄崡?
				SDIO_DataInitStructure.SDIO_DataLength= nblks*blksize ;
				SDIO_DataInitStructure.SDIO_DataTimeOut=SD_DATATIMEOUT ;
				SDIO_DataInitStructure.SDIO_DPSM=SDIO_DPSM_Enable;
				SDIO_DataInitStructure.SDIO_TransferDir=SDIO_TransferDir_ToCard;
				SDIO_DataInitStructure.SDIO_TransferMode=SDIO_TransferMode_Block;
				SDIO_DataConfig(&SDIO_DataInitStructure);
				
		if(DeviceMode==SD_POLLING_MODE)
	    {
			timeout=SDIO_DATATIMEOUT;
			INTX_DISABLE();//閸忔娊妫撮幀璁宠厬閺?POLLING濡€崇础,娑撱儳顩︽稉顓熸焽閹垫挻鏌嘢DIO鐠囪鍟撻幙宥勭稊!!!)
			while(!(SDIO->STA&((1<<4)|(1<<1)|(1<<8)|(1<<3)|(1<<9))))//娑撳瀛?CRC/閺佺増宓佺紒鎾存将/鐡掑懏妞?鐠у嘲顫愭担宥夋晩鐠?
			{
				if(SDIO_GetFlagStatus(SDIO_FLAG_TXFIFOHE) != RESET)							//閸欐垿鈧礁灏崡濠勨敄,鐞涖劎銇氶懛鍐茬毌鐎涙ü绨?鐎?32鐎涙濡?
				{	  
					if((tlen-bytestransferred)<SD_HALFFIFOBYTES)//娑撳秴顧?2鐎涙濡禍?
					{
						restwords=((tlen-bytestransferred)%4==0)?((tlen-bytestransferred)/4):((tlen-bytestransferred)/4+1);
						for(count=0;count<restwords;count++,tempbuff++,bytestransferred+=4)
						{
							SDIO->FIFO=*tempbuff;
						}
					}else 										//閸欐垿鈧礁灏崡濠勨敄,閸欘垯浜掗崣鎴︹偓浣藉殾鐏?鐎?32鐎涙濡?閺佺増宓?
					{
						for(count=0;count<SD_HALFFIFO;count++)
						{
							SDIO->FIFO=*(tempbuff+count);
						}
						tempbuff+=SD_HALFFIFO;
						bytestransferred+=SD_HALFFIFOBYTES;
					}
					timeout=0X3FFFFFFF;	//閸愭瑦鏆熼幑顔藉閸戠儤妞傞梻?
				}else
				{
					if(timeout==0)return SD_DATA_TIMEOUT; 
					timeout--;
				}
			} 
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		//閺佺増宓佺搾鍛闁挎瑨顕?
		{										   
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	//閺佺増宓侀崸妗烺C闁挎瑨顕?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_CRC_FAIL;		   
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_TXUNDERR) != RESET) 	//閹恒儲鏁筬ifo娑撳瀛╅柨娆掝嚖
		{
	 		SDIO_ClearFlag(SDIO_FLAG_TXUNDERR);		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_TX_UNDERRUN;		 
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	//閹恒儲鏁圭挧宄邦潗娴ｅ秹鏁婄拠?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);//濞撳懘鏁婄拠顖涚垼韫?
			return SD_START_BIT_ERR;		 
		}   
	      										   
			if(SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) != RESET)		//閸欐垿鈧胶绮ㄩ弶?
			{															 
				if((SDIO_STD_CAPACITY_SD_CARD_V1_1==CardType)||(SDIO_STD_CAPACITY_SD_CARD_V2_0==CardType)||(SDIO_HIGH_CAPACITY_SD_CARD==CardType))
				{   
					SDIO_CmdInitStructure.SDIO_Argument =0;//閸欐垿鈧竼MD12+缂佹挻娼导鐘虹翻 	  
					SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_STOP_TRANSMISSION;
					SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
					SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
					SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
					SDIO_SendCommand(&SDIO_CmdInitStructure);	
					
					errorstatus=CmdResp1Error(SD_CMD_STOP_TRANSMISSION);//缁涘绶烺1閸濆秴绨?  
					if(errorstatus!=SD_OK)return errorstatus;	 
				}
			}
			INTX_ENABLE();//瀵偓閸氼垱鈧鑵戦弬?
	 		SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
	    }else if(DeviceMode==SD_DMA_MODE)
		{
	   	TransferError=SD_OK;
			StopCondition=1;			//婢舵艾娼￠崘?闂団偓鐟曚礁褰傞柅浣镐粻濮濐澀绱舵潏鎾村瘹娴?
			TransferEnd=0;				//娴肩姾绶紒鎾存将閺嶅洨鐤嗘担宥忕礉閸︺劋鑵戦弬顓熸箛閸旓紕鐤?
			SDIO->MASK|=(1<<1)|(1<<3)|(1<<8)|(1<<4)|(1<<9);	//闁板秶鐤嗘禍褏鏁撻弫鐗堝祦閹恒儲鏁圭€瑰本鍨氭稉顓熸焽
			SD_DMA_Config((u32*)buf,nblks*blksize,DMA_DIR_MemoryToPeripheral);		//SDIO DMA闁板秶鐤?
	 	 	SDIO->DCTRL|=1<<3;								//SDIO DMA娴ｈ儻鍏? 
			timeout=SDIO_DATATIMEOUT;
	 		while(((DMA2->LISR&(1<<27))==RESET)&&timeout)timeout--;//缁涘绶熸导鐘虹翻鐎瑰本鍨?
			if(timeout==0)	 								//鐡掑懏妞?
			{									  
  				SD_Init();	 					//闁插秵鏌婇崚婵嗩潗閸栨湥D閸?閸欘垯浜掔憴锝呭枀閸愭瑥鍙嗗缁樻簚閻ㄥ嫰妫舵０?
	 			return SD_DATA_TIMEOUT;			//鐡掑懏妞? 
	 		}
			timeout=SDIO_DATATIMEOUT;
			while((TransferEnd==0)&&(TransferError==SD_OK)&&timeout)timeout--;
	 		if(timeout==0)return SD_DATA_TIMEOUT;			//鐡掑懏妞? 
	 		if(TransferError!=SD_OK)return TransferError;	 
		}
  	}
 	SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
 	errorstatus=IsCardProgramming(&cardstate);
 	while((errorstatus==SD_OK)&&((cardstate==SD_CARD_PROGRAMMING)||(cardstate==SD_CARD_RECEIVING)))
	{
		errorstatus=IsCardProgramming(&cardstate);
	}   
	return errorstatus;	   
}
//SDIO娑擃厽鏌囬張宥呭閸戣姤鏆?	  
void SDIO_IRQHandler(void) 
{											
 	SD_ProcessIRQSrc();//婢跺嫮鎮婇幍鈧張濉朌IO閻╃鍙ф稉顓熸焽
}	 																    
//SDIO娑擃厽鏌囨径鍕倞閸戣姤鏆?
//婢跺嫮鎮奡DIO娴肩姾绶潻鍥┾柤娑擃厾娈戦崥鍕潚娑擃厽鏌囨禍瀣
//鏉╂柨娲栭崐?闁挎瑨顕ゆ禒锝囩垳
SD_Error SD_ProcessIRQSrc(void)
{
	if(SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) != RESET)//閹恒儲鏁圭€瑰本鍨氭稉顓熸焽
	{	 
		if (StopCondition==1)
		{  
				SDIO_CmdInitStructure.SDIO_Argument =0;//閸欐垿鈧竼MD12+缂佹挻娼导鐘虹翻 	  
				SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_STOP_TRANSMISSION;
				SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
				SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
				SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
				SDIO_SendCommand(&SDIO_CmdInitStructure);	
					
			TransferError=CmdResp1Error(SD_CMD_STOP_TRANSMISSION);
		}else TransferError = SD_OK;	
 		SDIO->ICR|=1<<8;//濞撳懘娅庣€瑰本鍨氭稉顓熸焽閺嶅洩顔?
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));//閸忔娊妫撮惄绋垮彠娑擃厽鏌?
 		TransferEnd = 1;
		return(TransferError);
	}
 	if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)//閺佺増宓丆RC闁挎瑨顕?
	{
		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		//濞撳懘鏁婄拠顖涚垼韫?
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));//閸忔娊妫撮惄绋垮彠娑擃厽鏌?
	    TransferError = SD_DATA_CRC_FAIL;
	    return(SD_DATA_CRC_FAIL);
	}
 	if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)//閺佺増宓佺搾鍛闁挎瑨顕?
	{
		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT);  			//濞撳懍鑵戦弬顓熺垼韫?
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));//閸忔娊妫撮惄绋垮彠娑擃厽鏌?
	    TransferError = SD_DATA_TIMEOUT;
	    return(SD_DATA_TIMEOUT);
	}
  	if(SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR) != RESET)//FIFO娑撳﹥瀛╅柨娆掝嚖
	{
		SDIO_ClearFlag(SDIO_FLAG_RXOVERR);  			//濞撳懍鑵戦弬顓熺垼韫?
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));//閸忔娊妫撮惄绋垮彠娑擃厽鏌?
	    TransferError = SD_RX_OVERRUN;
	    return(SD_RX_OVERRUN);
	}
   	if(SDIO_GetFlagStatus(SDIO_FLAG_TXUNDERR) != RESET)//FIFO娑撳瀛╅柨娆掝嚖
	{
		SDIO_ClearFlag(SDIO_FLAG_TXUNDERR);  			//濞撳懍鑵戦弬顓熺垼韫?
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));//閸忔娊妫撮惄绋垮彠娑擃厽鏌?
	    TransferError = SD_TX_UNDERRUN;
	    return(SD_TX_UNDERRUN);
	}
	if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET)//鐠у嘲顫愭担宥夋晩鐠?
	{
		SDIO_ClearFlag(SDIO_FLAG_STBITERR);  		//濞撳懍鑵戦弬顓熺垼韫?
		SDIO->MASK&=~((1<<1)|(1<<3)|(1<<8)|(1<<14)|(1<<15)|(1<<4)|(1<<5)|(1<<9));//閸忔娊妫撮惄绋垮彠娑擃厽鏌?
	    TransferError = SD_START_BIT_ERR;
	    return(SD_START_BIT_ERR);
	}
	return(SD_OK);
}
  
//濡偓閺岊檳MD0閻ㄥ嫭澧界悰宀€濮搁幀?
//鏉╂柨娲栭崐?sd閸楋繝鏁婄拠顖滅垳
SD_Error CmdError(void)
{
	SD_Error errorstatus = SD_OK;
	u32 timeout=SDIO_CMD0TIMEOUT;	   
	while(timeout--)
	{
		if(SDIO_GetFlagStatus(SDIO_FLAG_CMDSENT) != RESET)break;	//閸涙垝鎶ゅ鎻掑絺闁?閺冪娀娓堕崫宥呯安)	 
	}	    
	if(timeout==0)return SD_CMD_RSP_TIMEOUT;  
	SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
	return errorstatus;
}	 
//濡偓閺岊櫂7閸濆秴绨查惃鍕晩鐠囶垳濮搁幀?
//鏉╂柨娲栭崐?sd閸楋繝鏁婄拠顖滅垳
SD_Error CmdResp7Error(void)
{
	SD_Error errorstatus=SD_OK;
	u32 status;
	u32 timeout=SDIO_CMD0TIMEOUT;
 	while(timeout--)
	{
		status=SDIO->STA;
		if(status&((1<<0)|(1<<2)|(1<<6)))break;//CRC闁挎瑨顕?閸涙垝鎶ら崫宥呯安鐡掑懏妞?瀹歌尙绮￠弨璺哄煂閸濆秴绨?CRC閺嶏繝鐛欓幋鎰)	
	}
 	if((timeout==0)||(status&(1<<2)))	//閸濆秴绨茬搾鍛
	{																				    
		errorstatus=SD_CMD_RSP_TIMEOUT;	//瑜版挸澧犻崡鈥茬瑝閺?.0閸忕厧顔愰崡?閹存牞鈧懍绗夐弨顖涘瘮鐠佹儳鐣鹃惃鍕暩閸樺瀵栭崶?
		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT); 			//濞撳懘娅庨崨鎴掓姢閸濆秴绨茬搾鍛閺嶅洤绻?
		return errorstatus;
	}	 
	if(status&1<<6)						//閹存劕濮涢幒銉︽暪閸掓澘鎼锋惔?
	{								   
		errorstatus=SD_OK;
		SDIO_ClearFlag(SDIO_FLAG_CMDREND); 				//濞撳懘娅庨崫宥呯安閺嶅洤绻?
 	}
	return errorstatus;
}	   
//濡偓閺岊櫂1閸濆秴绨查惃鍕晩鐠囶垳濮搁幀?
//cmd:瑜版挸澧犻崨鎴掓姢
//鏉╂柨娲栭崐?sd閸楋繝鏁婄拠顖滅垳
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
	if(SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET)					//閸濆秴绨茬搾鍛
	{																				    
 		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT); 				//濞撳懘娅庨崨鎴掓姢閸濆秴绨茬搾鍛閺嶅洤绻?
		return SD_CMD_RSP_TIMEOUT;
	}	
 	if(SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET)					//CRC闁挎瑨顕?
	{																				    
 		SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL); 				//濞撳懘娅庨弽鍥х箶
		return SD_CMD_CRC_FAIL;
	}		
	if(SDIO->RESPCMD!=cmd)return SD_ILLEGAL_CMD;//閸涙垝鎶ゆ稉宥呭爱闁?
  SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
	return (SD_Error)(SDIO->RESP1&SD_OCR_ERRORBITS);//鏉╂柨娲栭崡鈥虫惙鎼?
}
//濡偓閺岊櫂3閸濆秴绨查惃鍕晩鐠囶垳濮搁幀?
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?
SD_Error CmdResp3Error(void)
{
	u32 status;
	u32 timeout=SDIO_CMD0TIMEOUT;
	while(timeout--)
	{
		status=SDIO->STA;
		if(status&((1<<0)|(1<<2)|(1<<6)))break;//CRC闁挎瑨顕?閸涙垝鎶ら崫宥呯安鐡掑懏妞?瀹歌尙绮￠弨璺哄煂閸濆秴绨?CRC閺嶏繝鐛欓幋鎰)	
	}
 	if(SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET)					//閸濆秴绨茬搾鍛
	{											 
		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT);			//濞撳懘娅庨崨鎴掓姢閸濆秴绨茬搾鍛閺嶅洤绻?
		return SD_CMD_RSP_TIMEOUT;
	}	 
   SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
 	return SD_OK;								  
}
//濡偓閺岊櫂2閸濆秴绨查惃鍕晩鐠囶垳濮搁幀?
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?
SD_Error CmdResp2Error(void)
{
	SD_Error errorstatus=SD_OK;
	u32 status;
	u32 timeout=SDIO_CMD0TIMEOUT;
 	while(timeout--)
	{
		status=SDIO->STA;
		if(status&((1<<0)|(1<<2)|(1<<6)))break;//CRC闁挎瑨顕?閸涙垝鎶ら崫宥呯安鐡掑懏妞?瀹歌尙绮￠弨璺哄煂閸濆秴绨?CRC閺嶏繝鐛欓幋鎰)	
	}
  	if((timeout==0)||(status&(1<<2)))	//閸濆秴绨茬搾鍛
	{																				    
		errorstatus=SD_CMD_RSP_TIMEOUT; 
		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT); 		//濞撳懘娅庨崨鎴掓姢閸濆秴绨茬搾鍛閺嶅洤绻?
		return errorstatus;
	}	 
	if(SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET)						//CRC闁挎瑨顕?
	{								   
		errorstatus=SD_CMD_CRC_FAIL;
		SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL);		//濞撳懘娅庨崫宥呯安閺嶅洤绻?
 	}
	SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
 	return errorstatus;								    		 
} 
//濡偓閺岊櫂6閸濆秴绨查惃鍕晩鐠囶垳濮搁幀?
//cmd:娑斿澧犻崣鎴︹偓浣烘畱閸涙垝鎶?
//prca:閸椔ょ箲閸ョ偟娈慠CA閸︽澘娼?
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?
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
	if(SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET)					//閸濆秴绨茬搾鍛
	{																				    
 		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT);			//濞撳懘娅庨崨鎴掓姢閸濆秴绨茬搾鍛閺嶅洤绻?
		return SD_CMD_RSP_TIMEOUT;
	}	 	 
	if(SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET)						//CRC闁挎瑨顕?
	{								   
		SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL);					//濞撳懘娅庨崫宥呯安閺嶅洤绻?
 		return SD_CMD_CRC_FAIL;
	}
	if(SDIO->RESPCMD!=cmd)				//閸掋倖鏌囬弰顖氭儊閸濆秴绨瞔md閸涙垝鎶?
	{
 		return SD_ILLEGAL_CMD; 		
	}	    
	SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
	rspr1=SDIO->RESP1;					//瀵版鍩岄崫宥呯安 	 
	if(SD_ALLZERO==(rspr1&(SD_R6_GENERAL_UNKNOWN_ERROR|SD_R6_ILLEGAL_CMD|SD_R6_COM_CRC_FAILED)))
	{
		*prca=(u16)(rspr1>>16);			//閸欏磭些16娴ｅ秴绶遍崚?rca
		return errorstatus;
	}
   	if(rspr1&SD_R6_GENERAL_UNKNOWN_ERROR)return SD_GENERAL_UNKNOWN_ERROR;
   	if(rspr1&SD_R6_ILLEGAL_CMD)return SD_ILLEGAL_CMD;
   	if(rspr1&SD_R6_COM_CRC_FAILED)return SD_COM_CRC_FAILED;
	return errorstatus;
}

//SDIO娴ｈ儻鍏樼€硅姤鈧崵鍤庡Ο鈥崇础
//enx:0,娑撳秳濞囬懗?1,娴ｈ儻鍏?
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?
SD_Error SDEnWideBus(u8 enx)
{
	SD_Error errorstatus = SD_OK;
 	u32 scr[2]={0,0};
	u8 arg=0X00;
	if(enx)arg=0X02;
	else arg=0X00;
 	if(SDIO->RESP1&SD_CARD_LOCKED)return SD_LOCK_UNLOCK_FAILED;//SD閸椻€愁槱娴滃董OCKED閻樿埖鈧?	    
 	errorstatus=FindSCR(RCA,scr);						//瀵版鍩孲CR鐎靛嫬鐡ㄩ崳銊︽殶閹?
 	if(errorstatus!=SD_OK)return errorstatus;
	if((scr[1]&SD_WIDE_BUS_SUPPORT)!=SD_ALLZERO)		//閺€顖涘瘮鐎硅姤鈧崵鍤?
	{
		  SDIO_CmdInitStructure.SDIO_Argument = (uint32_t) RCA << 16;//閸欐垿鈧竼MD55+RCA,閻厼鎼锋惔?
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);
		
	 	errorstatus=CmdResp1Error(SD_CMD_APP_CMD);
		
	 	if(errorstatus!=SD_OK)return errorstatus; 
		
		  SDIO_CmdInitStructure.SDIO_Argument = arg;//閸欐垿鈧竸CMD6,閻厼鎼锋惔?閸欏倹鏆?10,4娴?00,1娴?	
      SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_SD_SET_BUSWIDTH;
      SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
      SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
      SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
      SDIO_SendCommand(&SDIO_CmdInitStructure);
			
     errorstatus=CmdResp1Error(SD_CMD_APP_SD_SET_BUSWIDTH);
		
		return errorstatus;
	}else return SD_REQUEST_NOT_APPLICABLE;				//娑撳秵鏁幐浣割啍閹崵鍤庣拋鍓х枂 	 
}												   
//濡偓閺屻儱宕遍弰顖氭儊濮濓絽婀幍褑顢戦崘娆愭惙娴?
//pstatus:瑜版挸澧犻悩鑸碘偓?
//鏉╂柨娲栭崐?闁挎瑨顕ゆ禒锝囩垳
SD_Error IsCardProgramming(u8 *pstatus)
{
 	vu32 respR1 = 0, status = 0;  
  
  SDIO_CmdInitStructure.SDIO_Argument = (uint32_t) RCA << 16; //閸楋紕娴夌€电懓婀撮崸鈧崣鍌涙殶
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_STATUS;//閸欐垿鈧竼MD13 	
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);	
 	
	status=SDIO->STA;
	
	while(!(status&((1<<0)|(1<<6)|(1<<2))))status=SDIO->STA;//缁涘绶熼幙宥勭稊鐎瑰本鍨?
   	if(SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) != RESET)			//CRC濡偓濞村銇戠拹?
	{  
	  SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL);	//濞撳懘娅庨柨娆掝嚖閺嶅洩顔?
		return SD_CMD_CRC_FAIL;
	}
   	if(SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) != RESET)			//閸涙垝鎶ょ搾鍛 
	{
		SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT);			//濞撳懘娅庨柨娆掝嚖閺嶅洩顔?
		return SD_CMD_RSP_TIMEOUT;
	}
 	if(SDIO->RESPCMD!=SD_CMD_SEND_STATUS)return SD_ILLEGAL_CMD;
	SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
	respR1=SDIO->RESP1;
	*pstatus=(u8)((respR1>>9)&0x0000000F);
	return SD_OK;
}
//鐠囪褰囪ぐ鎾冲閸楋紕濮搁幀?
//pcardstatus:閸楋紕濮搁幀?
//鏉╂柨娲栭崐?闁挎瑨顕ゆ禒锝囩垳
SD_Error SD_SendStatus(uint32_t *pcardstatus)
{
	SD_Error errorstatus = SD_OK;
	if(pcardstatus==NULL)
	{
		errorstatus=SD_INVALID_PARAMETER;
		return errorstatus;
	}
	
	SDIO_CmdInitStructure.SDIO_Argument = (uint32_t) RCA << 16;//閸欐垿鈧竼MD13,閻厼鎼锋惔?	 
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SEND_STATUS;
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);	
	
	errorstatus=CmdResp1Error(SD_CMD_SEND_STATUS);	//閺屻儴顕楅崫宥呯安閻樿埖鈧?
	if(errorstatus!=SD_OK)return errorstatus;
	*pcardstatus=SDIO->RESP1;//鐠囪褰囬崫宥呯安閸?
	return errorstatus;
} 
//鏉╂柨娲朣D閸楋紕娈戦悩鑸碘偓?
//鏉╂柨娲栭崐?SD閸楋紕濮搁幀?
SDCardState SD_GetState(void)
{
	u32 resp1=0;
	if(SD_SendStatus(&resp1)!=SD_OK)return SD_CARD_ERROR;
	else return (SDCardState)((resp1>>9) & 0x0F);
}
//閺屻儲澹楽D閸楋紕娈慡CR鐎靛嫬鐡ㄩ崳銊モ偓?
//rca:閸楋紕娴夌€电懓婀撮崸鈧?
//pscr:閺佺増宓佺紓鎾崇摠閸?鐎涙ê鍋峉CR閸愬懎顔?
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?	   
SD_Error FindSCR(u16 rca,u32 *pscr)
{ 
	u32 index = 0; 
	SD_Error errorstatus = SD_OK;
	u32 tempscr[2]={0,0};  
	
	SDIO_CmdInitStructure.SDIO_Argument = (uint32_t)8;	 //閸欐垿鈧竼MD16,閻厼鎼锋惔?鐠佸墽鐤咮lock Size娑?鐎涙濡?
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SET_BLOCKLEN; //	 cmd16
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;  //r1
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);
	
 	errorstatus=CmdResp1Error(SD_CMD_SET_BLOCKLEN);
	
 	if(errorstatus!=SD_OK)return errorstatus;	 
	
  SDIO_CmdInitStructure.SDIO_Argument = (uint32_t) RCA << 16; 
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_APP_CMD;//閸欐垿鈧竼MD55,閻厼鎼锋惔?	
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);
	
 	errorstatus=CmdResp1Error(SD_CMD_APP_CMD);
 	if(errorstatus!=SD_OK)return errorstatus;
	
  SDIO_DataInitStructure.SDIO_DataTimeOut = SD_DATATIMEOUT;
  SDIO_DataInitStructure.SDIO_DataLength = 8;  //8娑擃亜鐡ч懞鍌炴毐鎼?block娑?鐎涙濡?SD閸椻€冲煂SDIO.
  SDIO_DataInitStructure.SDIO_DataBlockSize = SDIO_DataBlockSize_8b  ;  //閸ф銇囩亸?byte 
  SDIO_DataInitStructure.SDIO_TransferDir = SDIO_TransferDir_ToSDIO;
  SDIO_DataInitStructure.SDIO_TransferMode = SDIO_TransferMode_Block;
  SDIO_DataInitStructure.SDIO_DPSM = SDIO_DPSM_Enable;
  SDIO_DataConfig(&SDIO_DataInitStructure);		

  SDIO_CmdInitStructure.SDIO_Argument = 0x0;
  SDIO_CmdInitStructure.SDIO_CmdIndex = SD_CMD_SD_APP_SEND_SCR;	//閸欐垿鈧竸CMD51,閻厼鎼锋惔?閸欏倹鏆熸稉?	
  SDIO_CmdInitStructure.SDIO_Response = SDIO_Response_Short;  //r1
  SDIO_CmdInitStructure.SDIO_Wait = SDIO_Wait_No;
  SDIO_CmdInitStructure.SDIO_CPSM = SDIO_CPSM_Enable;
  SDIO_SendCommand(&SDIO_CmdInitStructure);
	
 	errorstatus=CmdResp1Error(SD_CMD_SD_APP_SEND_SCR);
 	if(errorstatus!=SD_OK)return errorstatus;							   
 	while(!(SDIO->STA&(SDIO_FLAG_RXOVERR|SDIO_FLAG_DCRCFAIL|SDIO_FLAG_DTIMEOUT|SDIO_FLAG_DBCKEND|SDIO_FLAG_STBITERR)))
	{ 
		if(SDIO_GetFlagStatus(SDIO_FLAG_RXDAVL) != RESET)//閹恒儲鏁笷IFO閺佺増宓侀崣顖滄暏
		{
			*(tempscr+index)=SDIO->FIFO;	//鐠囪褰嘑IFO閸愬懎顔?
			index++;
			if(index>=2)break;
		}
	}
		if(SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) != RESET)		//閺佺増宓佺搾鍛闁挎瑨顕?
		{										   
	 		SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); 	//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_TIMEOUT;
	 	}else if(SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) != RESET)	//閺佺増宓侀崸妗烺C闁挎瑨顕?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL);  		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_DATA_CRC_FAIL;		   
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR) != RESET) 	//閹恒儲鏁筬ifo娑撳﹥瀛╅柨娆掝嚖
		{
	 		SDIO_ClearFlag(SDIO_FLAG_RXOVERR);		//濞撳懘鏁婄拠顖涚垼韫?
			return SD_RX_OVERRUN;		 
		}else if(SDIO_GetFlagStatus(SDIO_FLAG_STBITERR) != RESET) 	//閹恒儲鏁圭挧宄邦潗娴ｅ秹鏁婄拠?
		{
	 		SDIO_ClearFlag(SDIO_FLAG_STBITERR);//濞撳懘鏁婄拠顖涚垼韫?
			return SD_START_BIT_ERR;		 
		}  
   SDIO_ClearFlag(SDIO_STATIC_FLAGS);//濞撳懘娅庨幍鈧張澶嬬垼鐠?
	//閹跺﹥鏆熼幑顕€銆庢惔蹇斿瘻8娴ｅ秳璐熼崡鏇氱秴閸婃帟绻冮弶?   	
	*(pscr+1)=((tempscr[0]&SD_0TO7BITS)<<24)|((tempscr[0]&SD_8TO15BITS)<<8)|((tempscr[0]&SD_16TO23BITS)>>8)|((tempscr[0]&SD_24TO31BITS)>>24);
	*(pscr)=((tempscr[1]&SD_0TO7BITS)<<24)|((tempscr[1]&SD_8TO15BITS)<<8)|((tempscr[1]&SD_16TO23BITS)>>8)|((tempscr[1]&SD_24TO31BITS)>>24);
 	return errorstatus;
}
//瀵版鍩孨umberOfBytes娴?娑撳搫绨抽惃鍕瘹閺?
//NumberOfBytes:鐎涙濡弫?
//鏉╂柨娲栭崐?娴?娑撳搫绨抽惃鍕瘹閺佹澘鈧?
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

//闁板秶鐤哠DIO DMA  
//mbuf:鐎涙ê鍋嶉崳銊ユ勾閸р偓
//bufsize:娴肩姾绶弫鐗堝祦闁?
//dir:閺傜懓鎮?DMA_DIR_MemoryToPeripheral  鐎涙ê鍋嶉崳?->SDIO(閸愭瑦鏆熼幑?;DMA_DIR_PeripheralToMemory SDIO-->鐎涙ê鍋嶉崳?鐠囩粯鏆熼幑?;
void SD_DMA_Config(u32*mbuf,u32 bufsize,u32 dir)
{		 

  DMA_InitTypeDef  DMA_InitStructure;
	
	while (DMA_GetCmdStatus(DMA2_Stream3) != DISABLE){}//缁涘绶烡MA閸欘垶鍘ょ純?
		
  DMA_DeInit(DMA2_Stream3);//濞撳懐鈹栨稊瀣鐠囶櫣tream3娑撳﹦娈戦幍鈧張澶夎厬閺傤厽鐖ｈ箛?
	
 
  DMA_InitStructure.DMA_Channel = DMA_Channel_4;  //闁岸浜鹃柅澶嬪
  DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)&SDIO->FIFO;//DMA婢舵牞顔曢崷鏉挎絻
  DMA_InitStructure.DMA_Memory0BaseAddr = (u32)mbuf;//DMA 鐎涙ê鍋嶉崳?閸︽澘娼?
  DMA_InitStructure.DMA_DIR = dir;//鐎涙ê鍋嶉崳銊ュ煂婢舵牞顔曞Ο鈥崇础
  DMA_InitStructure.DMA_BufferSize = 0;//閺佺増宓佹导鐘虹翻闁?
  DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;//婢舵牞顔曢棃鐐差杻闁插繑膩瀵?
  DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;//鐎涙ê鍋嶉崳銊ヮ杻闁插繑膩瀵?
  DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;//婢舵牞顔曢弫鐗堝祦闂€鍨:32娴?
  DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Word;//鐎涙ê鍋嶉崳銊︽殶閹诡噣鏆辨惔?32娴?
  DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;// 娴ｈ法鏁ら弲顕€鈧碍膩瀵?
  DMA_InitStructure.DMA_Priority = DMA_Priority_VeryHigh;//閺堚偓妤傛ü绱崗鍫㈤獓
  DMA_InitStructure.DMA_FIFOMode = DMA_FIFOMode_Enable;   //FIFO娴ｈ儻鍏?     
  DMA_InitStructure.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;//閸忊€礗FO
  DMA_InitStructure.DMA_MemoryBurst = DMA_MemoryBurst_INC4;//婢舵牞顔曠粣浣稿絺4濞嗏€茬炊鏉?
  DMA_InitStructure.DMA_PeripheralBurst = DMA_PeripheralBurst_INC4;//鐎涙ê鍋嶉崳銊х崐閸?濞嗏€茬炊鏉?
  DMA_Init(DMA2_Stream3, &DMA_InitStructure);//閸掓繂顫愰崠鏈孧A Stream

	DMA_FlowControllerConfig(DMA2_Stream3,DMA_FlowCtrl_Peripheral);//婢舵牞顔曞ù浣瑰付閸?
	 
  DMA_Cmd(DMA2_Stream3 ,ENABLE);//瀵偓閸氱枍MA娴肩姾绶? 

}   


//鐠囩睏D閸?
//buf:鐠囩粯鏆熼幑顔剧处鐎涙ê灏?
//sector:閹靛洤灏崷鏉挎絻
//cnt:閹靛洤灏稉顏呮殶	
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?0,濮濓絽鐖?閸忔湹绮?闁挎瑨顕ゆ禒锝囩垳;				  				 
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
		 	sta=SD_ReadBlock(SDIO_DATA_BUFFER,lsector+512*n,512);//閸楁洑閲渟ector閻ㄥ嫯顕伴幙宥勭稊
			memcpy(buf,SDIO_DATA_BUFFER,512);
			buf+=512;
		} 
	}else
	{
		if(cnt==1)sta=SD_ReadBlock(buf,lsector,512);    	//閸楁洑閲渟ector閻ㄥ嫯顕伴幙宥勭稊
		else sta=SD_ReadMultiBlocks(buf,lsector,512,cnt);//婢舵矮閲渟ector  
	}
	return sta;
}
//閸愭┎D閸?
//buf:閸愭瑦鏆熼幑顔剧处鐎涙ê灏?
//sector:閹靛洤灏崷鏉挎絻
//cnt:閹靛洤灏稉顏呮殶	
//鏉╂柨娲栭崐?闁挎瑨顕ら悩鑸碘偓?0,濮濓絽鐖?閸忔湹绮?闁挎瑨顕ゆ禒锝囩垳;	
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
		 	sta=SD_WriteBlock(SDIO_DATA_BUFFER,lsector+512*n,512);//閸楁洑閲渟ector閻ㄥ嫬鍟撻幙宥勭稊
			buf+=512;
		} 
	}else
	{
		if(cnt==1)sta=SD_WriteBlock(buf,lsector,512);    	//閸楁洑閲渟ector閻ㄥ嫬鍟撻幙宥勭稊
		else sta=SD_WriteMultiBlocks(buf,lsector,512,cnt);	//婢舵矮閲渟ector  
	}
	return sta;
}







