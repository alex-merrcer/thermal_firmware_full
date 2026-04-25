/**
 * @copyright (C) 2017 Melexis N.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#ifndef _MLX90640_I2C_Driver_H_
#define _MLX90640_I2C_Driver_H_

#include "sys.h" 
#include "delay.h"
#include <stdint.h>

#ifndef REDPIC1_MLX90640_I2C_DMA_ENABLE
    #define REDPIC1_MLX90640_I2C_DMA_ENABLE 1U
#endif

#ifndef REDPIC1_MLX90640_I2C_DMA_WORD_THRESHOLD
    #define REDPIC1_MLX90640_I2C_DMA_WORD_THRESHOLD 32U
#endif

#ifndef REDPIC1_MLX90640_I2C_DMA_TIMEOUT_MS
    #define REDPIC1_MLX90640_I2C_DMA_TIMEOUT_MS 100UL
#endif



void MLX90640_I2CInit(void);
uint8_t MLX90640_I2CRuntimeInit(void);
int MLX90640_I2CRead(uint8_t slaveAddr,uint16_t startAddress, uint16_t nMemAddressRead, uint16_t *data);
int MLX90640_I2CWrite(uint8_t slaveAddr,uint16_t writeAddress, uint16_t data);
//void MLX90640_I2CFreqSet(int freq);
#endif
