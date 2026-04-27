#include "iap_boot_entry.h"

#include "iap_boot_info.h"
#include "iap_platform.h"
#include "iap_ui.h"

static uint8_t iap_boot_txn_matches_current_app_commit(const OtaTxnRecord *txn,
                                                       const BootInfoTypeDef *boot_info)
{
    if (txn == 0 || boot_info == 0)
    {
        return 0U;
    }

    if (txn->state != IAP_TXN_STATE_RECEIVED &&
        txn->state != IAP_TXN_STATE_AUTHORIZED &&
        txn->state != IAP_TXN_STATE_COMMITTED)
    {
        return 0U;
    }

    if (txn->target_slot != boot_info->active_partition)
    {
        return 0U;
    }

    if (strcmp(txn->target_version, boot_info->current_version) != 0)
    {
        return 0U;
    }

    return 1U;
}

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

void iap_boot_entry_run(void)
{
    BootInfoTypeDef boot_info;
    uint8_t reset_reason;
    uint32_t rollback_slot = OTA_CTRL_PARTITION_APP1;

    reset_reason = get_reset_reason();
    clear_reset_flags();
    boot_info_load(&boot_info);
    iap_boot_try_clear_stale_completed_txn(&boot_info);

    if (boot_info.target_partition > OTA_CTRL_PARTITION_APP2 ||
        boot_info.target_partition == boot_info.active_partition)
    {
        boot_info.target_partition = boot_info_inactive_partition(boot_info.active_partition);
    }

    if (boot_info.boot_magic == MAGIC_REQUEST &&
        boot_info.upgrade_flag == BOOT_UPGRADE_FLAG_UPGRADE)
    {
        iap_ui_boot_prepare((reset_reason == BOOT_REASON_SOFTWARE) ? 1U : 0U);
        iap_main();
        NVIC_SystemReset();
    }

    if (boot_info.boot_magic == MAGIC_REQUEST &&
        boot_info.upgrade_flag == BOOT_UPGRADE_FLAG_ROLLBACK)
    {
        iap_ui_boot_prepare((reset_reason == BOOT_REASON_SOFTWARE) ? 1U : 0U);
        iap_ui_show_upgrade_prepare();
        rollback_slot = boot_info_inactive_partition(boot_info.active_partition);
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

        boot_info.upgrade_flag = BOOT_UPGRADE_FLAG_NONE;
        boot_info.boot_magic = MAGIC_NORMAL;
        boot_info_save(&boot_info);
    }

    if (reset_reason == BOOT_REASON_POWER || reset_reason == BOOT_REASON_IWDG)
    {
        iap_ui_boot_prepare(0U);
        iap_ui_run_normal_boot_2s((reset_reason == BOOT_REASON_IWDG) ? 1U : 0U);
    }

    if (reset_reason == BOOT_REASON_IWDG && boot_info.trial_state == BOOT_INFO_TRIAL_PENDING)
    {
        if (boot_info.boot_tries > 0U)
        {
            --boot_info.boot_tries;
        }

        if (boot_info.boot_tries > 0U)
        {
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

    APPLICATION_ADDRESS = boot_info_partition_address(boot_info.active_partition);
    if (is_app_valid(APPLICATION_ADDRESS))
    {
        iap_init_watchdog();
        jump_to_app(APPLICATION_ADDRESS);
        while (1)
        {
        }
    }

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

    iap_ui_show_boot_splash();
    iap_init_watchdog();
    while (1)
    {
        iap_feed_watchdog();
        delay_ms(100);
    }
}
