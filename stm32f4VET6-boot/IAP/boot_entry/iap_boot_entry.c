/**
 * @file    iap_boot_entry.c
 * @brief   IAP 引导入口 —— 升级/回滚/试启动逻辑
 * @note    本模块实现 bootloader 的核心启动决策逻辑，
 *          根据 BootInfo 中的 magic、upgrade_flag、trial_state 等字段，
 *          决定执行升级、回滚、试启动还是正常跳转应用。
 *
 * @par 启动流程
 *      1. 加载 BootInfo，清除过期事务记录
 *      2. 校验 target_partition 合法性
 *      3. 若 magic=REQUEST + upgrade_flag=UPGRADE → 执行 IAP 升级
 *      4. 若 magic=REQUEST + upgrade_flag=ROLLBACK → 执行回滚
 *      5. 若 IWDG 复位 + 试启动状态 → 递减试启动计数
 *         - 计数 > 0：再次尝试跳转活跃分区
 *         - 计数 = 0：回滚到已确认分区
 *      6. 尝试跳转活跃分区 → 已确认分区 → 非活跃分区
 *      7. 全部失败：显示启动画面，进入看门狗喂狗循环
 *
 * @par 分区方案
 *      - APP1（活跃分区 1）
 *      - APP2（活跃分区 2）
 *      - 每个分区有独立的 BootInfo 记录
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "iap_boot_entry.h"

#include "iap_boot_info.h"
#include "iap_platform.h"
#include "iap_ui.h"

/* =========================================================================
 *  2. 内部函数实现 —— 过期事务检测与清除
 * ======================================================================= */

/**
 * @brief  检查事务记录是否与当前应用提交匹配
 * @note   匹配条件：
 *         1. 事务状态为 RECEIVED / AUTHORIZED / COMMITTED
 *         2. 目标分区 = 当前活跃分区
 *         3. 目标版本 = 当前版本
 * @param  txn       — 事务记录指针
 * @param  boot_info — BootInfo 指针
 * @retval 1 — 匹配（事务已过期）；0 — 不匹配
 */
static uint8_t iap_boot_txn_matches_current_app_commit(const OtaTxnRecord *txn,
                                                       const BootInfoTypeDef *boot_info)
{
    if (txn == 0 || boot_info == 0)
    {
        return 0U;
    }

    /* 检查事务状态 */
    if (txn->state != IAP_TXN_STATE_RECEIVED &&
        txn->state != IAP_TXN_STATE_AUTHORIZED &&
        txn->state != IAP_TXN_STATE_COMMITTED)
    {
        return 0U;
    }

    /* 检查目标分区 */
    if (txn->target_slot != boot_info->active_partition)
    {
        return 0U;
    }

    /* 检查版本匹配 */
    if (strcmp(txn->target_version, boot_info->current_version) != 0)
    {
        return 0U;
    }

    return 1U;
}

/**
 * @brief  尝试清除与当前应用匹配的过期事务记录
 * @note   若事务记录的目标分区和版本与当前活跃应用一致，
 *         说明该事务已完成，可以安全清除。
 * @param  boot_info — BootInfo 指针
 */
static void iap_boot_try_clear_stale_completed_txn(const BootInfoTypeDef *boot_info)
{
    OtaTxnRecord txn;

    if (boot_info == 0)
    {
        return;
    }

    txn_load(&txn);
    if (iap_boot_txn_matches_current_app_commit(&txn, boot_info) == 0U)
    {
        return;
    }

    (void)txn_clear();
}

/* =========================================================================
 *  3. 公共接口实现 —— 引导入口主函数
 * ======================================================================= */

/**
 * @brief  IAP 引导入口主函数
 * @note   启动决策逻辑（按优先级执行）：
 *         1. 清除过期事务，校验分区合法性
 *         2. 升级请求 → iap_main() 执行升级
 *         3. 回滚请求 → 切换到已确认分区并跳转
 *         4. IWDG 复位 + 试启动 → 递减计数或回滚
 *         5. 尝试跳转活跃分区
 *         6. 尝试跳转已确认分区
 *         7. 尝试跳转非活跃分区
 *         8. 全部失败 → 启动画面 + 看门狗喂狗循环
 *
 * @warning 本函数不会返回（正常路径直接跳转应用）
 */
void iap_boot_entry_run(void)
{
    BootInfoTypeDef boot_info;
    uint8_t reset_reason;
    uint32_t rollback_slot = OTA_CTRL_PARTITION_APP1;

    /* --- 步骤 1：初始化与清理 --- */
    reset_reason = get_reset_reason();
    clear_reset_flags();
    boot_info_load(&boot_info);
    iap_boot_try_clear_stale_completed_txn(&boot_info);

    /* 校验 target_partition 合法性 */
    if (boot_info.target_partition > OTA_CTRL_PARTITION_APP2 ||
        boot_info.target_partition == boot_info.active_partition)
    {
        boot_info.target_partition = boot_info_inactive_partition(boot_info.active_partition);
    }

    /* --- 步骤 2：升级请求处理 --- */
    if (boot_info.boot_magic == MAGIC_REQUEST &&
        boot_info.upgrade_flag == BOOT_UPGRADE_FLAG_UPGRADE)
    {
        iap_ui_boot_prepare((reset_reason == BOOT_REASON_SOFTWARE) ? 1U : 0U);
        iap_main();             /* 执行 IAP 升级流程 */
        NVIC_SystemReset();     /* 升级完成后复位 */
    }

    /* --- 步骤 3：回滚请求处理 --- */
    if (boot_info.boot_magic == MAGIC_REQUEST &&
        boot_info.upgrade_flag == BOOT_UPGRADE_FLAG_ROLLBACK)
    {
        iap_ui_boot_prepare((reset_reason == BOOT_REASON_SOFTWARE) ? 1U : 0U);
        iap_ui_show_upgrade_prepare();

        rollback_slot = boot_info_inactive_partition(boot_info.active_partition);

        /* 切换到已确认分区并验证应用有效性 */
        if (boot_info_switch_to_confirmed_slot(&boot_info, rollback_slot) != 0U &&
            is_app_valid(boot_info_partition_address(rollback_slot)) != 0U)
        {
            boot_info_save(&boot_info);
            APPLICATION_ADDRESS = boot_info_partition_address(rollback_slot);
            iap_ui_show_upgrade_success();
            delay_ms(500);
            iap_init_watchdog();
            jump_to_app(APPLICATION_ADDRESS);
            while (1)
            {
            }
        }

        /* 回滚失败：清除升级标志 */
        boot_info.upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
        boot_info.boot_magic = MAGIC_NORMAL;
        boot_info_save(&boot_info);
    }

    /* --- 步骤 4：启动画面（上电/IWDG 复位） --- */
    if (reset_reason == BOOT_REASON_POWER || reset_reason == BOOT_REASON_IWDG)
    {
        iap_ui_boot_prepare(0U);
        iap_ui_run_normal_boot_2s((reset_reason == BOOT_REASON_IWDG) ? 1U : 0U);
    }

    /* --- 步骤 5：试启动逻辑（IWDG 复位 + 试启动状态） --- */
    if (reset_reason == BOOT_REASON_IWDG && boot_info.trial_state == BOOT_INFO_TRIAL_PENDING)
    {
        if (boot_info.boot_tries > 0U)
        {
            --boot_info.boot_tries;
        }

        if (boot_info.boot_tries > 0U)
        {
            /* 仍有试启动机会：保存计数并跳转活跃分区 */
            boot_info_save(&boot_info);
            APPLICATION_ADDRESS = boot_info_partition_address(boot_info.active_partition);
            if (is_app_valid(APPLICATION_ADDRESS))
            {
                iap_init_watchdog();
                jump_to_app(APPLICATION_ADDRESS);
                while (1)
                {
                }
            }
        }
        else
        {
            /* 试启动次数耗尽：回滚到已确认分区 */
            rollback_slot = boot_info.confirmed_slot;
            if (boot_info_switch_to_confirmed_slot(&boot_info, rollback_slot) != 0U &&
                is_app_valid(boot_info_partition_address(rollback_slot)) != 0U)
            {
                ++boot_info.rollback_counter;
                boot_info_save(&boot_info);
                APPLICATION_ADDRESS = boot_info_partition_address(rollback_slot);
                iap_init_watchdog();
                jump_to_app(APPLICATION_ADDRESS);
                while (1)
                {
                }
            }
        }
    }

    /* --- 步骤 6：尝试跳转活跃分区 --- */
    APPLICATION_ADDRESS = boot_info_partition_address(boot_info.active_partition);
    if (is_app_valid(APPLICATION_ADDRESS))
    {
        iap_init_watchdog();
        jump_to_app(APPLICATION_ADDRESS);
        while (1)
        {
        }
    }

    /* --- 步骤 7：尝试跳转已确认分区 --- */
    rollback_slot = boot_info.confirmed_slot;
    APPLICATION_ADDRESS = boot_info_partition_address(rollback_slot);
    if (rollback_slot != boot_info.active_partition &&
        boot_info_switch_to_confirmed_slot(&boot_info, rollback_slot) != 0U &&
        is_app_valid(APPLICATION_ADDRESS))
    {
        boot_info_save(&boot_info);
        iap_init_watchdog();
        jump_to_app(APPLICATION_ADDRESS);
        while (1)
        {
        }
    }

    /* --- 步骤 8：尝试跳转非活跃分区 --- */
    rollback_slot = boot_info_inactive_partition(boot_info.active_partition);
    APPLICATION_ADDRESS = boot_info_partition_address(rollback_slot);
    if (boot_info_switch_to_confirmed_slot(&boot_info, rollback_slot) != 0U &&
        is_app_valid(APPLICATION_ADDRESS))
    {
        boot_info_save(&boot_info);
        iap_init_watchdog();
        jump_to_app(APPLICATION_ADDRESS);
        while (1)
        {
        }
    }

    /* --- 步骤 9：所有分区均无效 → 显示启动画面，进入看门狗喂狗循环 --- */
    iap_ui_show_boot_splash();
    iap_init_watchdog();
    while (1)
    {
        iap_feed_watchdog();
        delay_ms(100);
    }
}
