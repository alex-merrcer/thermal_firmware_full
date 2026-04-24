#ifndef _MLX90640_H_
#define _MLX90640_H_

#include "MLX90640_API.h"


uint16_t TempToColor(float val);
uint16_t BSP_LCD_GetColor565(uint8_t red, uint8_t green, uint8_t blue);

void update_com_reg(float ta);

int mlx90640_init(void);
int get_temp(float *data,float *ta);
void temp_data_sort(float *frameData,float *result);//温度数据镜像
void get_min_max_temp(float *frameData, float *min, uint16_t *min_pos,float *max,uint16_t *max_pos);//获取最低最高温度和位置
void conversion_data (float *frameData,float min ,float max, uint8_t *result);	//温度值转8位灰度
void bilinear_interpolation_128x160(uint8_t *frameData, uint8_t *result);
void bilinear_interpolation_128x160(uint8_t *frameData, uint8_t *result);
void LCD_Disp_Thermal(uint8_t *data, uint8_t state) ;
void lcd_disp(uint8_t *data,uint8_t state);
void LCD_Disp_Thermal_DMA(uint8_t *data) ;
//void temp_data_sort(float *frameData);
#endif
