#include "adc.h"

void Adc_Init(void)
{
    GPIO_InitTypeDef gpio_init_structure;
    ADC_CommonInitTypeDef adc_common_init_structure;
    ADC_InitTypeDef adc_init_structure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    gpio_init_structure.GPIO_Pin = GPIO_Pin_0;
    gpio_init_structure.GPIO_Mode = GPIO_Mode_AN;
    gpio_init_structure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOB, &gpio_init_structure);

    RCC_APB2PeriphResetCmd(RCC_APB2Periph_ADC1, ENABLE);
    RCC_APB2PeriphResetCmd(RCC_APB2Periph_ADC1, DISABLE);

    adc_common_init_structure.ADC_Mode = ADC_Mode_Independent;
    adc_common_init_structure.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles;
    adc_common_init_structure.ADC_DMAAccessMode = ADC_DMAAccessMode_Disabled;
    adc_common_init_structure.ADC_Prescaler = ADC_Prescaler_Div4;
    ADC_CommonInit(&adc_common_init_structure);

    adc_init_structure.ADC_Resolution = ADC_Resolution_12b;
    adc_init_structure.ADC_ScanConvMode = DISABLE;
    adc_init_structure.ADC_ContinuousConvMode = DISABLE;
    adc_init_structure.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None;
    adc_init_structure.ADC_DataAlign = ADC_DataAlign_Right;
    adc_init_structure.ADC_NbrOfConversion = 1;
    ADC_Init(ADC1, &adc_init_structure);

    ADC_Cmd(ADC1, ENABLE);
}

u16 Get_Adc(u8 ch)
{
    ADC_RegularChannelConfig(ADC1, ch, 1, ADC_SampleTime_480Cycles);
    ADC_SoftwareStartConv(ADC1);

    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET)
    {
    }

    return ADC_GetConversionValue(ADC1);
}

u16 Get_Adc_Average(u8 ch, u8 times)
{
    u32 temp_val = 0U;
    u8 t = 0U;

    if (times == 0U)
    {
        return 0U;
    }

    for (t = 0U; t < times; t++)
    {
        temp_val += Get_Adc(ch);
    }

    return (u16)(temp_val / times);
}
