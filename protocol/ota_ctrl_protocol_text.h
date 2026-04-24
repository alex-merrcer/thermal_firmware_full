#ifndef OTA_CTRL_PROTOCOL_TEXT_H
#define OTA_CTRL_PROTOCOL_TEXT_H

#include "ota_ctrl_protocol.h"

static const char *ota_ctrl_msg_name(uint8_t msg_type)
{
    switch (msg_type) {
        case OTA_CTRL_MSG_REQ:
            return "REQ";
        case OTA_CTRL_MSG_CANCEL:
            return "CANCEL";
        case OTA_CTRL_MSG_GO:
            return "GO";
        case OTA_CTRL_MSG_ACK:
            return "ACK";
        case OTA_CTRL_MSG_STATUS:
            return "STATUS";
        case OTA_CTRL_MSG_READY:
            return "READY";
        case OTA_CTRL_MSG_ERROR:
            return "ERROR";
        case OTA_CTRL_MSG_META:
            return "META";
        case OTA_CTRL_MSG_RESULT:
            return "RESULT";
        default:
            return "UNKNOWN";
    }
}

static const char *ota_ctrl_stage_name(uint8_t stage)
{
    switch (stage) {
        case OTA_CTRL_STAGE_IDLE:
            return "IDLE";
        case OTA_CTRL_STAGE_QUERY:
            return "QUERY";
        case OTA_CTRL_STAGE_DOWNLOAD:
            return "DOWNLOAD";
        case OTA_CTRL_STAGE_VERIFY_SIG:
            return "VER_SIG";
        case OTA_CTRL_STAGE_VERIFY_CRC:
            return "VER_CRC";
        case OTA_CTRL_STAGE_AES_PREPARE:
            return "AES_PREP";
        case OTA_CTRL_STAGE_READY:
            return "READY";
        case OTA_CTRL_STAGE_TRANSFER:
            return "TRANSFER";
        case OTA_CTRL_STAGE_DONE:
            return "DONE";
        default:
            return "UNKNOWN";
    }
}

static const char *ota_ctrl_error_name(uint16_t error_code)
{
    switch (error_code) {
        case OTA_CTRL_ERR_BUSY:
            return "BUSY";
        case OTA_CTRL_ERR_NO_WIFI:
            return "NO_WIFI";
        case OTA_CTRL_ERR_FETCH_PACKAGE:
            return "FETCH_PKG";
        case OTA_CTRL_ERR_NO_PACKAGE:
            return "NO_PKG";
        case OTA_CTRL_ERR_PARTITION:
            return "PARTITION";
        case OTA_CTRL_ERR_PRODUCT:
            return "PRODUCT";
        case OTA_CTRL_ERR_HW_REV:
            return "HW_REV";
        case OTA_CTRL_ERR_SIGNATURE:
            return "SIGNATURE";
        case OTA_CTRL_ERR_CRC32:
            return "CRC32";
        case OTA_CTRL_ERR_AES:
            return "AES";
        case OTA_CTRL_ERR_WAIT_GO:
            return "WAIT_GO";
        case OTA_CTRL_ERR_TRANSFER:
            return "TRANSFER";
        case OTA_CTRL_ERR_PROTOCOL:
            return "PROTOCOL";
        case OTA_CTRL_ERR_FETCH_METADATA:
            return "FETCH_META";
        case OTA_CTRL_ERR_NO_UPDATE:
            return "NO_UPDATE";
        case OTA_CTRL_ERR_VERSION:
            return "VERSION";
        default:
            return "UNKNOWN";
    }
}

#endif
