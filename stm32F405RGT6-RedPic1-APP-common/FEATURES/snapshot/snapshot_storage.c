#include "snapshot_storage.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "redpic1_thermal.h"
#include "thermal_snapshot_file.h"

#define SNAPSHOT_STORAGE_INDEX_FILE      "0:/REDPIC/INDEX.TXT"
#define SNAPSHOT_STORAGE_LOG_FILE        "0:/REDPIC/RUN.CSV"
#define SNAPSHOT_STORAGE_PATH_TEMPLATE   "0:/REDPIC/SNAP/%06lu.RTS"

static uint32_t snapshot_storage_parse_u32(const char *text)
{
    uint32_t value = 0U;

    if (text == 0)
    {
        return 0U;
    }

    while (*text >= '0' && *text <= '9')
    {
        value = (value * 10UL) + (uint32_t)(*text - '0');
        ++text;
    }

    return value;
}

static storage_status_t snapshot_storage_read_index(uint32_t *out_index)
{
    FIL file;
    char text[16];
    UINT bytes_done = 0U;
    FRESULT fr = FR_OK;

    if (out_index == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    *out_index = 0U;
    memset(&file, 0, sizeof(file));
    memset(text, 0, sizeof(text));

    fr = f_open(&file, (const TCHAR *)SNAPSHOT_STORAGE_INDEX_FILE, FA_READ);
    if (fr == FR_NO_FILE)
    {
        return STORAGE_STATUS_OK;
    }
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_read(&file, text, sizeof(text) - 1U, &bytes_done);
    (void)f_close(&file);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    text[bytes_done] = '\0';
    *out_index = snapshot_storage_parse_u32(text);
    return STORAGE_STATUS_OK;
}

static storage_status_t snapshot_storage_write_index(uint32_t index)
{
    FIL file;
    char text[16];
    UINT bytes_done = 0U;
    UINT text_len = 0U;
    FRESULT fr = FR_OK;

    memset(&file, 0, sizeof(file));
    memset(text, 0, sizeof(text));

    snprintf(text, sizeof(text), "%lu\r\n", (unsigned long)index);
    text_len = (UINT)strlen(text);

    fr = f_open(&file, (const TCHAR *)SNAPSHOT_STORAGE_INDEX_FILE, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_write(&file, text, text_len, &bytes_done);
    (void)f_close(&file);
    if (fr != FR_OK || bytes_done != text_len)
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    return STORAGE_STATUS_OK;
}

static storage_status_t snapshot_storage_append_log(uint32_t index,
                                                    const redpic1_thermal_snapshot_t *snapshot)
{
    FIL file;
    char line[96];
    UINT bytes_done = 0U;
    UINT line_len = 0U;
    FRESULT fr = FR_OK;

    if (snapshot == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    memset(&file, 0, sizeof(file));
    memset(line, 0, sizeof(line));

    snprintf(line,
             sizeof(line),
             "%06lu,%lu,%d,%d,%d\r\n",
             (unsigned long)index,
             (unsigned long)snapshot->timestamp_ms,
             snapshot->min_x10,
             snapshot->max_x10,
             snapshot->center_x10);
    line_len = (UINT)strlen(line);

    fr = f_open(&file, (const TCHAR *)SNAPSHOT_STORAGE_LOG_FILE, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_lseek(&file, f_size(&file));
    if (fr != FR_OK)
    {
        (void)f_close(&file);
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_write(&file, line, line_len, &bytes_done);
    (void)f_close(&file);
    if (fr != FR_OK || bytes_done != line_len)
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    return STORAGE_STATUS_OK;
}

storage_status_t snapshot_storage_save_latest(uint32_t *out_index)
{
    redpic1_thermal_snapshot_t thermal_snapshot;
    redpic_snapshot_t file_snapshot;
    FIL file;
    char path[32];
    UINT bytes_done = 0U;
    uint32_t last_index = 0U;
    uint32_t next_index = 0U;
    FRESULT fr = FR_OK;
    storage_status_t status = STORAGE_STATUS_OK;

    memset(&thermal_snapshot, 0, sizeof(thermal_snapshot));
    memset(&file_snapshot, 0, sizeof(file_snapshot));
    memset(&file, 0, sizeof(file));
    memset(path, 0, sizeof(path));

    status = storage_service_ensure_redpic_dirs();
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    if (redpic1_thermal_copy_latest_snapshot(&thermal_snapshot) == 0U)
    {
        return STORAGE_STATUS_NO_SNAPSHOT;
    }

    status = snapshot_storage_read_index(&last_index);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    next_index = last_index + 1UL;
    snprintf(path, sizeof(path), SNAPSHOT_STORAGE_PATH_TEMPLATE, (unsigned long)next_index);

    thermal_snapshot_file_fill(&file_snapshot, &thermal_snapshot);

    fr = f_open(&file, (const TCHAR *)path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_write(&file, &file_snapshot, sizeof(file_snapshot), &bytes_done);
    (void)f_close(&file);
    if (fr != FR_OK || bytes_done != sizeof(file_snapshot))
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    status = snapshot_storage_write_index(next_index);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    status = snapshot_storage_append_log(next_index, &thermal_snapshot);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    if (out_index != 0)
    {
        *out_index = next_index;
    }

    return STORAGE_STATUS_OK;
}

storage_status_t snapshot_storage_get_latest_index(uint32_t *out_index)
{
    if (out_index == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    *out_index = 0U;
    if (storage_service_is_mounted() == 0U)
    {
        return STORAGE_STATUS_NOT_READY;
    }

    return snapshot_storage_read_index(out_index);
}
