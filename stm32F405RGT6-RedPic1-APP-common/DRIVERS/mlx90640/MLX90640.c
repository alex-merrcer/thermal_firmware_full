#include "MLX90640.h"
#include "math.h"
#include <string.h>
#include "MLX90640_API.h"
#include "MLX90640_I2C_Driver.h"
#include "lcd.h"
#include "lcd_init.h"
#include "arm_math.h"
#include "lcd_dma.h"

/* MLX90640 的完整标定参数表。
 * 初始化时会从 EEPROM 中读出，并在后续每一帧温度计算中反复复用。 */
CCMRAM paramsMLX90640 MLXPars;
/* 传感器 EEPROM 原始数据缓存。 */
static CCMRAM uint16_t g_sensorEE[832];
/* 当前一帧的原始寄存器数据缓存。 */
static CCMRAM uint16_t g_sensorFrame[834];
/* 旧版整屏插值工作区已经移除。
 * 现在真正参与显示的插值流程已经迁移到 lcd_dma.c，并按行流式发送到 LCD。 */

/* 传感器初始化流程：
 * 1. 初始化 I2C 链路
 * 2. 设置帧率和工作模式
 * 3. 读取 EEPROM 标定参数
 * 4. 解析出后续测温所需的数学模型参数 */
int mlx90640_init(void)
{
		int status;
		MLX90640_I2CInit();
		delay_ms(50);
		status = MLX90640_SetRefreshRate(MLX90640_ADDR, RefreshRate);					//设置帧率
		if(status != 0) return status;

		status = MLX90640_SetChessMode(MLX90640_ADDR);	
		if(status != 0) return status;

		status = MLX90640_DumpEE(MLX90640_ADDR, g_sensorEE);					//读取像素校正参数
		if(status != 0) return status;

		status = MLX90640_ExtractParameters(g_sensorEE, &MLXPars);		//解析校正参数
		if(status != 0) return status;

		return 0;
}
/* 获取一帧温度结果：
 * data 指向 768 个像素温度，ta 返回当前环境温度/芯片温度估计值。 */
int get_temp_ex(float *data, float *ta, uint8_t *subpage)
{
	int status;
	if (data == 0 || ta == 0)
	{
		return -1;
	}
	status = MLX90640_GetFrameData(MLX90640_ADDR, g_sensorFrame);
	if(status < 0)
	{
		return status;
	}

	if(subpage != 0)
	{
		*subpage = (uint8_t)(g_sensorFrame[833] & 0x01U);
	}

	*ta = MLX90640_GetTa(g_sensorFrame, &MLXPars);
	MLX90640_CalculateTo(g_sensorFrame, &MLXPars, 0.95f , *ta - 8.0f, data);
	return 0;
}

int get_temp(float *data ,float *ta)
{
	return get_temp_ex(data, ta, 0);
}

/* 把 MLX90640 的 24x32 原始布局转成 32x24。
 * 这样后面的显示链路按“屏幕行优先”的方式处理会更自然。 */
void temp_data_sort(float *frameData,float *result)//温度数据镜像
{
	 arm_matrix_instance_f32 src = {24, 32, frameData};
   arm_matrix_instance_f32 dst = {32, 24, result};
   arm_mat_trans_f32(&src, &dst);
}

/* 扫描整帧温度，找出最低温、最高温以及它们的位置。
 * 这一步直接决定灰度拉伸区间，也就是最终显示的对比度。 */
void get_min_max_temp(float *frameData, float *min, uint16_t *min_pos,float *max,uint16_t *max_pos)//获取最低最高温度和位置
{
	uint16_t x;
	*max = -40;
	*min = 400;       
	for(x=0;x<768;x++)
	{
		if(frameData[x]>*max) 
		{ 
			*max=frameData[x]; 
			*max_pos=x;
		}
		if(frameData[x]<*min) 
		{ 
			*min=frameData[x]; 
			*min_pos=x;
		}
	}
}
/* 把真实温度线性映射到 0~255 灰度。
 * 后面的伪彩并不是直接对温度着色，而是先对这个灰度值查表。 */
void conversion_data(float *frameData,float min ,float max, uint8_t *result)	//温度值转8位灰度
{
	uint16_t x;
	float a;
	if (max <= min)                                             /* C9 修复：防止除零 */
	{
		memset(result, 0, 768);
		return;
	}
	a = 255.0f/(max - min);
	for(x=0;x<768;x++)
	{
		result[x] = (uint8_t)((frameData[x]- min) *a);
	}
}
