#include "sd_test.h"
#include "sdio_sdcard.h"
#include "usart.h"

extern SD_CardInfo SDCardInfo;

const char* SD_GetCardTypeString(u8 card_type)
{
    switch(card_type)
    {
        case SDIO_STD_CAPACITY_SD_CARD_V1_1:
            return "Standard Capacity SD V1.1";
        case SDIO_STD_CAPACITY_SD_CARD_V2_0:
            return "Standard Capacity SD V2.0";
        case SDIO_HIGH_CAPACITY_SD_CARD:
            return "High Capacity SDHC";
        case SDIO_MULTIMEDIA_CARD:
            return "MMC Card";
        case SDIO_SECURE_DIGITAL_IO_CARD:
            return "SDIO Card";
        case SDIO_HIGH_SPEED_MULTIMEDIA_CARD:
            return "High Speed MMC";
        case SDIO_SECURE_DIGITAL_IO_COMBO_CARD:
            return "SDIO Combo Card";
        case SDIO_HIGH_CAPACITY_MMC_CARD:
            return "High Capacity MMC";
        default:
            return "Unknown Type";
    }
}

void SD_Test_Init(void)
{
}

u8 SD_Init_Check(void)
{
    SD_Error status;

    printf("\r\n========================================\r\n");
    printf("       SD Card Initialization          \r\n");
    printf("========================================\r\n");

    status = SD_Init();
    if(status != SD_OK)
    {
        printf("SD Init Failed: %d\r\n", status);
        return 1;
    }
    printf("SD Init Success!\r\n");

    printf("\r\n========================================\r\n");
    printf("       SD Card Information              \r\n");
    printf("========================================\r\n");
    printf("Card Type:    %s\r\n", SD_GetCardTypeString(SDCardInfo.CardType));
    printf("Manufacturer ID: 0x%02X\r\n", SDCardInfo.SD_cid.ManufacturerID);
    printf("Card Capacity: %lu Bytes (%.2f GB)\r\n",
           (u32)SDCardInfo.CardCapacity,
           (float)SDCardInfo.CardCapacity / (1024.0f * 1024.0f * 1024.0f));
    printf("Block Size: %lu Bytes\r\n", SDCardInfo.CardBlockSize);
    printf("========================================\r\n");

    return 0;
}
