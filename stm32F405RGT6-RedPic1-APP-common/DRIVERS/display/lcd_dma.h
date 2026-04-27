#ifndef __LCD_DMA_H__
#define __LCD_DMA_H__

#include "sys.h"

void MYDMA_Config(void);
uint8_t LCD_Disp_Thermal_Interpolated_DMA(uint8_t *data24x32);
void set_color_mode(uint16_t mode);

#endif
