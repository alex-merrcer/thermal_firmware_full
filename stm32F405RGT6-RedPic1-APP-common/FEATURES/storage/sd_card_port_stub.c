#include <string.h>

#include "sdio_sdcard.h"

SD_CardInfo SDCardInfo;

SD_Error SD_Init(void)
{
    memset(&SDCardInfo, 0, sizeof(SDCardInfo));
    SDCardInfo.CardBlockSize = 512U;
    SDCardInfo.CardCapacity = 0;
    return SD_NOT_CONFIGURED;
}

u8 SD_ReadDisk(u8 *buf, u32 sector, u8 cnt)
{
    (void)buf;
    (void)sector;
    (void)cnt;
    return 1U;
}

u8 SD_WriteDisk(u8 *buf, u32 sector, u8 cnt)
{
    (void)buf;
    (void)sector;
    (void)cnt;
    return 1U;
}
