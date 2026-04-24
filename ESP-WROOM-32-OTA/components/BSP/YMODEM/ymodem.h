/*
 * ESP32 YModem driver
 *
 * Copyright (C) LoBo 2017
 *
 * Author: Boris Lovosevic (loboris@gmail.com)
 *
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

#ifndef __YMODEM_H__
#define __YMODEM_H__

#include <stddef.h>
#include <stdint.h>

#define EX_UART_NUM UART_NUM_2
#define BUF_SIZE (1080)

#define YMODEM_LED_ACT 0
#define YMODEM_LED_ACT_ON 1

#define PACKET_SEQNO_INDEX      (1)
#define PACKET_SEQNO_COMP_INDEX (2)

#define PACKET_HEADER           (3)
#define PACKET_TRAILER          (2)
#define PACKET_OVERHEAD         (PACKET_HEADER + PACKET_TRAILER)
#define PACKET_SIZE             (128)
#define PACKET_1K_SIZE          (1024)

#define FILE_SIZE_LENGTH        (16)

#define SOH                     (0x01)
#define STX                     (0x02)
#define EOT                     (0x04)
#define ACK                     (0x06)
#define NAK                     (0x15)
#define CA                      (0x18)
#define CRC16                   (0x43)

#define ABORT1                  (0x41)
#define ABORT2                  (0x61)

#define NAK_TIMEOUT             (3000)
#define MAX_ERRORS              (45)

#define YM_MAX_FILESIZE         (10 * 1024 * 1024)

typedef size_t (*ymodem_read_callback_t)(void *context, uint8_t *buffer, size_t max_len);

int Ymodem_TransmitStream(char *sendFileName,
                          unsigned int sizeFile,
                          ymodem_read_callback_t read_callback,
                          void *read_context);

#endif
