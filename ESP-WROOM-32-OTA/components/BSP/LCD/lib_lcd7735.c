#include <stdio.h>
#include "lib_lcd7735.h"
#include "ascii_font.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_err.h"

spi_device_handle_t tft_hspi = NULL;
static uint8_t s_lcd_panel_sleeping = 0;

void lcdSelectRegister(unsigned char data);
void lcdWriteDataU8(unsigned char data);
void lcdWriteDataU16(unsigned short data);
void SpiSend(uint8_t *data, uint8_t dataLength);

void SpiSend(uint8_t *data, uint8_t dataLength)
{
	spi_transaction_t ext;
	memset(&ext, 0, sizeof(ext));
	ext.rxlength = 0;
	ext.length = 8 * dataLength;
	ext.tx_buffer = data;

	esp_err_t lcd_state = spi_device_polling_transmit(tft_hspi, &ext);
	if (lcd_state != ESP_OK)
	{
		printf("lcd_err -->%d\n", lcd_state);
	}
}

void LcdGpioSpiInit(void)
{
	gpio_config_t lcd_io = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = (1ULL << LCD_PIN_RES),
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.pull_up_en = GPIO_PULLUP_ENABLE,
	};
	gpio_config(&lcd_io);
#if (LCD_HARDWARE_CS)
	lcd_io.pin_bit_mask = (1ULL << LCD_PIN_CS);
	gpio_config(&lcd_io);
#endif
	lcd_io.pin_bit_mask = (1ULL << LCD_PIN_A0);
	gpio_config(&lcd_io);
	lcd_io.pin_bit_mask = (1ULL << LCD_PIN_BL);
	gpio_config(&lcd_io);

	spi_bus_config_t buscfg = {
		.miso_io_num = -1,
		.mosi_io_num = LCD_PIN_SDA,
		.sclk_io_num = LCD_PIN_SCL,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
	};
	buscfg.max_transfer_sz = 40 * sizeof(uint8_t);

	esp_err_t tft_spi_f = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
	if (tft_spi_f != ESP_OK)
	{
		printf("--tft--spi--bus--initialize--err,%d\n", tft_spi_f);
	}

	spi_device_interface_config_t interface_config = {
		.address_bits = 0,
		.command_bits = 0,
		.clock_speed_hz = 30 * 1000 * 1000,
		.mode = LCD_SPI_MODE,
#if LCD_HARDWARE_CS
		.spics_io_num = LCD_PIN_CS,
#endif
		.pre_cb = NULL,
		.post_cb = NULL,
		.duty_cycle_pos = 0,
		.queue_size = 6,
	};

	tft_spi_f = spi_bus_add_device(LCD_SPI_HOST, &interface_config, &tft_hspi);
	if (tft_spi_f != ESP_OK)
	{
		printf("--tft--spi--deiver--config--err,%d\n", tft_spi_f);
	}
}
//鍙戝懡浠c鈥斺€攃
void lcdSelectRegister(unsigned char com)
{
	CLR_LCD_A0;  ///<鍛戒护
#if !(LCD_HARDWARE_CS)
	CLR_LCD_CS;	 ///<浣胯兘璁惧
#endif

	SpiSend(&com, 1); 	///<鍙?bit

#if !(LCD_HARDWARE_CS)
	SET_LCD_CS;
#endif
}
//鍙戞暟鎹甦c-d
void lcdWriteDataU8(unsigned char data)
{
	SET_LCD_A0;
#if !(LCD_HARDWARE_CS)
	CLR_LCD_CS;	 ///<浣胯兘璁惧
#endif
	//LCD_DELAY(1);
	SpiSend(&data, 1);
	//LCD_DELAY(1);
#if !(LCD_HARDWARE_CS)
	SET_LCD_CS;
#endif
}

void lcdWriteDataU16(unsigned short data)
{
	lcdWriteDataU8(data >> 8);
	lcdWriteDataU8(data);

	// SET_LCD_A0;
	// #if !(LCD_HARDWARE_CS)

	//--*ESP32 灞炰簬灏忕鑺墖锛屽嵆 uint16_t 鍜?uint32_t 鍙橀噺鐨勬渶浣庢湁鏁堜綅瀛樺偍鍦ㄦ渶灏忕殑鍦板潃銆傚洜姝わ紝濡傛灉 uint16_t 瀛樺偍鍦ㄥ唴瀛樹腑锛屽垯棣栧厛鍙戦€佷綅 [7:0]锛屽叾娆℃槸浣?[15:8]銆?/
	/*idf缂栫▼鎵嬪唽 :鍦ㄦ煇浜涙儏鍐典笅锛岃浼犺緭鐨勬暟鎹ぇ灏忎笌 uint8_t 鏁扮粍涓嶅悓锛屽彲浣跨敤浠ヤ笅瀹忓皢鏁版嵁杞崲涓哄彲鐢?SPI 椹卞姩鐩存帴鍙戦€佺殑鏍煎紡锛?		闇€浼犺緭鐨勬暟鎹紝浣跨敤 SPI_SWAP_DATA_TX
		鎺ユ敹鍒扮殑鏁版嵁锛屼娇鐢?SPI_SWAP_DATA_RX
		*/
	// 	CLR_LCD_CS;	 ///<浣胯兘璁惧
	// #endif
	// //LCD_DELAY(1);
	// SpiSend((uint8_t*)&data,2);
	// //LCD_DELAY(1);
	// #if !(LCD_HARDWARE_CS)
	// 	SET_LCD_CS;
	// #endif

}


void lcdInit(void)
{
	//gpio
	LcdGpioSpiInit();
	ESP_LOGI("LCD", "lcdInit");
	//澶嶄綅
	SET_LCD_RES;
	LCD_DELAY(10);
	CLR_LCD_RES;
	LCD_DELAY(100);
	SET_LCD_RES;
	LCD_DELAY(200);

	SET_LCD_BL;
	LCD_DELAY(100);
	//
	lcdSelectRegister(0x11); //Sleep out
	LCD_DELAY(120);

	lcdSelectRegister(0xB1);
	lcdWriteDataU8(0x05);
	lcdWriteDataU8(0x3C);
	lcdWriteDataU8(0x3C);

	lcdSelectRegister(0xB2);
	lcdWriteDataU8(0x05);
	lcdWriteDataU8(0x3C);
	lcdWriteDataU8(0x3C);

	lcdSelectRegister(0xB3);
	lcdWriteDataU8(0x05);
	lcdWriteDataU8(0x3C);
	lcdWriteDataU8(0x3C);
	lcdWriteDataU8(0x05);
	lcdWriteDataU8(0x3C);
	lcdWriteDataU8(0x3C);

	lcdSelectRegister(0xB4);
	lcdWriteDataU8(0x03);

	lcdSelectRegister(0xC0);
	lcdWriteDataU8(0x28);
	lcdWriteDataU8(0x08);
	lcdWriteDataU8(0x04);

	lcdSelectRegister(0xC1);
	lcdWriteDataU8(0XC0);

	lcdSelectRegister(0xC2);
	lcdWriteDataU8(0x0D);
	lcdWriteDataU8(0x00);

	lcdSelectRegister(0xC3);
	lcdWriteDataU8(0x8D);
	lcdWriteDataU8(0x2A);

	lcdSelectRegister(0xC4);
	lcdWriteDataU8(0x8D);
	lcdWriteDataU8(0xEE);

	lcdSelectRegister(0xC5);
	lcdWriteDataU8(0x1A);

	///<鏄剧ず鏂瑰悜/
	lcdSelectRegister(0x36);
#if LCD_DIR==LCD_DIR_VAERTICAL1
	lcdWriteDataU8(0x00);
#elif (LCD_DIR==LCD_DIR_VAERTICAL2)
	lcdWriteDataU8(0xC0);
#elif (LCD_DIR==LCD_DIR_TRANSVERSEt1)
	lcdWriteDataU8(0x70);
#else
	lcdWriteDataU8(0xA0);
#endif
	lcdSelectRegister(0xE0);
	lcdWriteDataU8(0x04);
	lcdWriteDataU8(0x22);
	lcdWriteDataU8(0x07);
	lcdWriteDataU8(0x0A);
	lcdWriteDataU8(0x2E);
	lcdWriteDataU8(0x30);
	lcdWriteDataU8(0x25);
	lcdWriteDataU8(0x2A);
	lcdWriteDataU8(0x28);
	lcdWriteDataU8(0x26);
	lcdWriteDataU8(0x2E);
	lcdWriteDataU8(0x3A);
	lcdWriteDataU8(0x00);
	lcdWriteDataU8(0x01);
	lcdWriteDataU8(0x03);
	lcdWriteDataU8(0x13);

	lcdSelectRegister(0xE1);
	lcdWriteDataU8(0x04);
	lcdWriteDataU8(0x16);
	lcdWriteDataU8(0x06);
	lcdWriteDataU8(0x0D);
	lcdWriteDataU8(0x2D);
	lcdWriteDataU8(0x26);
	lcdWriteDataU8(0x23);
	lcdWriteDataU8(0x27);
	lcdWriteDataU8(0x27);
	lcdWriteDataU8(0x25);
	lcdWriteDataU8(0x2D);
	lcdWriteDataU8(0x3B);
	lcdWriteDataU8(0x00);
	lcdWriteDataU8(0x01);
	lcdWriteDataU8(0x04);
	lcdWriteDataU8(0x13);

	lcdSelectRegister(0x3A);
	lcdWriteDataU8(0x05);

	lcdSelectRegister(0x29);
s_lcd_panel_sleeping = 0;
}

//璁剧疆缁樺埗鍦板潃
void lcdSetAddress(unsigned short x1, unsigned short y1, unsigned short x2, unsigned short y2)
{
#if (LCD_DIR==LCD_DIR_VAERTICAL1||LCD_DIR==LCD_DIR_VAERTICAL2)
	lcdSelectRegister(0x2A);   ///
	//寮€濮嬪垪鍦板潃
	lcdWriteDataU16(x1); ///st7735s灞忓箷濂藉儚鏈夌偣鍋忕Щ锛?2姝ｅ父
	//缁撴潫鍒楀湴鍧€
	lcdWriteDataU16(x2);
	lcdSelectRegister(0x2B); 	///
	//寮€濮?	lcdWriteDataU16(y1);
	//缁撴潫鐐?	lcdWriteDataU16(y2);
#else
	lcdSelectRegister(0x2A);   ///
	//寮€濮嬪垪鍦板潃
	lcdWriteDataU16(x1); ///灞忓箷濂藉儚鏈夌偣鍋忕Щ锛?2姝ｅ父
	//缁撴潫鍒楀湴鍧€
	lcdWriteDataU16(x2);
	lcdSelectRegister(0x2B); 	///
	//寮€濮?	lcdWriteDataU16(y1 + 2);
	//缁撴潫鐐?	lcdWriteDataU16(y2 + 2);
#endif

	lcdSelectRegister(0x2C);	///<Memory Write
}

//娓呭睆
void lcdClear(unsigned short color)
{
	lcdSetAddress(0, 0, LCD_ROW_SIZE, LCD_COLUMN_SIZE);///<鏁翠釜灞忓箷澶у皬

	for (unsigned char i = 0; i <= LCD_ROW_SIZE; i++)
	{
		for (unsigned char j = 0; j <= LCD_COLUMN_SIZE; j++)
		{
			lcdWriteDataU16(color);
		}
	}
}

//LCD鐢荤偣
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
	lcdSetAddress(x, y, x, y);//璁剧疆鍏夋爣浣嶇疆
	lcdWriteDataU16(color);
}

//蹇€熺敾鐐?//x,y:鍧愭爣
//color:棰滆壊
void LCD_Fast_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
	lcdWriteDataU16(color);
}

void LCD_DisplayOn(void)
{
	 lcdSelectRegister(0x29);
}
//LCD鍏抽棴鏄剧ず
void LCD_DisplayOff(void)
{
	lcdSelectRegister(0x28);	//Display off
}

void LCD_PanelWake(void)
{
	if (s_lcd_panel_sleeping == 0)
	{
		LCD_DisplayOn();
		SET_LCD_BL;
		return;
	}

	lcdSelectRegister(0x11);	//Sleep out
	LCD_DELAY(120);
	LCD_DisplayOn();
	SET_LCD_BL;
	s_lcd_panel_sleeping = 0;
}

void LCD_PanelSleep(void)
{
	if (s_lcd_panel_sleeping != 0)
	{
		CLR_LCD_BL;
		LCD_DisplayOff();
		return;
	}

	LCD_DisplayOff();
	LCD_DELAY(20);
	lcdSelectRegister(0x10);	//Sleep in
	LCD_DELAY(120);
	CLR_LCD_BL;
	s_lcd_panel_sleeping = 1;
}

//鍦ㄦ寚瀹氬尯鍩熷唴濉厖鍗曚釜棰滆壊
//(sx,sy),(ex,ey):濉厖鐭╁舰瀵硅鍧愭爣,鍖哄煙澶у皬涓?(ex-sx+1)*(ey-sy+1)
//color:瑕佸～鍏呯殑棰滆壊
void LCD_Fill(unsigned short sx, unsigned short sy, unsigned short ex, unsigned short ey, unsigned short color)
{
	unsigned short i, j;
	unsigned short xlen = 0;
	unsigned short ylen = 0;

	xlen = ex - sx + 1;
	ylen = ey - sy + 1;

	lcdSetAddress(sx, sy, ex, ey);
	for (i = 0; i < xlen; i++)
	{
		for (j = 0; j < ylen; j++)
		{
			lcdWriteDataU16(color);
		}
	}
}

//鐢荤嚎
//x1,y1:璧风偣鍧愭爣
//x2,y2:缁堢偣鍧愭爣
void LCD_DrawLine(unsigned short x1, unsigned short y1, unsigned short x2, unsigned short y2, unsigned short color)
{
	unsigned short t;
	int xerr = 0, yerr = 0, delta_x, delta_y, distance;
	int incx, incy, uRow, uCol;
	delta_x = x2 - x1; //璁＄畻鍧愭爣澧為噺
	delta_y = y2 - y1;
	uRow = x1;
	uCol = y1;

	//璁剧疆鍗曟鏂瑰悜
	if ( delta_x > 0 )
	{
		incx = 1;
	}
	else if ( delta_x == 0 ) //鍨傜洿绾?	
	{
		incx = 0;
	}
	else
	{
		incx = -1;
		delta_x = -delta_x;
	}


	if ( delta_y > 0 )
	{
		incy = 1;
	}
	else if ( delta_y == 0 ) //姘村钩绾?	
	{
		incy = 0;
	}
	else
	{
		incy = -1;
		delta_y = -delta_y;
	}

	if ( delta_x > delta_y ) //閫夊彇鍩烘湰澧為噺鍧愭爣杞?	
	{
		distance = delta_x;
	}
	else
	{
		distance = delta_y;
	}

	for (t = 0; t <= distance + 1; t++ ) //鐢荤嚎杈撳嚭
	{
		LCD_DrawPoint(uRow, uCol, color);//鐢荤偣
		xerr += delta_x ;
		yerr += delta_y ;
		if ( xerr > distance )
		{
			xerr -= distance;
			uRow += incx;
		}

		if ( yerr > distance )
		{
			yerr -= distance;
			uCol += incy;
		}
	}
}
/// @brief 缁樺埗鐭╁舰
/// @param x1 	stort x
/// @param y1 	stort y
/// @param x2 	end	x
/// @param y2 	end y
/// @param color 棰滆壊
void LCD_DrawRectangle(unsigned short x1, unsigned short y1, unsigned short x2, unsigned short y2, unsigned short color)
{
	LCD_DrawLine(x1, y1, x2, y1, color);
	LCD_DrawLine(x1, y1, x1, y2, color);
	LCD_DrawLine(x1, y2, x2, y2, color);
	LCD_DrawLine(x2, y1, x2, y2, color);
}

//鍦ㄦ寚瀹氫綅缃敾涓€涓寚瀹氬ぇ灏忕殑鍦?//(x,y):涓績鐐?//r    :鍗婂緞
void LCD_Draw_Circle(unsigned short x0, unsigned short y0, unsigned char r, unsigned short color)
{
	int a, b;
	int di;
	a = 0;
	b = r;
	di = 3 - ( r << 1 );           //鍒ゆ柇涓嬩釜鐐逛綅缃殑鏍囧織
	while ( a <= b )
	{
		LCD_DrawPoint(x0 + a, y0 - b, color);
		LCD_DrawPoint(x0 + b, y0 - a, color);
		LCD_DrawPoint(x0 + b, y0 + a, color);
		LCD_DrawPoint(x0 + a, y0 + b, color);
		LCD_DrawPoint(x0 - a, y0 + b, color);
		LCD_DrawPoint(x0 - b, y0 + a, color);
		LCD_DrawPoint(x0 - a, y0 - b, color);
		LCD_DrawPoint(x0 - b, y0 - a, color);
		a++;
		//浣跨敤Bresenham绠楁硶鐢诲渾
		if ( di < 0 )
		{
			di += 4 * a + 6;
		}
		else
		{
			di += 10 + 4 * ( a - b );
			b--;
		}
	}
}

//鍦ㄦ寚瀹氫綅缃敾涓€涓寚瀹氬ぇ灏忕殑鍦?//(x,y):涓績鐐?//r    :鍗婂緞
void LCD_DrawFullCircle(unsigned short Xpos, unsigned short Ypos, unsigned short Radius, unsigned short Color)
{
	uint16_t x, y, r = Radius;
	for (y = Ypos - r; y < Ypos + r; y++)
	{
		for (x = Xpos - r; x < Xpos + r; x++)
		{
			if (((x - Xpos) * (x - Xpos) + (y - Ypos) * (y - Ypos)) <= r * r)
			{
				LCD_DrawPoint(x, y, Color);
			}
		}
	}
}

// void LCD_ShowChar8(unsigned short x, unsigned short y, unsigned char ch, unsigned char font_size,  unsigned short pen_color, unsigned short back_color)
// {
// 	int i = 0, j = 0;
// 	unsigned char temp = 0;
// 	unsigned char size = 0;


// 	 if((x > (LCD_COLUMN_SIZE - font_size / 2)) || (y > (LCD_ROW_SIZE - font_size)))
// 		 return;

// 	 ch = ch - ' ';
// 	 if(font_size == 8)
// 	 {

//  			size = (font_size / 8 + ((font_size % 8) ? 1 : 0)) * (font_size / 2);

// 			for(i = 0; i < size; i++)
// 			{
// 				  temp = asc2_0804[ch][i];

// 					for(j = 0; j < 4; j++)
// 					{
// 							if(temp & 0x80)
// 							LCD_DrawPoint(x, y, pen_color);
// 							else
// 							LCD_DrawPoint(x, y, back_color);

// 							temp <<= 1;
// 					}
// 			}
// 	 }
// 	}
//鍦ㄦ寚瀹氫綅缃樉绀轰竴涓瓧绗?//x,y:璧峰鍧愭爣
//num:瑕佹樉绀虹殑瀛楃:" "--->"~"
//size:瀛椾綋澶у皬 12/16/24
//mode:鍙犲姞鏂瑰紡(1)杩樻槸闈炲彔鍔犳柟寮?0)
void LCD_ShowChar(unsigned short x, unsigned short y, unsigned char num, unsigned char size, unsigned char mode, unsigned short pen_color, unsigned short back_color)
{
    unsigned char temp, t1, t;
	unsigned short y0 = y;
	unsigned char csize = ( size / 8 + ( (size % 8) ? 1 : 0)) * (size / 2); //寰楀埌瀛椾綋涓€涓瓧绗﹀搴旂偣闃甸泦鎵€鍗犵殑瀛楄妭鏁? 	num = num - ' ';//寰楀埌鍋忕Щ鍚庣殑鍊硷紙ASCII瀛楀簱鏄粠绌烘牸寮€濮嬪彇妯★紝鎵€浠?' '灏辨槸瀵瑰簲瀛楃鐨勫瓧搴擄級

	for(t = 0; t < csize; t++)
	{
		// if(size == 8)//璋冪敤0804瀛椾綋锛堜笉鍙敤锛?		// {
		// 	temp = asc2_0804[num][t];
		// }
		if(size == 12)//璋冪敤1206瀛椾綋
		{
			temp = asc2_1206[num][t];
		}
		else if(size == 16)//璋冪敤1608瀛椾綋
		{
			temp=asc2_1608[num][t];
		}
		else if(size == 24)	//璋冪敤2412瀛椾綋
		{
			temp=asc2_2412[num][t];
		}
		else
			return; //娌℃湁鐨勫瓧搴?
		for(t1 = 0; t1 < 8; t1++)
		{
			if( temp & 0x80 )
			{
				LCD_DrawPoint(x, y, pen_color);
			}
			else if( mode == 0)
			{
				LCD_DrawPoint(x, y, back_color);
			}
			temp <<= 1;
			y++;

			if(y >= LCD_COLUMN_SIZE)//瓒呭尯鍩?			
			{
				return;
			}

			if((y-y0) == size)
			{
				y = y0;
				x++;
				if(x>=LCD_ROW_SIZE)//瓒呭尯鍩?				
				{
					return;
				}
				break;
			}
		}
	}
}


//鏄剧ず瀛楃涓?//x,y:璧风偣鍧愭爣
//width,height:鍖哄煙澶у皬
//size:瀛椾綋澶у皬
//*p:瀛楃涓茶捣濮嬪湴鍧€
void LCD_ShowString(unsigned short x, unsigned short y, unsigned short width, unsigned short height, unsigned char size, unsigned char *p, unsigned short pen_color, unsigned short back_color)
{
	unsigned char x0 = x;
	width += x;
	height += y;
    while((*p<='~')&&(*p>=' '))//鍒ゆ柇鏄笉鏄潪娉曞瓧绗?
    {
        if(x >= width)
		{
			x = x0;
			y += size;
		}

        if(y >= height)//閫€鍑?		
		{
			break;
		}

        LCD_ShowChar(x, y, *p, size, 0, pen_color, back_color);
        x += size / 2;
        p++;
    }
}

uint32_t LCD_Pow(uint8_t m,uint8_t n)
{
	uint32_t result=1;
	while(n--)result*=m;
	return result;
}
//鏄剧ず鏁板瓧
//x,y:璧风偣鍧愭爣
//len:鏁板瓧鐨勪綅鏁?//size:瀛椾綋澶у皬
//num:瑕佹樉绀虹殑鏁板瓧,0~4294967295
void LCD_ShowNum(unsigned short x, unsigned short y, unsigned short len, unsigned short size, unsigned long num, unsigned short pen_color, unsigned short back_color)
{
	unsigned short t,temp;
	for(t=0;t<len;t++)
	{
		temp=(num/LCD_Pow(10,len-t-1))%10;
			if(temp==0)
			{
				LCD_ShowChar(x+(size/2)*t,y,'0',size,0, pen_color, back_color);
		  }
			else
	 	LCD_ShowChar(x+(size/2)*t,y,temp+'0',size,0, pen_color, back_color);
	}
}

/******************************************************************************
      鍑芥暟璇存槑锛氭樉绀轰袱浣嶅皬鏁板彉閲?      鍏ュ彛鏁版嵁锛歺,y鏄剧ず鍧愭爣
                num 瑕佹樉绀哄皬鏁板彉閲?                len 瑕佹樉绀虹殑浣嶆暟
                fc 瀛楃殑棰滆壊
                bc 瀛楃殑鑳屾櫙鑹?                sizey 瀛楀彿
      杩斿洖鍊硷細  鏃?******************************************************************************/
void LCD_ShowFloatNum1(unsigned short x, unsigned short y, unsigned short len, unsigned short size,float num, unsigned short pen_color, unsigned short back_color)
{         	
	unsigned short t,temp,sizex;
	uint16_t num1;
	sizex=size/2;
	num1=num*100;
	for(t=0;t<len;t++) 
	{
		temp=(num1/LCD_Pow(10,len-t-1))%10;
		if(t==(len-2))
		{
			LCD_ShowChar(x+(len-2)*sizex,y,'.',size,0, pen_color, back_color);
			t++;
			len+=1;
		}
		
		LCD_ShowChar(x+t*sizex,y,temp+'0',size,0, pen_color, back_color);
	}
}



/******************************************************************************
      鍑芥暟璇存槑锛氭樉绀哄崟涓?6x16姹夊瓧
      鍏ュ彛鏁版嵁锛歺,y鏄剧ず鍧愭爣
                *s 瑕佹樉绀虹殑姹夊瓧
                fc 瀛楃殑棰滆壊
                bc 瀛楃殑鑳屾櫙鑹?                sizey 瀛楀彿
                mode:  0闈炲彔鍔犳ā寮? 1鍙犲姞妯″紡
      杩斿洖鍊硷細  鏃?******************************************************************************/
void LCD_ShowChinese16x16(u16 x,u16 y,u8 *s,u16 fc,u16 bc,u8 sizey,u8 mode)
{
	u8 i,j,m=0;
	u16 k;
	u16 HZnum;//姹夊瓧鏁扮洰
	u16 TypefaceNum;//涓€涓瓧绗︽墍鍗犲瓧鑺傚ぇ灏?	
	u16 x0=x;
  TypefaceNum=(sizey/8+((sizey%8)?1:0))*sizey;
	HZnum=sizeof(tfont16)/sizeof(typFNT_GB16);	//缁熻姹夊瓧鏁扮洰
	for(k=0;k<HZnum;k++) 
	{
		if ((tfont16[k].Index[0]==*(s))&&(tfont16[k].Index[1]==*(s+1)))
		{ 	
			lcdSetAddress(x,y,x+sizey-1,y+sizey-1);
			for(i=0;i<TypefaceNum;i++)
			{
				for(j=0;j<8;j++)
				{	
					if(!mode)				
					{
						if(tfont16[k].Msk[i]&(0x01<<j))lcdWriteDataU16(fc);
						else lcdWriteDataU16(bc);
						m++;
						if(m%sizey==0)
						{
							m=0;
							break;
						}
					}
					else
					{
						if(tfont16[k].Msk[i]&(0x01<<j))	LCD_DrawPoint(x,y,fc);
						x++;
						if((x-x0)==sizey)
						{
							x=x0;
							y++;
							break;
						}
					}
				}
			}
		}				  	
		continue;  
	}
} 





