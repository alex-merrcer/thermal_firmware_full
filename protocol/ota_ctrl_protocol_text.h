/**
 * @file    ota_ctrl_protocol_text.h
 * @brief   OTA 控制协议文本名称查找 —— 调试与日志辅助
 * @note    本头文件提供 OTA 控制协议各枚举值的文本名称映射，
 *          用于串口日志输出和调试诊断。包含：
 *          - 消息类型名称（REQ/ACK/STATUS/READY/ERROR/META/RESULT/GO/CANCEL）
 *          - 升级阶段名称（IDLE/QUERY/DOWNLOAD/VER_SIG/VER_CRC/AES_PREP/READY/TRANSFER/DONE）
 *          - 错误码名称（BUSY/NO_WIFI/FETCH_PKG/...）
 *
 * @version 2.0
 * @date    2026-05-01
 */

#ifndef OTA_CTRL_PROTOCOL_TEXT_H
#define OTA_CTRL_PROTOCOL_TEXT_H

#include "ota_ctrl_protocol.h"

/* =========================================================================
 *  1. 消息类型名称查找
 * ======================================================================= */

/**
 * @brief  获取 OTA 控制消息类型的文本名称
 * @param  msg_type — 消息类型码（OTA_CTRL_MSG_xxx）
 * @return 消息类型名称字符串
 */
static const char *ota_ctrl_msg_name(uint8_t msg_type)
{
    switch (msg_type)
    {
        case OTA_CTRL_MSG_REQ:      return "REQ";
        case OTA_CTRL_MSG_CANCEL:   return "CANCEL";
        case OTA_CTRL_MSG_GO:       return "GO";
        case OTA_CTRL_MSG_ACK:      return "ACK";
        case OTA_CTRL_MSG_STATUS:   return "STATUS";
        case OTA_CTRL_MSG_READY:    return "READY";
        case OTA_CTRL_MSG_ERROR:    return "ERROR";
        case OTA_CTRL_MSG_META:     return "META";
        case OTA_CTRL_MSG_RESULT:   return "RESULT";
        default:                    return "UNKNOWN";
    }
}

/* =========================================================================
 *  2. 升级阶段名称查找
 * ======================================================================= */

/**
 * @brief  获取升级阶段的文本名称
 * @param  stage — 阶段码（OTA_CTRL_STAGE_xxx）
 * @return 阶段名称字符串
 */
static const char *ota_ctrl_stage_name(uint8_t stage)
{
    switch (stage)
    {
        case OTA_CTRL_STAGE_IDLE:        return "IDLE";
        case OTA_CTRL_STAGE_QUERY:       return "QUERY";
        case OTA_CTRL_STAGE_DOWNLOAD:    return "DOWNLOAD";
        case OTA_CTRL_STAGE_VERIFY_SIG:  return "VER_SIG";
        case OTA_CTRL_STAGE_VERIFY_CRC:  return "VER_CRC";
        case OTA_CTRL_STAGE_AES_PREPARE: return "AES_PREP";
        case OTA_CTRL_STAGE_READY:       return "READY";
        case OTA_CTRL_STAGE_TRANSFER:    return "TRANSFER";
        case OTA_CTRL_STAGE_DONE:        return "DONE";
        default:                         return "UNKNOWN";
    }
}

/* =========================================================================
 *  3. 错误码名称查找
 * ======================================================================= */

/**
 * @brief  获取错误码的文本名称
 * @param  error_code — 错误码（OTA_CTRL_ERR_xxx）
 * @return 错误码名称字符串
 */
static const char *ota_ctrl_error_name(uint16_t error_code)
{
    switch (error_code)
    {
        case OTA_CTRL_ERR_BUSY:           return "BUSY";
        case OTA_CTRL_ERR_NO_WIFI:        return "NO_WIFI";
        case OTA_CTRL_ERR_FETCH_PACKAGE:  return "FETCH_PKG";
        case OTA_CTRL_ERR_NO_PACKAGE:     return "NO_PKG";
        case OTA_CTRL_ERR_PARTITION:      return "PARTITION";
        case OTA_CTRL_ERR_PRODUCT:        return "PRODUCT";
        case OTA_CTRL_ERR_HW_REV:         return "HW_REV";
        case OTA_CTRL_ERR_SIGNATURE:      return "SIGNATURE";
        case OTA_CTRL_ERR_CRC32:          return "CRC32";
        case OTA_CTRL_ERR_AES:            return "AES";
        case OTA_CTRL_ERR_WAIT_GO:        return "WAIT_GO";
        case OTA_CTRL_ERR_TRANSFER:       return "TRANSFER";
        case OTA_CTRL_ERR_PROTOCOL:       return "PROTOCOL";
        case OTA_CTRL_ERR_FETCH_METADATA: return "FETCH_META";
        case OTA_CTRL_ERR_NO_UPDATE:      return "NO_UPDATE";
        case OTA_CTRL_ERR_VERSION:        return "VERSION";
        default:                          return "UNKNOWN";
    }
}

#endif /* OTA_CTRL_PROTOCOL_TEXT_H */
