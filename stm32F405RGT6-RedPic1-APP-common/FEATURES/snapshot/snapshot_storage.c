/**
 * @file    snapshot_storage.c
 * @brief   热成像快照存储管理模块
 * @note    本模块负责热成像快照的 SD 卡存储管理，包括：
 *          - 快照文件（.RTS）的保存与加载
 *          - BMP 预览图的生成
 *          - 灰度预览帧的构建
 *          - 索引文件（INDEX.TXT）与运行日志（RUN.CSV）的维护
 *          - 全部快照的清除
 *
 * @par 存储目录结构
 *      0:/REDPIC/
 *      ├── INDEX.TXT          — 当前最大快照索引号
 *      ├── RUN.CSV            — 快照运行日志
 *      └── SNAP/
 *          ├── 000001.RTS     — 快照数据文件
 *          ├── 000001.BMP     — 快照 BMP 预览图
 *          ├── 000002.RTS
 *          ├── 000002.BMP
 *          └── ...
 *
 * @par BMP 生成流程
 *      1. 计算温度显示窗口（百分位截断 + headroom）
 *      2. 将像素温度映射为灰度值 [0, 255]
 *      3. 通过调色板将灰度值转换为 RGB565
 *      4. RGB565 转 RGB888 后写入 24-bit BMP 文件
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "snapshot_storage.h"

#include <stdio.h>
#include <string.h>

#include "lcd_dma.h"
#include "ff.h"
#include "redpic1_thermal.h"
#include "thermal_visual.h"
#include "thermal_snapshot_file.h"

/* =========================================================================
 *  2. 内部宏定义
 * ======================================================================= */

#define SNAPSHOT_STORAGE_INDEX_FILE              "0:/REDPIC/INDEX.TXT"       /**< 索引文件路径          */
#define SNAPSHOT_STORAGE_LOG_FILE                "0:/REDPIC/RUN.CSV"         /**< 运行日志路径          */
#define SNAPSHOT_STORAGE_PATH_TEMPLATE           "0:/REDPIC/SNAP/%06lu.RTS"  /**< 快照文件路径模板      */
#define SNAPSHOT_STORAGE_BMP_TEMPLATE            "0:/REDPIC/SNAP/%06lu.BMP"  /**< BMP 文件路径模板      */

#define SNAPSHOT_STORAGE_WIDTH                   32U     /**< 热成像图像宽度            */
#define SNAPSHOT_STORAGE_HEIGHT                  24U     /**< 热成像图像高度            */
#define SNAPSHOT_STORAGE_PIXEL_COUNT             (SNAPSHOT_STORAGE_WIDTH * SNAPSHOT_STORAGE_HEIGHT) /**< 总像素数 */

#define SNAPSHOT_STORAGE_PERCENTILE_LOW_PERMILLE   20U   /**< 低百分位截断（千分位）    */
#define SNAPSHOT_STORAGE_PERCENTILE_HIGH_PERMILLE  995U  /**< 高百分位截断（千分位）    */
#define SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_RATIO 0.08f /**< 高温侧 headroom 比例      */
#define SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_MIN_C 0.4f  /**< headroom 最小值（°C）     */
#define SNAPSHOT_STORAGE_MIN_SPAN_C                1.5f  /**< 显示窗口最小跨度（°C）    */

/* =========================================================================
 *  3. 模块级静态缓冲区
 * ======================================================================= */

static float    s_snapshot_sort_buffer[SNAPSHOT_STORAGE_PIXEL_COUNT];   /**< 排序用临时缓冲区  */
static float    s_snapshot_temp_buffer[SNAPSHOT_STORAGE_PIXEL_COUNT];   /**< 温度临时缓冲区    */
static uint8_t  s_snapshot_gray_buffer[SNAPSHOT_STORAGE_PIXEL_COUNT];   /**< 灰度帧缓冲区      */

/* =========================================================================
 *  4. 内部函数实现 —— 索引文件读写
 * ======================================================================= */

/**
 * @brief  解析无符号 32 位整数（十进制字符串）
 * @param  text — 输入字符串指针
 * @return 解析结果；指针为空或无有效数字时返回 0
 */
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

/**
 * @brief  从索引文件读取当前最大快照索引号
 * @param  out_index — 输出：索引号指针
 * @retval STORAGE_STATUS_OK       — 读取成功（文件不存在时返回索引 0）
 * @retval STORAGE_STATUS_FS_ERROR — 参数错误或打开文件失败
 * @retval STORAGE_STATUS_IO_ERROR — 读取失败
 */
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

    /* 打开索引文件 */
    fr = f_open(&file, (const TCHAR *)SNAPSHOT_STORAGE_INDEX_FILE, FA_READ);
    if (fr == FR_NO_FILE)
    {
        /* 文件不存在：首次使用，索引为 0 */
        return STORAGE_STATUS_OK;
    }
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 读取文件内容 */
    fr = f_read(&file, text, sizeof(text) - 1U, &bytes_done);
    (void)f_close(&file);

    if (fr != FR_OK)
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    /* 解析索引号 */
    text[bytes_done] = '\0';
    *out_index = snapshot_storage_parse_u32(text);

    return STORAGE_STATUS_OK;
}

/**
 * @brief  将索引号写入索引文件
 * @param  index — 待写入的索引号
 * @retval STORAGE_STATUS_OK       — 写入成功
 * @retval STORAGE_STATUS_FS_ERROR — 打开文件失败
 * @retval STORAGE_STATUS_IO_ERROR — 写入失败或写入长度不匹配
 */
static storage_status_t snapshot_storage_write_index(uint32_t index)
{
    FIL file;
    char text[16];
    UINT bytes_done = 0U;
    UINT text_len = 0U;
    FRESULT fr = FR_OK;

    memset(&file, 0, sizeof(file));
    memset(text, 0, sizeof(text));

    /* 格式化索引号 */
    snprintf(text, sizeof(text), "%lu\r\n", (unsigned long)index);
    text_len = (UINT)strlen(text);

    /* 写入文件 */
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

/* =========================================================================
 *  5. 内部函数实现 —— BMP 文件生成辅助
 * ======================================================================= */

/**
 * @brief  计算 BMP 每行字节数（4 字节对齐）
 * @return 每行字节数
 */
static uint32_t snapshot_storage_bmp_row_bytes(void)
{
    uint32_t raw = SNAPSHOT_STORAGE_WIDTH * 3UL;
    return (raw + 3UL) & ~3UL;
}

/**
 * @brief  写入小端 16 位值
 * @param  dst   — 目标地址
 * @param  value — 待写入的值
 */
static void snapshot_storage_write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/**
 * @brief  写入小端 32 位值
 * @param  dst   — 目标地址
 * @param  value — 待写入的值
 */
static void snapshot_storage_write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/* =========================================================================
 *  6. 内部函数实现 —— 温度窗口计算辅助
 * ======================================================================= */

/**
 * @brief  希尔排序（升序）
 * @param  values — 待排序数组
 * @param  count  — 数组长度
 */
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

/**
 * @brief  计算百分位值
 * @param  values   — 已排序数组
 * @param  count    — 数组长度
 * @param  permille — 百分位（千分位，如 20 表示 P2，995 表示 P99.5）
 * @return 百分位对应的值
 */
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

/**
 * @brief  计算温度显示窗口（用于灰度映射）
 * @note   算法：
 *         1. 将像素温度排序
 *         2. 取 P2 ~ P99.5 作为基础窗口
 *         3. 高温侧添加 8% headroom（最小 0.4°C）
 *         4. 确保窗口跨度不小于 1.5°C
 * @param  snapshot    — 输入：热成像快照
 * @param  out_window_min — 输出：窗口下限（°C）
 * @param  out_window_max — 输出：窗口上限（°C）
 */
static void snapshot_storage_get_display_window(const redpic1_thermal_snapshot_t *snapshot,
                                                float *out_window_min,
                                                float *out_window_max)
{
    uint16_t index = 0U;
    float low  = 0.0f;
    float high = 0.0f;
    float span = 0.0f;
    float headroom = 0.0f;

    if (snapshot == 0 || out_window_min == 0 || out_window_max == 0)
    {
        return;
    }

    /* 将像素温度从 ×10 格式转换为浮点 */
    for (index = 0U; index < SNAPSHOT_STORAGE_PIXEL_COUNT; ++index)
    {
        s_snapshot_sort_buffer[index] = (float)snapshot->pixels_x10[index] / 10.0f;
    }

    /* 排序并计算百分位 */
    snapshot_storage_shell_sort(s_snapshot_sort_buffer, SNAPSHOT_STORAGE_PIXEL_COUNT);

    low = snapshot_storage_percentile(s_snapshot_sort_buffer,
                                      SNAPSHOT_STORAGE_PIXEL_COUNT,
                                      SNAPSHOT_STORAGE_PERCENTILE_LOW_PERMILLE);
    high = snapshot_storage_percentile(s_snapshot_sort_buffer,
                                       SNAPSHOT_STORAGE_PIXEL_COUNT,
                                       SNAPSHOT_STORAGE_PERCENTILE_HIGH_PERMILLE);

    /* 百分位无效时回退到 min/max */
    if (high <= low)
    {
        low  = (float)snapshot->min_x10 / 10.0f;
        high = (float)snapshot->max_x10 / 10.0f;
    }

    /* 添加 headroom */
    span = high - low;
    headroom = span * SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_RATIO;
    if (headroom < SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_MIN_C)
    {
        headroom = SNAPSHOT_STORAGE_PERCENTILE_HEADROOM_MIN_C;
    }
    high += headroom;

    /* 确保最小跨度 */
    if ((high - low) < SNAPSHOT_STORAGE_MIN_SPAN_C)
    {
        float center = (high + low) * 0.5f;
        float half = SNAPSHOT_STORAGE_MIN_SPAN_C * 0.5f;

        low  = center - half;
        high = center + half;
    }

    *out_window_min = low;
    *out_window_max = high;
}

/* =========================================================================
 *  7. 内部函数实现 —— 颜色转换辅助
 * ======================================================================= */

/**
 * @brief  将温度值映射为灰度值
 * @param  temp_c     — 温度（°C）
 * @param  window_min — 显示窗口下限
 * @param  window_max — 显示窗口上限
 * @return 灰度值 [0, 255]
 */
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

/**
 * @brief  RGB565 转 RGB888
 * @param  rgb565 — 输入 RGB565 值
 * @param  out_r  — 输出：红色分量 [0, 255]
 * @param  out_g  — 输出：绿色分量 [0, 255]
 * @param  out_b  — 输出：蓝色分量 [0, 255]
 */
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

/* =========================================================================
 *  8. 公共接口实现 —— 灰度预览构建
 * ======================================================================= */

/**
 * @brief  构建灰度预览帧（百分位窗口映射）
 * @note   使用百分位截断算法计算温度窗口，适用于快照存储场景。
 * @param  snapshot   — 输入：热成像快照
 * @param  gray_frame — 输出：灰度帧缓冲区（768 字节）
 * @retval STORAGE_STATUS_OK       — 构建成功
 * @retval STORAGE_STATUS_FS_ERROR — 参数为空
 */
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

    /* 计算温度显示窗口 */
    snapshot_storage_get_display_window(snapshot, &window_min, &window_max);

    /* 逐像素映射为灰度值 */
    for (index = 0U; index < SNAPSHOT_STORAGE_PIXEL_COUNT; ++index)
    {
        float temp_c = (float)snapshot->pixels_x10[index] / 10.0f;
        gray_frame[index] = snapshot_storage_temp_to_gray(temp_c, window_min, window_max);
    }

    return STORAGE_STATUS_OK;
}

/**
 * @brief  构建灰度预览帧（实时可视化模式）
 * @note   使用 thermal_visual 模块的算法，适用于实时查看最新快照。
 * @param  snapshot   — 输入：热成像快照
 * @param  gray_frame — 输出：灰度帧缓冲区（768 字节）
 * @retval STORAGE_STATUS_OK       — 构建成功
 * @retval STORAGE_STATUS_FS_ERROR — 参数为空
 */
storage_status_t snapshot_storage_build_view_latest_gray_preview(const redpic1_thermal_snapshot_t *snapshot,
                                                                 uint8_t *gray_frame)
{
    uint16_t index = 0U;

    if (snapshot == 0 || gray_frame == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 将像素温度从 ×10 格式转换为浮点 */
    for (index = 0U; index < SNAPSHOT_STORAGE_PIXEL_COUNT; ++index)
    {
        s_snapshot_temp_buffer[index] = (float)snapshot->pixels_x10[index] / 10.0f;
    }

    /* 使用 thermal_visual 模块生成灰度帧 */
    redpic1_thermal_visual_prepare_gray_frame(s_snapshot_temp_buffer,
                                              s_snapshot_temp_buffer,
                                              0U,
                                              gray_frame,
                                              0,
                                              0);

    return STORAGE_STATUS_OK;
}

/* =========================================================================
 *  9. 内部函数实现 —— BMP 文件写入
 * ======================================================================= */

/**
 * @brief  将快照保存为 24-bit BMP 文件
 * @note   BMP 格式：14 字节文件头 + 40 字节信息头 + 像素数据（BGR 顺序）。
 *         像素行从底向上排列（BMP 标准要求）。
 * @param  index    — 快照索引号（用于生成文件名）
 * @param  snapshot — 输入：热成像快照
 * @retval STORAGE_STATUS_OK       — 写入成功
 * @retval STORAGE_STATUS_FS_ERROR — 参数为空或创建文件失败
 * @retval STORAGE_STATUS_IO_ERROR — 写入失败
 */
static storage_status_t snapshot_storage_write_bmp(uint32_t index,
                                                   const redpic1_thermal_snapshot_t *snapshot)
{
    FIL file;
    char path[32];
    uint8_t file_header[14];
    uint8_t info_header[40];
    uint8_t row_buf[96];
    UINT bytes_done = 0U;
    uint32_t row_bytes   = snapshot_storage_bmp_row_bytes();
    uint32_t pixel_bytes = row_bytes * SNAPSHOT_STORAGE_HEIGHT;
    uint32_t file_size   = 14UL + 40UL + pixel_bytes;
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

    /* 先构建灰度帧 */
    status = snapshot_storage_build_gray_preview(snapshot, s_snapshot_gray_buffer);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    /* 生成文件路径 */
    snprintf(path, sizeof(path), SNAPSHOT_STORAGE_BMP_TEMPLATE, (unsigned long)index);

    /* 构建 BMP 文件头（14 字节） */
    file_header[0] = 'B';
    file_header[1] = 'M';
    snapshot_storage_write_le32(&file_header[2], file_size);     /* 文件大小          */
    snapshot_storage_write_le32(&file_header[10], 54UL);         /* 像素数据偏移      */

    /* 构建 BMP 信息头（40 字节） */
    snapshot_storage_write_le32(&info_header[0],  40UL);                             /* 信息头大小  */
    snapshot_storage_write_le32(&info_header[4],  SNAPSHOT_STORAGE_WIDTH);            /* 图像宽度    */
    snapshot_storage_write_le32(&info_header[8],  SNAPSHOT_STORAGE_HEIGHT);           /* 图像高度    */
    snapshot_storage_write_le16(&info_header[12], 1U);                               /* 平面数      */
    snapshot_storage_write_le16(&info_header[14], 24U);                              /* 位深度      */
    snapshot_storage_write_le32(&info_header[20], pixel_bytes);                      /* 像素数据大小*/

    /* 创建 BMP 文件 */
    fr = f_open(&file, (const TCHAR *)path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 写入文件头 */
    fr = f_write(&file, file_header, sizeof(file_header), &bytes_done);
    if (fr != FR_OK || bytes_done != sizeof(file_header))
    {
        (void)f_close(&file);
        return STORAGE_STATUS_IO_ERROR;
    }

    /* 写入信息头 */
    fr = f_write(&file, info_header, sizeof(info_header), &bytes_done);
    if (fr != FR_OK || bytes_done != sizeof(info_header))
    {
        (void)f_close(&file);
        return STORAGE_STATUS_IO_ERROR;
    }

    /* 逐行写入像素数据（从底向上） */
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

            /* BMP 像素顺序为 BGR */
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

/* =========================================================================
 *  10. 公共接口实现 —— 快照加载
 * ======================================================================= */

/**
 * @brief  加载最新的热成像快照
 * @param  out_snapshot — 输出：热成像快照指针
 * @param  out_index    — 输出：快照索引号（可选，可为 NULL）
 * @retval STORAGE_STATUS_OK          — 加载成功
 * @retval STORAGE_STATUS_NO_SNAPSHOT — 无快照数据
 * @retval STORAGE_STATUS_FS_ERROR    — 文件系统错误
 * @retval STORAGE_STATUS_IO_ERROR    — 读取失败
 */
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

    /* 获取最新索引号 */
    status = snapshot_storage_get_latest_index(&latest_index);
    if (status != STORAGE_STATUS_OK || latest_index == 0U)
    {
        return (status == STORAGE_STATUS_OK) ? STORAGE_STATUS_NO_SNAPSHOT : status;
    }

    /* 打开快照文件 */
    snprintf(path, sizeof(path), SNAPSHOT_STORAGE_PATH_TEMPLATE, (unsigned long)latest_index);
    fr = f_open(&file, (const TCHAR *)path, FA_READ);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 读取文件数据 */
    fr = f_read(&file, &file_snapshot, sizeof(file_snapshot), &bytes_done);
    (void)f_close(&file);

    if (fr != FR_OK || bytes_done != sizeof(file_snapshot))
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    /* 反序列化 */
    if (thermal_snapshot_file_parse(out_snapshot, &file_snapshot) == 0U)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 输出索引号 */
    if (out_index != 0)
    {
        *out_index = latest_index;
    }

    return STORAGE_STATUS_OK;
}

/* =========================================================================
 *  11. 内部函数实现 —— 运行日志追加
 * ======================================================================= */

/**
 * @brief  向运行日志文件追加一条快照记录
 * @note   日志格式：索引,时间戳,最低温度,最高温度,中心温度
 * @param  index    — 快照索引号
 * @param  snapshot — 输入：热成像快照
 * @retval STORAGE_STATUS_OK       — 写入成功
 * @retval STORAGE_STATUS_FS_ERROR — 参数为空或文件操作失败
 * @retval STORAGE_STATUS_IO_ERROR — 写入失败
 */
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

    /* 格式化日志行 */
    snprintf(line,
             sizeof(line),
             "%06lu,%lu,%d,%d,%d\r\n",
             (unsigned long)index,
             (unsigned long)snapshot->timestamp_ms,
             snapshot->min_x10,
             snapshot->max_x10,
             snapshot->center_x10);
    line_len = (UINT)strlen(line);

    /* 打开日志文件（追加模式） */
    fr = f_open(&file, (const TCHAR *)SNAPSHOT_STORAGE_LOG_FILE, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 定位到文件末尾 */
    fr = f_lseek(&file, f_size(&file));
    if (fr != FR_OK)
    {
        (void)f_close(&file);
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 写入日志行 */
    fr = f_write(&file, line, line_len, &bytes_done);
    (void)f_close(&file);

    if (fr != FR_OK || bytes_done != line_len)
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    return STORAGE_STATUS_OK;
}

/* =========================================================================
 *  12. 公共接口实现 —— 按索引加载
 * ======================================================================= */

/**
 * @brief  获取快照总数（即最新索引号）
 * @param  out_count — 输出：快照总数指针
 * @retval STORAGE_STATUS_OK       — 获取成功
 * @retval STORAGE_STATUS_FS_ERROR — 参数为空
 */
storage_status_t snapshot_storage_get_count(uint32_t *out_count)
{
    if (out_count == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    return snapshot_storage_get_latest_index(out_count);
}

/**
 * @brief  按索引加载指定快照
 * @param  index        — 快照索引号（从 1 开始）
 * @param  out_snapshot — 输出：热成像快照指针
 * @retval STORAGE_STATUS_OK          — 加载成功
 * @retval STORAGE_STATUS_NOT_READY   — SD 卡未挂载
 * @retval STORAGE_STATUS_FS_ERROR    — 文件系统错误
 * @retval STORAGE_STATUS_IO_ERROR    — 读取失败
 */
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

    /* 检查 SD 卡挂载状态 */
    if (storage_service_is_mounted() == 0U)
    {
        return STORAGE_STATUS_NOT_READY;
    }

    /* 打开快照文件 */
    snprintf(path, sizeof(path), SNAPSHOT_STORAGE_PATH_TEMPLATE, (unsigned long)index);
    fr = f_open(&file, (const TCHAR *)path, FA_READ);
    if (fr != FR_OK)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 读取文件数据 */
    fr = f_read(&file, &file_snapshot, sizeof(file_snapshot), &bytes_done);
    (void)f_close(&file);

    if (fr != FR_OK || bytes_done != sizeof(file_snapshot))
    {
        return STORAGE_STATUS_IO_ERROR;
    }

    /* 反序列化 */
    if (thermal_snapshot_file_parse(out_snapshot, &file_snapshot) == 0U)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    return STORAGE_STATUS_OK;
}

/* =========================================================================
 *  13. 公共接口实现 —— 快照保存
 * ======================================================================= */

/**
 * @brief  保存当前最新的热成像快照到 SD 卡
 * @note   流程：
 *         1. 确保目录结构存在
 *         2. 从热成像模块获取最新快照
 *         3. 读取当前索引号并递增
 *         4. 序列化并写入 .RTS 文件
 *         5. 更新索引文件
 *         6. 追加运行日志
 *         7. 生成 BMP 预览图
 * @param  out_index — 输出：保存的快照索引号（可选，可为 NULL）
 * @retval STORAGE_STATUS_OK          — 保存成功
 * @retval STORAGE_STATUS_NO_SNAPSHOT — 无快照数据
 * @retval STORAGE_STATUS_FS_ERROR    — 文件系统错误
 * @retval STORAGE_STATUS_IO_ERROR    — 写入失败
 */
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

    /* 确保目录结构存在 */
    status = storage_service_ensure_redpic_dirs();
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    /* 获取最新热成像快照 */
    if (redpic1_thermal_copy_latest_snapshot(&thermal_snapshot) == 0U)
    {
        return STORAGE_STATUS_NO_SNAPSHOT;
    }

    /* 读取当前索引号并递增 */
    status = snapshot_storage_read_index(&last_index);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    next_index = last_index + 1UL;

    /* 序列化为文件格式 */
    thermal_snapshot_file_fill(&file_snapshot, &thermal_snapshot);

    /* 写入 .RTS 文件 */
    snprintf(path, sizeof(path), SNAPSHOT_STORAGE_PATH_TEMPLATE, (unsigned long)next_index);
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

    /* 更新索引文件 */
    status = snapshot_storage_write_index(next_index);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    /* 追加运行日志 */
    status = snapshot_storage_append_log(next_index, &thermal_snapshot);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    /* 生成 BMP 预览图 */
    status = snapshot_storage_write_bmp(next_index, &thermal_snapshot);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    /* 输出索引号 */
    if (out_index != 0)
    {
        *out_index = next_index;
    }

    return STORAGE_STATUS_OK;
}

/* =========================================================================
 *  14. 公共接口实现 —— 索引查询与清除
 * ======================================================================= */

/**
 * @brief  获取最新快照的索引号
 * @param  out_index — 输出：索引号指针
 * @retval STORAGE_STATUS_OK        — 获取成功
 * @retval STORAGE_STATUS_NOT_READY — SD 卡未挂载
 * @retval STORAGE_STATUS_FS_ERROR  — 参数为空
 */
storage_status_t snapshot_storage_get_latest_index(uint32_t *out_index)
{
    if (out_index == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    *out_index = 0U;

    /* 检查 SD 卡挂载状态 */
    if (storage_service_is_mounted() == 0U)
    {
        return STORAGE_STATUS_NOT_READY;
    }

    return snapshot_storage_read_index(out_index);
}

/**
 * @brief  清除所有快照数据
 * @note   删除所有 .RTS 文件、.BMP 文件、索引文件和运行日志。
 *         文件不存在时不报错（FR_NO_FILE）。
 * @retval STORAGE_STATUS_OK        — 清除成功
 * @retval STORAGE_STATUS_NOT_READY — SD 卡未挂载
 * @retval STORAGE_STATUS_FS_ERROR  — 删除文件失败
 */
storage_status_t snapshot_storage_clear_all(void)
{
    uint32_t latest_index = 0U;
    uint32_t index = 0U;
    char path[32];
    FRESULT fr = FR_OK;
    storage_status_t status = STORAGE_STATUS_OK;

    /* 检查 SD 卡挂载状态 */
    if (storage_service_is_mounted() == 0U)
    {
        return STORAGE_STATUS_NOT_READY;
    }

    /* 读取当前最大索引号 */
    status = snapshot_storage_read_index(&latest_index);
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    /* 逐个删除快照文件和 BMP 文件 */
    memset(path, 0, sizeof(path));
    for (index = 1U; index <= latest_index; ++index)
    {
        /* 删除 .RTS 文件 */
        snprintf(path, sizeof(path), SNAPSHOT_STORAGE_PATH_TEMPLATE, (unsigned long)index);
        fr = f_unlink((const TCHAR *)path);
        if (fr != FR_OK && fr != FR_NO_FILE)
        {
            return STORAGE_STATUS_FS_ERROR;
        }

        /* 删除 .BMP 文件 */
        snprintf(path, sizeof(path), SNAPSHOT_STORAGE_BMP_TEMPLATE, (unsigned long)index);
        fr = f_unlink((const TCHAR *)path);
        if (fr != FR_OK && fr != FR_NO_FILE)
        {
            return STORAGE_STATUS_FS_ERROR;
        }
    }

    /* 删除索引文件 */
    fr = f_unlink((const TCHAR *)SNAPSHOT_STORAGE_INDEX_FILE);
    if (fr != FR_OK && fr != FR_NO_FILE)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 删除运行日志 */
    fr = f_unlink((const TCHAR *)SNAPSHOT_STORAGE_LOG_FILE);
    if (fr != FR_OK && fr != FR_NO_FILE)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    return STORAGE_STATUS_OK;
}
