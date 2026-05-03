/**
 * @file storage_service.c
 * @brief SD卡存储服务模块 —— 基于FatFs文件系统的存储抽象层。
 *
 * 本模块封装了SD卡的初始化、挂载、文件读写、容量查询等操作，
 * 为上层应用提供统一的存储接口。同时处理低功耗模式下的SD卡安全卸载与恢复。
 *
 * 功能概述：
 *   1. SD卡初始化与FatFs文件系统挂载
 *   2. 应用目录结构自动创建（/REDPIC、/REDPIC/SNAP）
 *   3. 读写测试文件（用于验证SD卡读写完整性）
 *   4. 容量查询（总容量/可用空间）
 *   5. 低功耗模式前的安全卸载（同步缓存、卸载文件系统）
 *   6. STOP唤醒后的会话失效（需重新挂载）
 *
 * FatFs关键概念：
 *   - FATFS: 文件系统对象，包含卷信息、FAT表缓存等
 *   - FIL: 文件对象，包含文件指针、簇链位置等
 *   - FRESULT: FatFs操作返回码（FR_OK=成功）
 *   - f_mount: 挂载/卸载文件系统
 */
#include "storage_service.h"

#include <stdio.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"

/* ===================================================================== */
/*                            常量定义                                     */
/* ===================================================================== */

/** FatFs逻辑驱动器路径（驱动器0） */
#define STORAGE_SERVICE_DRIVE_PATH       "0:"

/** RedPic应用根目录路径 */
#define STORAGE_SERVICE_ROOT_DIR         "0:/REDPIC"

/** 热成像快照存储目录路径 */
#define STORAGE_SERVICE_SNAPSHOT_DIR     "0:/REDPIC/SNAP"

/** SD卡读写测试文件路径 */
#define STORAGE_SERVICE_TEST_FILE_PATH   "0:/REDPIC/TEST.TXT"

/** 测试文件内容（用于验证读写完整性） */
#define STORAGE_SERVICE_TEST_TEXT        "RedPic SD OK\r\n"

/* ===================================================================== */
/*                            静态变量                                     */
/* ===================================================================== */

/** FatFs文件系统对象（挂载后由FatFs管理） */
static FATFS s_storage_fs;

/** 存储服务信息（卡状态、容量、挂载状态等） */
static storage_info_t s_storage_info;

/** 模块初始化标志，0=未初始化，1=已初始化 */
static uint8_t s_storage_inited = 0U;

/* ===================================================================== */
/*                           内部辅助函数                                   */
/* ===================================================================== */

/**
 * @brief 查询并更新SD卡容量信息（总容量和可用空间）
 *
 * 通过FatFs的f_getfree()接口获取空闲簇数，再根据FAT参数计算容量：
 *   总扇区数 = (最大簇数 - 2) * 每簇扇区数
 *   空闲扇区数 = 空闲簇数 * 每簇扇区数
 *   容量(KB) = 扇区数 / 2（假设每扇区512字节）
 *
 * 注意：如果_MAX_SS != 512（扇区大小非512字节），需要额外换算。
 *
 * @param info 输出参数，更新total_kb和free_kb字段
 * @return STORAGE_STATUS_OK=成功，STORAGE_STATUS_FS_ERROR=查询失败
 */
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

    /* 查询指定驱动器的空闲簇数 */
    fr = f_getfree((const TCHAR *)STORAGE_SERVICE_DRIVE_PATH, &free_clusters, &fs);
    if (fr != FR_OK || fs == 0)
    {
        return STORAGE_STATUS_FS_ERROR;
    }

    /* 计算总扇区数和空闲扇区数 */
    total_sectors = (uint32_t)(fs->n_fatent - 2U) * (uint32_t)fs->csize;
    free_sectors = (uint32_t)free_clusters * (uint32_t)fs->csize;

    /* 如果扇区大小不是512字节，需要按比例换算 */
#if _MAX_SS != 512
    total_sectors *= (uint32_t)fs->ssize / 512UL;
    free_sectors *= (uint32_t)fs->ssize / 512UL;
#endif

    /* 扇区数右移1位 = 除以2，得到KB数（每扇区512字节 = 0.5KB） */
    info->total_kb = total_sectors >> 1;
    info->free_kb = free_sectors >> 1;
    return STORAGE_STATUS_OK;
}

/**
 * @brief 创建目录（如果不存在）
 *
 * 使用f_mkdir创建目录。如果目录已存在（FR_EXIST），不视为错误。
 *
 * @param path 目录路径
 * @return STORAGE_STATUS_OK=成功或已存在，STORAGE_STATUS_FS_ERROR=创建失败
 */
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

/**
 * @brief 低功耗模式前的SD卡安全关闭
 *
 * 操作序列：
 *   1. 如果未初始化，仅反初始化磁盘驱动
 *   2. 如果已挂载：
 *      a. 执行CTRL_SYNC（将FatFs缓存中的数据写入SD卡）
 *      b. 卸载文件系统（f_mount传NULL）
 *   3. 清零FATFS对象
 *   4. 反初始化磁盘驱动（关闭SDIO/DMA等底层硬件）
 *   5. 更新状态为未挂载、未就绪
 *
 * 为什么需要：STOP/STANDBY模式会关闭SDIO时钟，
 * 如果不先卸载文件系统，缓存中的数据会丢失。
 */
static void storage_service_prepare_low_power(void)
{
    if (s_storage_inited == 0U)
    {
        (void)disk_deinitialize(0);
        return;
    }

    if (s_storage_info.mounted != 0U)
    {
        /* 同步：将FatFs缓存中的数据写入SD卡 */
        (void)disk_ioctl(0, CTRL_SYNC, 0);
        /* 卸载文件系统（f_mount传NULL） */
        (void)f_mount((FATFS *)0, (const TCHAR *)STORAGE_SERVICE_DRIVE_PATH, 0);
    }

    /* 清零FATFS对象，防止唤醒后使用脏数据 */
    memset(&s_storage_fs, 0, sizeof(s_storage_fs));
    (void)disk_deinitialize(0);
    s_storage_info.mounted = 0U;
    s_storage_info.last_status = STORAGE_STATUS_NOT_READY;
}

/* ===================================================================== */
/*                           公共API实现                                   */
/* ===================================================================== */

/**
 * @brief 存储服务初始化
 *
 * 清零所有内部状态，设置初始状态为未就绪。
 * 在系统启动时调用（main()或初始化序列中）。
 */
void storage_service_init(void)
{
    memset(&s_storage_fs, 0, sizeof(s_storage_fs));
    memset(&s_storage_info, 0, sizeof(s_storage_info));
    s_storage_info.last_status = STORAGE_STATUS_NOT_READY;
    s_storage_inited = 1U;
}

/**
 * @brief 挂载SD卡文件系统
 *
 * 完整挂载序列：
 *   1. 检查模块是否已初始化（未初始化则自动初始化）
 *   2. 检查是否已挂载（已挂载则直接返回成功）
 *   3. 初始化SD卡磁盘驱动（disk_initialize）
 *   4. 挂载FatFs文件系统（f_mount，自动检测FAT类型）
 *
 * @return 1=挂载成功，0=挂载失败
 */
uint8_t storage_service_mount(void)
{
    DSTATUS disk_status_value = 0;
    FRESULT fr = FR_OK;

    /* 确保模块已初始化 */
    if (s_storage_inited == 0U)
    {
        storage_service_init();
    }

    /* 已挂载则直接返回成功 */
    if (s_storage_info.mounted != 0U)
    {
        return 1U;
    }

    /* 初始化SD卡磁盘驱动（底层SDIO通信） */
    disk_status_value = disk_initialize(0);
    if ((disk_status_value & STA_NOINIT) != 0U)
    {
        /* 初始化失败（卡未插入或硬件故障） */
        s_storage_info.card_present = 0U;
        s_storage_info.mounted = 0U;
        s_storage_info.last_status = STORAGE_STATUS_INIT_FAIL;
        return 0U;
    }

    s_storage_info.card_present = 1U;

    /* 挂载FatFs文件系统（参数1=立即挂载） */
    fr = f_mount(&s_storage_fs, (const TCHAR *)STORAGE_SERVICE_DRIVE_PATH, 1);
    if (fr != FR_OK)
    {
        /* 挂载失败（文件系统格式不支持或SD卡损坏） */
        s_storage_info.mounted = 0U;
        s_storage_info.last_status = STORAGE_STATUS_MOUNT_FAIL;
        return 0U;
    }

    s_storage_info.mounted = 1U;
    s_storage_info.last_status = STORAGE_STATUS_OK;
    return 1U;
}

/**
 * @brief 查询SD卡是否已挂载
 * @return 1=已挂载，0=未挂载
 */
uint8_t storage_service_is_mounted(void)
{
    return s_storage_info.mounted;
}

/**
 * @brief STOP模式前的存储准备（同步+卸载+关闭硬件）
 *
 * 由PM客户端回调调用：low_power_runtime → app_pm → storage_service
 */
void storage_service_prepare_for_stop(void)
{
    storage_service_prepare_low_power();
}

/**
 * @brief STANDBY模式前的存储准备（同STOP准备）
 *
 * 由PM客户端回调调用：low_power_runtime → app_pm → storage_service
 */
void storage_service_prepare_for_standby(void)
{
    storage_service_prepare_low_power();
}

/**
 * @brief STOP唤醒后使存储会话失效
 *
 * STOP模式关闭了SDIO时钟，唤醒后底层硬件状态已丢失。
 * 此函数将挂载状态标记为无效，下次使用SD卡时需要重新挂载。
 *
 * 注意：不清除card_present标志，因为物理卡仍在位。
 */
void storage_service_invalidate_session_after_stop(void)
{
    if (s_storage_inited == 0U)
    {
        return;
    }

    s_storage_info.mounted = 0U;
    memset(&s_storage_fs, 0, sizeof(s_storage_fs));
    s_storage_info.last_status = STORAGE_STATUS_NOT_READY;
}

/**
 * @brief 获取存储信息（卡状态、容量等）
 *
 * @param info 输出参数，接收当前存储信息的副本。可为NULL。
 * @return 当前存储状态枚举值
 */
storage_status_t storage_service_get_info(storage_info_t *info)
{
    if (s_storage_inited == 0U)
    {
        storage_service_init();
    }

    if (info != 0)
    {
        *info = s_storage_info;
    }

    return s_storage_info.last_status;
}

/**
 * @brief 查询SD卡容量（总容量和可用空间）
 *
 * 需要先挂载文件系统。如果未挂载，返回STORAGE_STATUS_NOT_READY。
 * 查询成功后会更新s_storage_info中的total_kb和free_kb字段。
 *
 * @param info 输出参数，接收容量信息。可为NULL。
 * @return 查询结果状态
 */
storage_status_t storage_service_query_capacity(storage_info_t *info)
{
    storage_status_t status = STORAGE_STATUS_NOT_READY;

    if (s_storage_inited == 0U)
    {
        storage_service_init();
    }

    /* 未挂载则返回NOT_READY */
    if (s_storage_info.mounted == 0U)
    {
        s_storage_info.last_status = STORAGE_STATUS_NOT_READY;
        if (info != 0)
        {
            *info = s_storage_info;
        }
        return s_storage_info.last_status;
    }

    /* 查询并更新容量信息 */
    status = storage_service_update_capacity(&s_storage_info);
    s_storage_info.last_status = status;

    if (info != 0)
    {
        *info = s_storage_info;
    }

    return status;
}

/**
 * @brief 确保RedPic应用目录结构存在
 *
 * 自动创建以下目录（如果不存在）：
 *   - 0:/REDPIC        （应用根目录）
 *   - 0:/REDPIC/SNAP   （热成像快照目录）
 *
 * 如果SD卡未挂载，会先尝试挂载。
 *
 * @return STORAGE_STATUS_OK=目录就绪，其他=错误
 */
storage_status_t storage_service_ensure_redpic_dirs(void)
{
    storage_status_t status = STORAGE_STATUS_OK;

    /* 确保SD卡已挂载 */
    if (storage_service_mount() == 0U)
    {
        return s_storage_info.last_status;
    }

    /* 创建根目录 */
    status = storage_service_mkdir_if_needed(STORAGE_SERVICE_ROOT_DIR);
    if (status == STORAGE_STATUS_OK)
    {
        /* 根目录创建成功后，创建快照目录 */
        status = storage_service_mkdir_if_needed(STORAGE_SERVICE_SNAPSHOT_DIR);
    }

    s_storage_info.last_status = status;
    return status;
}

/**
 * @brief 写入测试文件（验证SD卡写入功能）
 *
 * 写入流程：
 *   1. 确保目录结构存在
 *   2. 创建/覆盖测试文件
 *   3. 写入测试文本
 *   4. 关闭文件
 *   5. 验证写入字节数
 *
 * 测试文件路径：0:/REDPIC/TEST.TXT
 * 测试内容："RedPic SD OK\r\n"
 *
 * @return STORAGE_STATUS_OK=写入成功，其他=错误
 */
storage_status_t storage_service_write_test_file(void)
{
    FIL file;
    UINT bytes_done = 0U;
    UINT text_len = (UINT)strlen(STORAGE_SERVICE_TEST_TEXT);
    FRESULT fr = FR_OK;
    storage_status_t status = STORAGE_STATUS_OK;

    memset(&file, 0, sizeof(file));

    /* 确保目录结构存在（会触发挂载） */
    status = storage_service_ensure_redpic_dirs();
    if (status != STORAGE_STATUS_OK)
    {
        return status;
    }

    /* 创建/覆盖测试文件 */
    fr = f_open(&file, (const TCHAR *)STORAGE_SERVICE_TEST_FILE_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        s_storage_info.last_status = STORAGE_STATUS_FS_ERROR;
        return s_storage_info.last_status;
    }

    /* 写入测试文本 */
    fr = f_write(&file, STORAGE_SERVICE_TEST_TEXT, text_len, &bytes_done);
    (void)f_close(&file);

    /* 验证写入结果 */
    if (fr != FR_OK || bytes_done != text_len)
    {
        s_storage_info.last_status = STORAGE_STATUS_IO_ERROR;
        return s_storage_info.last_status;
    }

    s_storage_info.last_status = STORAGE_STATUS_OK;
    return STORAGE_STATUS_OK;
}

/**
 * @brief 读取测试文件并验证内容（验证SD卡读取功能）
 *
 * 读取流程：
 *   1. 检查SD卡是否已挂载
 *   2. 打开测试文件
 *   3. 读取文件内容到缓冲区
 *   4. 关闭文件
 *   5. 与预期内容逐字节比较
 *
 * @return STORAGE_STATUS_OK=读取验证成功，其他=错误
 */
storage_status_t storage_service_read_test_file(void)
{
    FIL file;
    UINT bytes_done = 0U;
    UINT text_len = (UINT)strlen(STORAGE_SERVICE_TEST_TEXT);
    char readback[24]; /* 读取缓冲区（比测试文本大1字节，用于null终止） */
    FRESULT fr = FR_OK;

    memset(&file, 0, sizeof(file));
    memset(readback, 0, sizeof(readback));

    /* 检查SD卡是否已挂载 */
    if (s_storage_info.mounted == 0U)
    {
        s_storage_info.last_status = STORAGE_STATUS_NOT_READY;
        return s_storage_info.last_status;
    }

    /* 以只读方式打开测试文件 */
    fr = f_open(&file, (const TCHAR *)STORAGE_SERVICE_TEST_FILE_PATH, FA_READ);
    if (fr != FR_OK)
    {
        s_storage_info.last_status = STORAGE_STATUS_FS_ERROR;
        return s_storage_info.last_status;
    }

    /* 读取文件内容（留1字节给null终止符） */
    fr = f_read(&file, readback, sizeof(readback) - 1U, &bytes_done);
    (void)f_close(&file);

    /* 验证读取结果 */
    if (fr != FR_OK || bytes_done < text_len)
    {
        s_storage_info.last_status = STORAGE_STATUS_IO_ERROR;
        return s_storage_info.last_status;
    }

    /* 逐字节比较读取内容与预期内容 */
    if (strncmp(readback, STORAGE_SERVICE_TEST_TEXT, text_len) != 0)
    {
        s_storage_info.last_status = STORAGE_STATUS_IO_ERROR;
        return s_storage_info.last_status;
    }

    s_storage_info.last_status = STORAGE_STATUS_OK;
    return STORAGE_STATUS_OK;
}

/**
 * @brief 获取存储状态对应的文本描述
 *
 * 用于UI显示或日志输出，将状态枚举值转换为人类可读的字符串。
 *
 * @param status 状态枚举值
 * @return 状态描述字符串（英文）
 */
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
