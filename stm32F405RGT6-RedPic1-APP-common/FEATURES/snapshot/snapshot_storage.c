#include "snapshot_storage.h"

#include <stdio.h>
#include <string.h>

#include "lcd_dma.h"
#include "ff.h"
#include "redpic1_thermal.h"
#include "thermal_visual.h"
#include "thermal_snapshot_file.h"

#define SNAPSHOT_STORAGE_INDEX_FILE      "0:/REDPIC/INDEX.TXT"
#define SNAPSHOT_STORAGE_LOG_FILE        "0:/REDPIC/RUN.CSV"
#define SNAPSHOT_STORAGE_PATH_TEMPLATE   "0:/REDPIC/SNAP/%06lu.RTS"
#define SNAPSHOT_STORAGE_BMP_TEMPLATE    "0:/REDPIC/SNAP/%06lu.BMP"
#define SNAPSHOT_STORAGE_WIDTH           32U
#define SNAPSHOT_STORAGE_HEIGHT          24U
#define SNAPSHOT_STORAGE_PIXEL_COUNT     (SNAPSHOT_STORAGE_WIDTH * SNAPSHOT_STORAGE_HEIGHT)
#define SNAPSHOT_STORAGE_PERCENTILE_LOW_PERMILLE   20U
#define SNAPSHOT_STORAGE_PERCENTILE_HIGH_PERMILLE  995U
#define SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_RATIO 0.08f
#define SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_MIN_C 0.4f
#define SNAPSHOT_STORAGE_MIN_SPAN_C                1.5f

static float s_snapshot_sort_buffer[SNAPSHOT_STORAGE_PIXEL_COUNT];
static float s_snapshot_temp_buffer[SNAPSHOT_STORAGE_PIXEL_COUNT];
static uint8_t s_snapshot_gray_buffer[SNAPSHOT_STORAGE_PIXEL_COUNT];

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

static uint32_t snapshot_storage_bmp_row_bytes(void)
{
    uint32_t raw = SNAPSHOT_STORAGE_WIDTH * 3UL;
    return (raw + 3UL) & ~3UL;
}

static void snapshot_storage_write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void snapshot_storage_write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void snapshot_storage_shell_sort(float *values, uint16_t count)
{
    uint16_t gap = count / 2U;

    while (gap != 0U)
    {
        uint16_t i = 0U;

        for (i = gap; i < count; ++i)
        {
            float temp = values[i];
            uint16_t j = i;

            while (j >= gap && values[j - gap] > temp)
            {
                values[j] = values[j - gap];
                j = (uint16_t)(j - gap);
            }

            values[j] = temp;
        }

        gap = (gap == 1U) ? 0U : (uint16_t)(gap / 2U);
    }
}

static float snapshot_storage_percentile(float *values, uint16_t count, uint16_t permille)
{
    uint16_t index = 0U;

    if (count == 0U)
    {
        return 0.0f;
    }

    index = (uint16_t)((((uint32_t)(count - 1U)) * permille) / 1000UL);
    return values[index];
}

static void snapshot_storage_get_display_window(const redpic1_thermal_snapshot_t *snapshot,
                                                float *out_window_min,
                                                float *out_window_max)
{
    uint16_t index = 0U;
    float low = 0.0f;
    float high = 0.0f;
    float span = 0.0f;
    float headroom = 0.0f;

    if (snapshot == 0 || out_window_min == 0 || out_window_max == 0)
    {
        return;
    }

    for (index = 0U; index < SNAPSHOT_STORAGE_PIXEL_COUNT; ++index)
    {
        s_snapshot_sort_buffer[index] = (float)snapshot->pixels_x10[index] / 10.0f;
    }

    snapshot_storage_shell_sort(s_snapshot_sort_buffer, SNAPSHOT_STORAGE_PIXEL_COUNT);
    low = snapshot_storage_percentile(s_snapshot_sort_buffer,
                                      SNAPSHOT_STORAGE_PIXEL_COUNT,
                                      SNAPSHOT_STORAGE_PERCENTILE_LOW_PERMILLE);
    high = snapshot_storage_percentile(s_snapshot_sort_buffer,
                                       SNAPSHOT_STORAGE_PIXEL_COUNT,
                                       SNAPSHOT_STORAGE_PERCENTILE_HIGH_PERMILLE);

    if (high <= low)
    {
        low = (float)snapshot->min_x10 / 10.0f;
        high = (float)snapshot->max_x10 / 10.0f;
    }

    span = high - low;
    headroom = span * SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_RATIO;
    if (headroom < SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_MIN_C)
    {
        headroom = SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_MIN_C;
    }
    high += headroom;

    if ((high - low) < SNAPSHOT_STORAGE_MIN_SPAN_C)
    {
        float center = (high + low) * 0.5f;
        float half = SNAPSHOT_STORAGE_MIN_SPAN_C * 0.5f;

        low = center - half;
        high = center + half;
    }

    *out_window_min = low;
    *out_window_max = high;
}

static uint8_t snapshot_storage_temp_to_gray(float temp_c, float window_min, float window_max)
{
    int32_t gray_value = 0;

    if (temp_c <= window_min)
    {
        return 0U;
    }
    if (temp_c >= window_max)
    {
        return 255U;
    }

    gray_value = (int32_t)(((temp_c - window_min) * 255.0f) / (window_max - window_min));
    if (gray_value < 0)
    {
        gray_value = 0;
    }
    else if (gray_value > 255)
    {
        gray_value = 255;
    }

    return (uint8_t)gray_value;
}

static void snapshot_storage_rgb565_to_rgb888(uint16_t rgb565,
                                              uint8_t *out_r,
                                              uint8_t *out_g,
                                              uint8_t *out_b)
{
    uint8_t r5 = (uint8_t)((rgb565 >> 11) & 0x1FU);
    uint8_t g6 = (uint8_t)((rgb565 >> 5) & 0x3FU);
    uint8_t b5 = (uint8_t)(rgb565 & 0x1FU);

    if (out_r != 0)
    {
        *out_r = (uint8_t)((r5 * 255U) / 31U);
    }
    if (out_g != 0)
    {
        *out_g = (uint8_t)((g6 * 255U) / 63U);
    }
    if (out_b != 0)
    {
        *out_b = (uint8_t)((b5 * 255U) / 31U);
    }
}

storage_status_t snapshot_storage_build_gray_preview(const redpic1_thermal_snapshot_t *snapshot,
                                                     uint8_t *gray_frame)
{
    uint16_t index = 0U;
    float window_min = 0.0f;
    float window_max = 0.0f;

    if (snapshot == 0 || gray_frame == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    snapshot_storage_get_display_window(snapshot, &window_min, &window_max);

    for (index = 0U; index < SNAPSHOT_STORAGE_PIXEL_COUNT; ++index)
    {
        float temp_c = (float)snapshot->pixels_x10[index] / 10.0f;
        gray_frame[index] = snapshot_storage_temp_to_gray(temp_c, window_min, window_max);
    }

    return STORAGE_STATUS_OK;
}

storage_status_t snapshot_storage_build_view_latest_gray_preview(const redpic1_thermal_snapshot_t *snapshot,
                                                                 uint8_t *gray_frame)
{
    uint16_t index = 0U;

    if (snapshot == 0 || gray_frame == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    for (index = 0U; index < SNAPSHOT_STORAGE_PIXEL_COUNT; ++index)
    {
        s_snapshot_temp_buffer[index] = (float)snapshot->pixels_x10[index] / 10.0f;
    }

    redpic1_thermal_visual_prepare_gray_frame(s_snapshot_temp_buffer,
                                              s_snapshot_temp_buffer,
                                              0U,
                                              gray_frame,
                                              0,
                                              0);

    return STORAGE_STATUS_OK;
}

static storage_status_t snapshot_storage_write_bmp(uint32_t index,
                                                   const redpic1_thermal_snapshot_t *snapshot)
{
    FIL file;
    char path[32];
    uint8_t file_header[14];
    uint8_t info_header[40];
    uint8_t row_buf[96];
    UINT bytes_done = 0U;
    uint32_t row_bytes = snapshot_storage_bmp_row_bytes();
    uint32_t pixel_bytes = row_bytes * SNAPSHOT_STORAGE_HEIGHT;
    uint32_t file_size = 14UL + 40UL + pixel_bytes;
    uint16_t row = 0U;
    FRESULT fr = FR_OK;
    storage_status_t status = STORAGE_STATUS_OK;

    if (snapshot == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    memset(&file, 0, sizeof(file));
    memset(path, 0, sizeof(path));
    memset(file_header, 0, sizeof(file_header));
    memset(info_header, 0, sizeof(info_header));

    status = snapshot_storage_build_gray_preview(snapshot, s_snapshot_gray_buffer);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    snprintf(path, sizeof(path), SNAPSHOT_STORAGE_BMP_TEMPLATE, (unsigned long)index);

    file_header[0] = 'B';
    file_header[1] = 'M';
    snapshot_storage_write_le32(&file_header[2], file_size);
    snapshot_storage_write_le32(&file_header[10], 54UL);

    snapshot_storage_write_le32(&info_header[0], 40UL);
    snapshot_storage_write_le32(&info_header[4], SNAPSHOT_STORAGE_WIDTH);
    snapshot_storage_write_le32(&info_header[8], SNAPSHOT_STORAGE_HEIGHT);
    snapshot_storage_write_le16(&info_header[12], 1U);
    snapshot_storage_write_le16(&info_header[14], 24U);
    snapshot_storage_write_le32(&info_header[20], pixel_bytes);

    fr = f_open(&file, (const TCHAR *)path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_write(&file, file_header, sizeof(file_header), &bytes_done);
    if (fr != FR_OK || bytes_done != sizeof(file_header))
    {
        (void)f_close(&file);
        return STORAGE_STATUS_IO_ERROR;
    }

    fr = f_write(&file, info_header, sizeof(info_header), &bytes_done);
    if (fr != FR_OK || bytes_done != sizeof(info_header))
    {
        (void)f_close(&file);
        return STORAGE_STATUS_IO_ERROR;
    }

    for (row = 0U; row < SNAPSHOT_STORAGE_HEIGHT; ++row)
    {
        uint16_t src_row = (uint16_t)(SNAPSHOT_STORAGE_HEIGHT - 1U - row);
        uint16_t col = 0U;

        memset(row_buf, 0, sizeof(row_buf));
        for (col = 0U; col < SNAPSHOT_STORAGE_WIDTH; ++col)
        {
            uint16_t idx = (uint16_t)(src_row * SNAPSHOT_STORAGE_WIDTH + col);
            uint16_t rgb565 = lcd_dma_palette_color_rgb565(s_snapshot_gray_buffer[idx]);
            uint8_t r = 0U;
            uint8_t g = 0U;
            uint8_t b = 0U;
            uint32_t pixel_offset = (uint32_t)col * 3UL;

            snapshot_storage_rgb565_to_rgb888(rgb565, &r, &g, &b);
            row_buf[pixel_offset + 0U] = b;
            row_buf[pixel_offset + 1U] = g;
            row_buf[pixel_offset + 2U] = r;
        }

        fr = f_write(&file, row_buf, row_bytes, &bytes_done);
        if (fr != FR_OK || bytes_done != row_bytes)
        {
            (void)f_close(&file);
            return STORAGE_STATUS_IO_ERROR;
        }
    }

    (void)f_close(&file);
    return STORAGE_STATUS_OK;
}

storage_status_t snapshot_storage_load_latest(redpic1_thermal_snapshot_t *out_snapshot,
                                              uint32_t *out_index)
{
    FIL file;
    redpic_snapshot_t file_snapshot;
    char path[32];
    UINT bytes_done = 0U;
    uint32_t latest_index = 0U;
    FRESULT fr = FR_OK;
    storage_status_t status = STORAGE_STATUS_OK;

    if (out_snapshot == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    memset(&file, 0, sizeof(file));
    memset(&file_snapshot, 0, sizeof(file_snapshot));
    memset(path, 0, sizeof(path));

    status = snapshot_storage_get_latest_index(&latest_index);
    if (status != STORAGE_STATUS_OK || latest_index == 0U)
    {
        return (status == STORAGE_STATUS_OK) ? STORAGE_STATUS_NO_SNAPSHOT : status;
    }

    snprintf(path, sizeof(path), SNAPSHOT_STORAGE_PATH_TEMPLATE, (unsigned long)latest_index);
    fr = f_open(&file, (const TCHAR *)path, FA_READ);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_read(&file, &file_snapshot, sizeof(file_snapshot), &bytes_done);
    (void)f_close(&file);
    if (fr != FR_OK || bytes_done != sizeof(file_snapshot))
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    if (thermal_snapshot_file_parse(out_snapshot, &file_snapshot) == 0U)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    if (out_index != 0)
    {
        *out_index = latest_index;
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

storage_status_t snapshot_storage_get_count(uint32_t *out_count)
{
    if (out_count == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    return snapshot_storage_get_latest_index(out_count);
}

storage_status_t snapshot_storage_load_index(uint32_t index,
                                             redpic1_thermal_snapshot_t *out_snapshot)
{
    FIL file;
    redpic_snapshot_t file_snapshot;
    char path[32];
    UINT bytes_done = 0U;
    FRESULT fr = FR_OK;

    if (out_snapshot == 0 || index == 0U)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    memset(&file, 0, sizeof(file));
    memset(&file_snapshot, 0, sizeof(file_snapshot));
    memset(path, 0, sizeof(path));

    if (storage_service_is_mounted() == 0U)
    {
        return STORAGE_STATUS_NOT_READY;
    }

    snprintf(path, sizeof(path), SNAPSHOT_STORAGE_PATH_TEMPLATE, (unsigned long)index);
    fr = f_open(&file, (const TCHAR *)path, FA_READ);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_read(&file, &file_snapshot, sizeof(file_snapshot), &bytes_done);
    (void)f_close(&file);
    if (fr != FR_OK || bytes_done != sizeof(file_snapshot))
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    if (thermal_snapshot_file_parse(out_snapshot, &file_snapshot) == 0U)
    {
        return STORAGE_STATUS_FS_ERROR;
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

    status = snapshot_storage_write_bmp(next_index, &thermal_snapshot);
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

storage_status_t snapshot_storage_clear_all(void)
{
    uint32_t latest_index = 0U;
    uint32_t index = 0U;
    char path[32];
    FRESULT fr = FR_OK;
    storage_status_t status = STORAGE_STATUS_OK;

    if (storage_service_is_mounted() == 0U)
    {
        return STORAGE_STATUS_NOT_READY;
    }

    status = snapshot_storage_read_index(&latest_index);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    memset(path, 0, sizeof(path));
    for (index = 1U; index <= latest_index; ++index)
    {
        snprintf(path, sizeof(path), SNAPSHOT_STORAGE_PATH_TEMPLATE, (unsigned long)index);
        fr = f_unlink((const TCHAR *)path);
        if (fr != FR_OK && fr != FR_NO_FILE)
        {
            return STORAGE_STATUS_FS_ERROR;
        }

        snprintf(path, sizeof(path), SNAPSHOT_STORAGE_BMP_TEMPLATE, (unsigned long)index);
        fr = f_unlink((const TCHAR *)path);
        if (fr != FR_OK && fr != FR_NO_FILE)
        {
            return STORAGE_STATUS_FS_ERROR;
        }
    }

    fr = f_unlink((const TCHAR *)SNAPSHOT_STORAGE_INDEX_FILE);
    if (fr != FR_OK && fr != FR_NO_FILE)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    fr = f_unlink((const TCHAR *)SNAPSHOT_STORAGE_LOG_FILE);
    if (fr != FR_OK && fr != FR_NO_FILE)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    return STORAGE_STATUS_OK;
}
