/**
 * @file    boot_info_v3.h
 * @brief   BootInfo 结构体定义 —— 引导信息的持久化存储格式
 * @note    本头文件定义 BootInfo 的存储布局，包括：
 *          - 布局魔数与版本号（V2/V3）
 *          - 分区槽位标识与试启动状态
 *          - 启动魔数与升级标志
 *          - V3 结构体（BootInfoTypeDef）：完整字段，含确认分区、
 *            试启动计数、回滚计数、多版本槽位、最低允许版本等
 *          - V2 遗留结构体（BootInfoV2Legacy）：兼容旧版布局
 *
 * @par V3 布局特点
 *      - 支持 A/B 双分区方案（BOOT_INFO_SLOT_APP1/APP2）
 *      - 支持试启动机制（trial_state + boot_tries）
 *      - 支持确认分区（confirmed_slot）与回滚计数
 *      - 支持最低允许 OTA 版本（min_allowed_ota_version）
 *      - 支持待定最低版本（pending_floor_version）
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef BOOT_INFO_V3_H
#define BOOT_INFO_V3_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 *  1. 布局魔数与版本号
 * ======================================================================= */

/** V2 布局魔数："BIV2"（0x42495632） */
#define BOOT_INFO_LAYOUT_MAGIC_V2      0x42495632UL

/** V3 布局魔数："BIV3"（0x42495633） */
#define BOOT_INFO_LAYOUT_MAGIC_V3      0x42495633UL

/** V3 布局版本号 */
#define BOOT_INFO_LAYOUT_VERSION_V3    3U

/** 版本字符串最大长度（含终止符） */
#define BOOT_INFO_VERSION_LEN          16U

/** 分区槽位数量 */
#define BOOT_INFO_SLOT_COUNT           2U

/* =========================================================================
 *  2. 分区槽位标识
 * ======================================================================= */

#define BOOT_INFO_SLOT_APP1            0U         /**< APP1 分区槽位     */
#define BOOT_INFO_SLOT_APP2            1U         /**< APP2 分区槽位     */

/* =========================================================================
 *  3. 试启动状态
 * ======================================================================= */

#define BOOT_INFO_TRIAL_NONE           0U         /**< 无试启动         */
#define BOOT_INFO_TRIAL_PENDING        1U         /**< 试启动进行中     */

/* =========================================================================
 *  4. 启动魔数
 * ======================================================================= */

#define BOOT_MAGIC_NORMAL              0x00000000UL   /**< 正常启动     */
#define BOOT_MAGIC_REQUEST             0xDEADBEAFUL   /**< 升级/回滚请求 */
#define BOOT_MAGIC_NEW_FW              0xBEEF0000UL   /**< 新固件就绪   */

/* =========================================================================
 *  5. 升级标志
 * ======================================================================= */

#define BOOT_UPGRADE_FLAG_NONE         0U         /**< 无升级操作       */
#define BOOT_UPGRADE_FLAG_UPGRADE      1U         /**< 执行升级         */
#define BOOT_UPGRADE_FLAG_ROLLBACK     2U         /**< 执行回滚         */

/* =========================================================================
 *  6. V3 BootInfo 结构体
 * ======================================================================= */

/**
 * @brief  V3 BootInfo 结构体 —— 引导信息的完整存储格式
 * @note   包含布局元数据、启动控制、分区管理、版本信息等。
 *         通过 data_crc32 字段保证数据完整性。
 */
typedef struct
{
    /* --- 布局元数据 --- */
    uint32_t layout_magic;                              /**< 布局魔数（BIV3）         */
    uint16_t layout_version;                            /**< 布局版本号               */
    uint16_t struct_size;                               /**< 结构体大小               */
    uint32_t data_crc32;                                /**< 数据区 CRC32 校验        */

    /* --- 启动控制 --- */
    uint32_t boot_magic;                                /**< 启动魔数                 */
    uint32_t upgrade_flag;                              /**< 升级标志                 */
    uint32_t active_slot;                               /**< 当前活跃分区             */
    uint32_t target_slot;                               /**< 目标升级分区             */
    uint32_t confirmed_slot;                            /**< 已确认分区               */
    uint32_t trial_state;                               /**< 试启动状态               */
    uint32_t boot_tries;                                /**< 剩余试启动次数           */
    uint32_t rollback_counter;                          /**< 回滚计数器               */

    /* --- 版本信息 --- */
    char current_version[BOOT_INFO_VERSION_LEN];        /**< 当前固件版本             */
    char slot_versions[BOOT_INFO_SLOT_COUNT][BOOT_INFO_VERSION_LEN]; /**< 各分区版本  */
    char last_good_version[BOOT_INFO_VERSION_LEN];      /**< 最后已知良好版本         */
    char min_allowed_ota_version[BOOT_INFO_VERSION_LEN]; /**< 最低允许 OTA 版本       */
    char pending_floor_version[BOOT_INFO_VERSION_LEN];   /**< 待定最低版本             */

    /* --- 保留字段 --- */
    uint32_t reserved[8];                               /**< 保留（未来扩展）         */
} BootInfoTypeDef;

/* =========================================================================
 *  7. V2 遗留 BootInfo 结构体（兼容旧版）
 * ======================================================================= */

/**
 * @brief  V2 遗留 BootInfo 结构体
 * @note   仅用于从 V2 格式迁移到 V3，不建议在新代码中使用。
 */
typedef struct
{
    /* --- 布局元数据 --- */
    uint32_t layout_magic;                              /**< 布局魔数（BIV2）         */
    uint16_t layout_version;                            /**< 布局版本号               */
    uint16_t struct_size;                               /**< 结构体大小               */
    uint32_t data_crc32;                                /**< 数据区 CRC32 校验        */

    /* --- 启动控制 --- */
    uint32_t boot_magic;                                /**< 启动魔数                 */
    uint32_t upgrade_flag;                              /**< 升级标志                 */
    uint32_t active_partition;                          /**< 当前活跃分区             */
    uint32_t target_partition;                          /**< 目标升级分区             */
    uint32_t boot_tries;                                /**< 剩余试启动次数           */
    uint32_t trial_complete;                            /**< 试启动完成标志           */

    /* --- 版本信息 --- */
    char current_version[BOOT_INFO_VERSION_LEN];        /**< 当前固件版本             */
    char app1_version[BOOT_INFO_VERSION_LEN];           /**< APP1 版本                */
    char app2_version[BOOT_INFO_VERSION_LEN];           /**< APP2 版本                */

    /* --- 保留字段 --- */
    uint32_t reserved[4];                               /**< 保留（未来扩展）         */
} BootInfoV2Legacy;

#ifdef __cplusplus
}
#endif

#endif /* BOOT_INFO_V3_H */
