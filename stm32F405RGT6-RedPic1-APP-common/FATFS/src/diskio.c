/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2013        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control module to the FatFs module with a defined API.        */
/*-----------------------------------------------------------------------*/

#include "diskio.h"		/* FatFs lower layer API */
#include "sdio_sdcard.h"

#define SD_CARD	 0

#define FLASH_SECTOR_SIZE 	512
#define FLASH_BLOCK_SIZE   	8
#define DISKIO_RETRY_LIMIT  2U

static volatile DSTATUS s_disk_status = STA_NOINIT;

DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive nmuber (0..) */
)
{
	u8 res=0;
	switch(pdrv)
	{
		case SD_CARD:
			res=SD_Init();
  			break;
		default:
			res=1;
	}
	if(res)
    {
        s_disk_status = STA_NOINIT;
        return  STA_NOINIT;
    }
    s_disk_status = 0;
	return 0;
}

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber (0..) */
)
{
    if (pdrv != SD_CARD)
    {
        return STA_NOINIT;
    }

	return s_disk_status;
}

DRESULT disk_read (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE *buff,		/* Data buffer to store read data */
	DWORD sector,	/* Sector address (LBA) */
	UINT count		/* Number of sectors to read (1..128) */
)
{
	u8 res=0;
    UINT attempt = 0U;
    if (!count)return RES_PARERR;
	switch(pdrv)
	{
		case SD_CARD:
            for (attempt = 0U; attempt < DISKIO_RETRY_LIMIT; ++attempt)
            {
			    res=SD_ReadDisk(buff,sector,count);
                if (res == 0U)
                {
                    break;
                }
                (void)SD_Init();
            }
			break;
		default:
			res=1;
	}
    if(res==0x00)
    {
        s_disk_status = 0;
        return RES_OK;
    }
    s_disk_status = STA_NOINIT;
    return RES_ERROR;
}

#if _USE_WRITE
DRESULT disk_write (
	BYTE pdrv,			/* Physical drive nmuber (0..) */
	const BYTE *buff,	/* Data to be written */
	DWORD sector,		/* Sector address (LBA) */
	UINT count			/* Number of sectors to write (1..128) */
)
{
	u8 res=0;
    UINT attempt = 0U;
    if (!count)return RES_PARERR;
	switch(pdrv)
	{
		case SD_CARD:
            for (attempt = 0U; attempt < DISKIO_RETRY_LIMIT; ++attempt)
            {
			    res=SD_WriteDisk((u8*)buff,sector,count);
                if (res == 0U)
                {
                    break;
                }
                (void)SD_Init();
            }
			break;
		default:
			res=1;
	}
    if(res == 0x00)
    {
        s_disk_status = 0;
        return RES_OK;
    }
    s_disk_status = STA_NOINIT;
    return RES_ERROR;
}
#endif

#if _USE_IOCTL
DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
	DRESULT res;
	if(pdrv==SD_CARD)
	{
	    switch(cmd)
	    {
		    case CTRL_SYNC:
				res = RES_OK;
		        break;
		    case GET_SECTOR_SIZE:
				*(DWORD*)buff = 512;
		        res = RES_OK;
		        break;
		    case GET_BLOCK_SIZE:
				*(WORD*)buff = SDCardInfo.CardBlockSize;
		        res = RES_OK;
		        break;
		    case GET_SECTOR_COUNT:
		        *(DWORD*)buff = SDCardInfo.CardCapacity/512;
		        res = RES_OK;
		        break;
		    default:
		        res = RES_PARERR;
		        break;
	    }
	}else res=RES_ERROR;
    return res;
}
#endif

DWORD get_fattime (void)
{
	return 0;
}

void *ff_memalloc (UINT size)
{
    (void)size;
	return 0;
}

void ff_memfree (void* mf)
{
    (void)mf;
}
