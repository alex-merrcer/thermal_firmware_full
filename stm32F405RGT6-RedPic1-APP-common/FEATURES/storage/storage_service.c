#include "storage_service.h"

#include <stdio.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"

#define STORAGE_SERVICE_DRIVE_PATH       "0:"
#define STORAGE_SERVICE_ROOT_DIR         "0:/REDPIC"
#define STORAGE_SERVICE_SNAPSHOT_DIR     "0:/REDPIC/SNAP"
#define STORAGE_SERVICE_TEST_FILE_PATH   "0:/REDPIC/TEST.TXT"
#define STORAGE_SERVICE_TEST_TEXT        "RedPic SD OK\r\n"

static FATFS s_storage_fs;
static storage_info_t s_storage_info;
static uint8_t s_storage_inited = 0U;

static storage_status_t storage_service_update_capacity(storage_info_t *info)
{
    FATFS *fs = 0;
    DWORD free_clusters = 0;
    FRESULT fr = FR_OK;
    uint32_t total_sectors = 0U;
    uint32_t free_sectors = 0U;

    if (info == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_getfree((const TCHAR *)STORAGE_SERVICE_DRIVE_PATH, &free_clusters, &fs);
    if (fr != FR_OK || fs == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    total_sectors = (uint32_t)(fs->n_fatent - 2U) * (uint32_t)fs->csize;
    free_sectors = (uint32_t)free_clusters * (uint32_t)fs->csize;
#if _MAX_SS != 512
    total_sectors *= (uint32_t)fs->ssize / 512UL;
    free_sectors *= (uint32_t)fs->ssize / 512UL;
#endif

    info->total_kb = total_sectors >> 1;
    info->free_kb = free_sectors >> 1;
    return STORAGE_STATUS_OK;
}

static storage_status_t storage_service_mkdir_if_needed(const char *path)
{
    FRESULT fr = FR_OK;

    if (path == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_mkdir((const TCHAR *)path);
    if (fr == FR_OK || fr == FR_EXIST)
    {
        return STORAGE_STATUS_OK;
    }

    return STORAGE_STATUS_FS_ERROR;
}

void storage_service_init(void)
{
    memset(&s_storage_fs, 0, sizeof(s_storage_fs));
    memset(&s_storage_info, 0, sizeof(s_storage_info));
    s_storage_info.last_status = STORAGE_STATUS_NOT_READY;
    s_storage_inited = 1U;
}

uint8_t storage_service_mount(void)
{
    DSTATUS disk_status_value = 0;
    FRESULT fr = FR_OK;
    storage_status_t dir_status = STORAGE_STATUS_OK;

    if (s_storage_inited == 0U)
    {
        storage_service_init();
    }

    if (s_storage_info.mounted != 0U)
    {
        return 1U;
    }

    disk_status_value = disk_initialize(0);
    if ((disk_status_value & STA_NOINIT) != 0U)
    {
        s_storage_info.card_present = 0U;
        s_storage_info.mounted = 0U;
        s_storage_info.last_status = STORAGE_STATUS_INIT_FAIL;
        return 0U;
    }

    s_storage_info.card_present = 1U;

    fr = f_mount(&s_storage_fs, (const TCHAR *)STORAGE_SERVICE_DRIVE_PATH, 1);
    if (fr != FR_OK)
    {
        s_storage_info.mounted = 0U;
        s_storage_info.last_status = STORAGE_STATUS_MOUNT_FAIL;
        return 0U;
    }

    s_storage_info.mounted = 1U;
    s_storage_info.last_status = STORAGE_STATUS_OK;
    (void)storage_service_update_capacity(&s_storage_info);

    dir_status = storage_service_mkdir_if_needed(STORAGE_SERVICE_ROOT_DIR);
    if (dir_status == STORAGE_STATUS_OK)
    {
        dir_status = storage_service_mkdir_if_needed(STORAGE_SERVICE_SNAPSHOT_DIR);
    }

    s_storage_info.last_status = dir_status;
    return (dir_status == STORAGE_STATUS_OK) ? 1U : 0U;
}

uint8_t storage_service_is_mounted(void)
{
    return s_storage_info.mounted;
}

storage_status_t storage_service_get_info(storage_info_t *info)
{
    if (s_storage_inited == 0U)
    {
        storage_service_init();
    }

    if (s_storage_info.mounted != 0U)
    {
        s_storage_info.last_status = storage_service_update_capacity(&s_storage_info);
    }

    if (info != 0)
    {
        *info = s_storage_info;
    }

    return s_storage_info.last_status;
}

storage_status_t storage_service_ensure_redpic_dirs(void)
{
    storage_status_t status = STORAGE_STATUS_OK;

    if (storage_service_mount() == 0U)
    {
        return s_storage_info.last_status;
    }

    status = storage_service_mkdir_if_needed(STORAGE_SERVICE_ROOT_DIR);
    if (status == STORAGE_STATUS_OK)
    {
        status = storage_service_mkdir_if_needed(STORAGE_SERVICE_SNAPSHOT_DIR);
    }

    s_storage_info.last_status = status;
    return status;
}

storage_status_t storage_service_test_file(void)
{
    FIL file;
    UINT bytes_done = 0U;
    UINT text_len = (UINT)strlen(STORAGE_SERVICE_TEST_TEXT);
    char readback[24];
    FRESULT fr = FR_OK;
    storage_status_t status = STORAGE_STATUS_OK;

    memset(&file, 0, sizeof(file));
    memset(readback, 0, sizeof(readback));

    status = storage_service_ensure_redpic_dirs();
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    fr = f_open(&file, (const TCHAR *)STORAGE_SERVICE_TEST_FILE_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        s_storage_info.last_status = STORAGE_STATUS_FS_ERROR;
        return s_storage_info.last_status;
    }

    fr = f_write(&file, STORAGE_SERVICE_TEST_TEXT, text_len, &bytes_done);
    (void)f_close(&file);
    if (fr != FR_OK || bytes_done != text_len)
    {
        s_storage_info.last_status = STORAGE_STATUS_IO_ERROR;
        return s_storage_info.last_status;
    }

    fr = f_open(&file, (const TCHAR *)STORAGE_SERVICE_TEST_FILE_PATH, FA_READ);
    if (fr != FR_OK)
    {
        s_storage_info.last_status = STORAGE_STATUS_FS_ERROR;
        return s_storage_info.last_status;
    }

    fr = f_read(&file, readback, sizeof(readback) - 1U, &bytes_done);
    (void)f_close(&file);
    if (fr != FR_OK || bytes_done < text_len)
    {
        s_storage_info.last_status = STORAGE_STATUS_IO_ERROR;
        return s_storage_info.last_status;
    }

    if (strncmp(readback, STORAGE_SERVICE_TEST_TEXT, text_len) != 0)
    {
        s_storage_info.last_status = STORAGE_STATUS_IO_ERROR;
        return s_storage_info.last_status;
    }

    s_storage_info.last_status = STORAGE_STATUS_OK;
    return STORAGE_STATUS_OK;
}

const char *storage_service_status_text(storage_status_t status)
{
    switch (status)
    {
    case STORAGE_STATUS_OK:
        return "OK";
    case STORAGE_STATUS_NOT_READY:
        return "Not ready";
    case STORAGE_STATUS_INIT_FAIL:
        return "Init fail";
    case STORAGE_STATUS_MOUNT_FAIL:
        return "Mount fail";
    case STORAGE_STATUS_FS_ERROR:
        return "FS error";
    case STORAGE_STATUS_IO_ERROR:
        return "IO error";
    case STORAGE_STATUS_NO_SNAPSHOT:
        return "No snapshot";
    default:
        return "Storage err";
    }
}
