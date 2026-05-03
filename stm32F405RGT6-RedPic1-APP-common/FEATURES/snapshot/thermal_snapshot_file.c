/**
 * @file    thermal_snapshot_file.c
 * @brief   热成像快照文件序列化/反序列化模块
 * @note    本模块负责将热成像快照数据与文件存储格式（.RTS）之间的相互转换。
 *          文件格式采用 CRC16 校验保证数据完整性。
 *
 * @par 文件格式（redpic_snapshot_t）
 *      | 字段        | 类型    | 说明                          |
 *      |-------------|---------|-------------------------------|
 *      | magic       | uint32  | 魔数 0x31535452 ("RTS1")      |
 *      | version     | uint16  | 格式版本号（当前为 1）         |
 *      | width       | uint16  | 图像宽度（32）                |
 *      | height      | uint16  | 图像高度（24）                |
 *      | min_x10     | int16   | 最低温度 ×10                  |
 *      | max_x10     | int16   | 最高温度 ×10                  |
 *      | center_x10  | int16   | 中心温度 ×10                  |
 *      | frame_id    | uint32  | 帧编号                        |
 *      | timestamp   | uint32  | 时间戳（ms）                  |
 *      | crc16       | uint16  | CRC16 校验码                  |
 *      | pixels_x10  | int16[] | 像素温度数组（32×24=768）     |
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "thermal_snapshot_file.h"

#include <string.h>

/* =========================================================================
 *  2. 公共接口实现 —— CRC16 校验
 * ======================================================================= */

/**
 * @brief  计算 CRC16 校验值（CCITT-FALSE 算法）
 * @note   多项式 0x1021，初始值 0xFFFF，MSB 优先。
 * @param  data   — 输入数据指针
 * @param  length — 数据长度（字节）
 * @return 计算得到的 CRC16 值；指针为空时返回 0
 */
uint16_t thermal_snapshot_file_crc16(const uint8_t *data, uint32_t length)
{
    uint16_t crc  = 0xFFFFU;
    uint32_t i    = 0U;
    uint32_t bit  = 0U;

    if (data == 0)
    {
        return 0U;
    }

    for (i = 0U; i < length; ++i)
    {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);

        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((crc & 0x8000U) != 0U)
            {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

/* =========================================================================
 *  3. 公共接口实现 —— 序列化
 * ======================================================================= */

/**
 * @brief  将热成像快照序列化为文件格式
 * @note   流程：清零 → 填充头部字段 → 拷贝像素数据 → 计算并写入 CRC16。
 * @param  file_snapshot    — 输出：文件格式快照指针
 * @param  thermal_snapshot — 输入：热成像快照指针
 */
void thermal_snapshot_file_fill(redpic_snapshot_t *file_snapshot,
                                const redpic1_thermal_snapshot_t *thermal_snapshot)
{
    uint16_t index = 0U;

    if (file_snapshot == 0 || thermal_snapshot == 0)
    {
        return;
    }

    memset(file_snapshot, 0, sizeof(*file_snapshot));

    /* 填充文件头 */
    file_snapshot->magic        = REDPIC_RTS_MAGIC;
    file_snapshot->version      = REDPIC_RTS_VERSION;
    file_snapshot->width        = REDPIC_RTS_WIDTH;
    file_snapshot->height       = REDPIC_RTS_HEIGHT;

    /* 填充温度统计数据 */
    file_snapshot->min_x10      = thermal_snapshot->min_x10;
    file_snapshot->max_x10      = thermal_snapshot->max_x10;
    file_snapshot->center_x10   = thermal_snapshot->center_x10;

    /* 填充帧信息 */
    file_snapshot->frame_id     = thermal_snapshot->frame_id;
    file_snapshot->timestamp    = thermal_snapshot->timestamp_ms;

    /* CRC16 字段先清零，后续统一计算 */
    file_snapshot->crc16 = 0U;

    /* 拷贝像素温度数据 */
    for (index = 0U; index < REDPIC_RTS_PIXELS; ++index)
    {
        file_snapshot->pixels_x10[index] = thermal_snapshot->pixels_x10[index];
    }

    /* 计算并写入 CRC16 校验码 */
    file_snapshot->crc16 = thermal_snapshot_file_crc16((const uint8_t *)file_snapshot,
                                                       (uint32_t)sizeof(*file_snapshot));
}

/* =========================================================================
 *  4. 公共接口实现 —— 反序列化
 * ======================================================================= */

/**
 * @brief  将文件格式快照反序列化为热成像快照
 * @note   流程：校验 magic/version/width/height → 校验 CRC16 → 拷贝数据。
 * @param  thermal_snapshot — 输出：热成像快照指针
 * @param  file_snapshot    — 输入：文件格式快照指针
 * @retval 1 — 解析成功；0 — 解析失败（数据损坏或格式不匹配）
 */
uint8_t thermal_snapshot_file_parse(redpic1_thermal_snapshot_t *thermal_snapshot,
                                    const redpic_snapshot_t *file_snapshot)
{
    uint16_t index = 0U;
    redpic_snapshot_t temp;

    if (thermal_snapshot == 0 || file_snapshot == 0)
    {
        return 0U;
    }

    /* 拷贝到临时缓冲区以避免修改原始数据 */
    memcpy(&temp, file_snapshot, sizeof(temp));

    /* 文件头校验 */
    if (temp.magic != REDPIC_RTS_MAGIC ||
        temp.version != REDPIC_RTS_VERSION ||
        temp.width != REDPIC_RTS_WIDTH ||
        temp.height != REDPIC_RTS_HEIGHT)
    {
        return 0U;
    }

    /* CRC16 校验：将 crc16 字段清零后重新计算 */
    temp.crc16 = 0U;
    if (thermal_snapshot_file_crc16((const uint8_t *)&temp,
                                    (uint32_t)sizeof(temp)) != file_snapshot->crc16)
    {
        return 0U;
    }

    /* 填充输出结构体 */
    memset(thermal_snapshot, 0, sizeof(*thermal_snapshot));
    thermal_snapshot->valid         = 1U;
    thermal_snapshot->frame_id      = file_snapshot->frame_id;
    thermal_snapshot->timestamp_ms  = file_snapshot->timestamp;
    thermal_snapshot->min_x10       = file_snapshot->min_x10;
    thermal_snapshot->max_x10       = file_snapshot->max_x10;
    thermal_snapshot->center_x10    = file_snapshot->center_x10;

    /* 拷贝像素温度数据 */
    for (index = 0U; index < REDPIC_RTS_PIXELS; ++index)
    {
        thermal_snapshot->pixels_x10[index] = file_snapshot->pixels_x10[index];
    }

    return 1U;
}
