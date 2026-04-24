#ifndef OTA_DATA_PROTOCOL_H
#define OTA_DATA_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_DATA_PROTOCOL_VERSION          0x01U

#define OTA_DATA_SOF1                      0xA5U
#define OTA_DATA_SOF2                      0x5AU

#define OTA_DATA_TYPE_START                0x01U
#define OTA_DATA_TYPE_CHUNK                0x02U
#define OTA_DATA_TYPE_FINISH               0x03U
#define OTA_DATA_TYPE_ACK                  0x81U
#define OTA_DATA_TYPE_NAK                  0x82U
#define OTA_DATA_TYPE_ABORT                0x84U

#define OTA_DATA_MAX_PAYLOAD_LEN           512U
#define OTA_DATA_FIXED_HEADER_LEN          16U
#define OTA_DATA_TRAILER_LEN               4U
#define OTA_DATA_FRAME_OVERHEAD            (OTA_DATA_FIXED_HEADER_LEN + OTA_DATA_TRAILER_LEN)
#define OTA_DATA_MAX_FRAME_LEN             (OTA_DATA_MAX_PAYLOAD_LEN + OTA_DATA_FRAME_OVERHEAD)

#define OTA_DATA_DEFAULT_CHUNK_SIZE        512U
#define OTA_DATA_DEFAULT_CHECKPOINT_SIZE   4096U
#define OTA_DATA_MAX_RETRIES               5U

#define OTA_DATA_START_PAYLOAD_LEN         48U
#define OTA_DATA_NAK_PAYLOAD_LEN           4U
#define OTA_DATA_ABORT_PAYLOAD_LEN         4U

#define OTA_DATA_START_FLAG_RESUME         0x0001U

#define OTA_DATA_NAK_REASON_RETRY          1U
#define OTA_DATA_NAK_REASON_SESSION        2U
#define OTA_DATA_NAK_REASON_OFFSET         3U
#define OTA_DATA_NAK_REASON_PAYLOAD_CRC    4U
#define OTA_DATA_NAK_REASON_LENGTH         5U
#define OTA_DATA_NAK_REASON_FLASH          6U
#define OTA_DATA_NAK_REASON_STATE          7U
#define OTA_DATA_NAK_REASON_PROTOCOL       8U

#define OTA_DATA_ABORT_CLASS_RETRYABLE     1U
#define OTA_DATA_ABORT_CLASS_TERMINAL      2U

#ifdef __cplusplus
}
#endif

#endif
