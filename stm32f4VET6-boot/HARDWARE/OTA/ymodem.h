/**
  ******************************************************************************
  * @file    ymodem.h
  * @brief   STM32 OTA data-plane receiver public definitions.
  ******************************************************************************
  */

#ifndef __YMODEM_H_
#define __YMODEM_H_

#include "stdint.h"
#include "../../../protocol/ota_ctrl_protocol.h"
#include "../../../protocol/ota_image_header.h"
#include "../../../protocol/ota_data_protocol.h"

#define YMODEM_OK               0
#define YMODEM_ERR_TIMEOUT      1
#define YMODEM_ERR_SEQ          2
#define YMODEM_ERR_CRC          3
#define YMODEM_ERR_SIZE         4
#define YMODEM_ERR_FLASH        5
#define YMODEM_ERR_ABORT        6
#define YMODEM_ERR_MAX_RETRIES  7
#define YMODEM_ERR_EOT          8
#define YMODEM_ERR_HEADER       9
#define YMODEM_ERR_AUTH         10
#define YMODEM_ERR_VERSION      11
#define YMODEM_ERR_SLOT         12
#define YMODEM_ERR_UART         13
#define YMODEM_ERR_PROTOCOL     14

typedef void (*YmodemProgressCallback)(uint32_t current, uint32_t total);
typedef uint8_t (*YmodemHeaderValidator)(const OtaImageHeaderBinary *header, void *context);
typedef uint8_t (*YmodemCheckpointCallback)(uint32_t durable_offset, void *context);

#define YMODEM_BODY_HASH_NONE         0U
#define YMODEM_BODY_HASH_OK           1U
#define YMODEM_BODY_HASH_ENGINE_FAIL  2U

typedef struct
{
    uint8_t body_hash_state;
    uint8_t body_hash[32];
} YmodemHashDiagnostics;

typedef struct
{
    uint32_t state[8];
    uint32_t total_len;
    uint8_t buffer_len;
    uint8_t buffer[64];
} OtaSha256Context;

uint8_t Ymodem_GetErrorCode(void);
uint8_t Ymodem_GetErrorStage(void);
uint32_t Ymodem_GetUartErrorFlags(void);
uint32_t Ymodem_GetLastAckedOffset(void);
void Ymodem_SetError(uint8_t code, uint8_t stage);
void Ymodem_ResetError(void);
void Ymodem_SetProgressCallback(YmodemProgressCallback callback);
void Ymodem_SetHeaderValidator(YmodemHeaderValidator validator, void *context);
uint8_t Ymodem_ConfigureTransfer(const OtaImageHeaderBinary *header,
                                 const uint8_t session_fingerprint[32],
                                 uint32_t start_offset,
                                 uint32_t checkpoint_size,
                                 YmodemCheckpointCallback checkpoint_callback,
                                 void *checkpoint_context);
const OtaImageHeaderBinary *Ymodem_GetReceivedHeader(void);
uint32_t Ymodem_GetReceivedFirmwareSize(void);
const YmodemHashDiagnostics *Ymodem_GetHashDiagnostics(void);
void OtaSha256_Init(OtaSha256Context *context);
void OtaSha256_Update(OtaSha256Context *context, const uint8_t *data, uint32_t data_len);
void OtaSha256_Final(OtaSha256Context *context, uint8_t output[32]);
uint8_t OtaSha256_Compute(const uint8_t *data, uint32_t length, uint8_t output[32]);
int32_t Ymodem_Receive(uint8_t *buf);

#endif
