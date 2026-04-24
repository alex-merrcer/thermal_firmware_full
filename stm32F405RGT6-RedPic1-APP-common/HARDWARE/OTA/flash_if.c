#include "flash_if.h"

static uint32_t GetSector(uint32_t Address);

uint32_t MY_FLASH_Erase(uint32_t Address)
{
    uint32_t start_sector = GetSector(Address);

    if (FLASH_EraseSector(start_sector, VoltageRange_3) != FLASH_COMPLETE)
    {
        return 1U;
    }

    return 0U;
}

uint32_t FLASH_If_Write(__IO uint32_t *FlashAddress, uint32_t *Data, uint32_t DataLength)
{
    uint32_t i = 0U;

    /* Write OTA payload to internal Flash word-by-word (32-bit).
     * Boundary check prevents writing outside configured APP area. */
    for (i = 0U; (i < DataLength) && (*FlashAddress <= (USER_FLASH_END_ADDRESS - 4U)); ++i)
    {
        if (FLASH_ProgramWord(*FlashAddress, *(uint32_t *)(Data + i)) == FLASH_COMPLETE)
        {
            /* Immediate read-back verify:
             * detect write disturb / power noise / flash programming failure. */
            if (*(uint32_t *)*FlashAddress != *(uint32_t *)(Data + i))
            {
                return 2U;
            }

            /* Move to next 32-bit address after successful write+verify. */
            *FlashAddress += 4U;
        }
        else
        {
            /* Flash controller reported programming error. */
            return 1U;
        }
    }

    /* 0U means all requested words were written successfully. */
    return 0U;
}

static uint32_t GetSector(uint32_t Address)
{
    uint32_t sector = 0U;

    if ((Address < ADDR_FLASH_SECTOR_1) && (Address >= ADDR_FLASH_SECTOR_0))
    {
        sector = FLASH_Sector_0;
    }
    else if ((Address < ADDR_FLASH_SECTOR_2) && (Address >= ADDR_FLASH_SECTOR_1))
    {
        sector = FLASH_Sector_1;
    }
    else if ((Address < ADDR_FLASH_SECTOR_3) && (Address >= ADDR_FLASH_SECTOR_2))
    {
        sector = FLASH_Sector_2;
    }
    else if ((Address < ADDR_FLASH_SECTOR_4) && (Address >= ADDR_FLASH_SECTOR_3))
    {
        sector = FLASH_Sector_3;
    }
    else if ((Address < ADDR_FLASH_SECTOR_5) && (Address >= ADDR_FLASH_SECTOR_4))
    {
        sector = FLASH_Sector_4;
    }
    else if ((Address < ADDR_FLASH_SECTOR_6) && (Address >= ADDR_FLASH_SECTOR_5))
    {
        sector = FLASH_Sector_5;
    }
    else if ((Address < ADDR_FLASH_SECTOR_7) && (Address >= ADDR_FLASH_SECTOR_6))
    {
        sector = FLASH_Sector_6;
    }
    else if ((Address < ADDR_FLASH_SECTOR_8) && (Address >= ADDR_FLASH_SECTOR_7))
    {
        sector = FLASH_Sector_7;
    }
    else if ((Address < ADDR_FLASH_SECTOR_9) && (Address >= ADDR_FLASH_SECTOR_8))
    {
        sector = FLASH_Sector_8;
    }
    else if ((Address < ADDR_FLASH_SECTOR_10) && (Address >= ADDR_FLASH_SECTOR_9))
    {
        sector = FLASH_Sector_9;
    }
    else if ((Address < ADDR_FLASH_SECTOR_11) && (Address >= ADDR_FLASH_SECTOR_10))
    {
        sector = FLASH_Sector_10;
    }
    else
    {
        sector = FLASH_Sector_11;
    }

    return sector;
}
