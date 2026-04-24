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

#include <stdio.h>
#include <string.h>

#include <driver/uart.h>
#include "esp_log.h"
#include "ymodem.h"
#include "ota_stm32_ctrl.h"
#include "driver/gpio.h"

static const char *TAG = "YMODEM_TX";

#define YMODEM_WAIT_CTRL_TAKEOVER          6U
#define YMODEM_MAX_UNEXPECTED_RESPONSES    96U
#define YMODEM_UNEXPECTED_RESPONSE_DELAYMS 15U

//----------------------------------
static void IRAM_ATTR LED_toggle() {
#if YMODEM_LED_ACT
	if (GPIO.out & (1 << YMODEM_LED_ACT)) {
		GPIO.out_w1tc = (1 << YMODEM_LED_ACT);
	} else {
		GPIO.out_w1ts = (1 << YMODEM_LED_ACT);
	}
#endif
}

//------------------------------------------------------------------------
static unsigned short crc16(const unsigned char *buf, unsigned long count)
{
  unsigned short crc = 0;
  int i;

  while(count--) {
    crc = crc ^ *buf++ << 8;

    for (i=0; i<8; i++) {
      if (crc & 0x8000) crc = crc << 1 ^ 0x1021;
      else crc = crc << 1;
    }
  }
  return crc;
}

//--------------------------------------------------------------
static int32_t Receive_Byte (unsigned char *c, uint32_t timeout)
{
	unsigned char ch;
    int len = uart_read_bytes(EX_UART_NUM, &ch, 1, timeout / portTICK_PERIOD_MS);
    if (len <= 0) return -1;

    *c = ch;
    return 0;
}

static int ymodem_try_handoff_control_frame(unsigned char first_byte)
{
	unsigned char next = 0;
	uint8_t sof[2];

	if (first_byte != OTA_CTRL_SOF1) {
		return 0;
	}

	if (Receive_Byte(&next, 20) < 0) {
		return 0;
	}

	if (next != OTA_CTRL_SOF2) {
		ESP_LOGW(TAG, "Discard stray control-prefix byte sequence: 0x%02X 0x%02X", first_byte, next);
		return 0;
	}

	sof[0] = OTA_CTRL_SOF1;
	sof[1] = OTA_CTRL_SOF2;
	ota_ctrl_pushback_bytes(sof, sizeof(sof));
	ESP_LOGW(TAG, "Detected STM32 control frame while waiting YMODEM response, hand off to OTA control");
	return 1;
}

static int ymodem_drain_initial_handshake_noise(void)
{
	unsigned char ch = 0;
	int drained = 0;

	for (;;) {
		if (Receive_Byte(&ch, 20) < 0) {
			break;
		}

		if (ch == CRC16 || ch == NAK) {
			drained++;
			continue;
		}

		if (ch == CA) {
			unsigned char next = 0;
			if (Receive_Byte(&next, 20) == 0 && next == CA) {
				ESP_LOGE(TAG, "Receiver aborted while draining duplicate handshake bytes");
				return -1;
			}
			drained++;
			continue;
		}

		ESP_LOGW(TAG, "Discard unexpected pre-packet byte: 0x%02X", ch);
		drained++;
	}

	return drained;
}

//--------------------------------
static uint32_t Send_Byte (char c)
{
  uart_write_bytes(EX_UART_NUM, &c, 1);
  return 0;
}

//----------------------------
static void send_CA ( void ) {
  Send_Byte(CA);
  Send_Byte(CA);
}

//------------------------------------------------------------------------------------
static void Ymodem_PrepareIntialPacket(uint8_t *data, char *fileName, uint32_t length)
{
  uint16_t tempCRC;
  char *file_size_text;

  memset(data, 0, PACKET_SIZE + PACKET_HEADER);
  // Make first three packet
  data[0] = SOH;
  data[1] = 0x00;
  data[2] = 0xff;
  
  // add filename
  sprintf((char *)(data+PACKET_HEADER), "%s", fileName);

  //add file site
  file_size_text = (char *)(data + PACKET_HEADER + strlen((char *)(data + PACKET_HEADER)) + 1);
  sprintf(file_size_text, "%lu", (unsigned long)length);
  data[PACKET_HEADER + strlen((char *)(data+PACKET_HEADER)) +
	   1 + strlen(file_size_text)] = ' ';
  
  // add crc
  tempCRC = crc16(&data[PACKET_HEADER], PACKET_SIZE);
  data[PACKET_SIZE + PACKET_HEADER] = tempCRC >> 8;
  data[PACKET_SIZE + PACKET_HEADER + 1] = tempCRC & 0xFF;
}

//-------------------------------------------------
static void Ymodem_PrepareLastPacket(uint8_t *data)
{
  uint16_t tempCRC;
  
  memset(data, 0, PACKET_SIZE + PACKET_HEADER);
  data[0] = SOH;
  data[1] = 0x00;
  data[2] = 0xff;
  tempCRC = crc16(&data[PACKET_HEADER], PACKET_SIZE);
  //tempCRC = crc16_le(0, &data[PACKET_HEADER], PACKET_SIZE);
  data[PACKET_SIZE + PACKET_HEADER] = tempCRC >> 8;
  data[PACKET_SIZE + PACKET_HEADER + 1] = tempCRC & 0xFF;
}

//-----------------------------------------------------------------------------------------
static bool Ymodem_PreparePacketFromReader(uint8_t *data,
                                           uint8_t pktNo,
                                           uint32_t sizeBlk,
                                           ymodem_read_callback_t read_callback,
                                           void *read_context,
                                           size_t *actual_read)
{
  uint16_t i;
  size_t requested_size;
  size_t read_len;
  uint16_t tempCRC;

  data[0] = STX;
  data[1] = (pktNo & 0x000000ff);
  data[2] = (~(pktNo & 0x000000ff));

  requested_size = sizeBlk < PACKET_1K_SIZE ? sizeBlk : PACKET_1K_SIZE;
  read_len = 0;
  if (requested_size > 0) {
    read_len = read_callback(read_context, data + PACKET_HEADER, requested_size);
  }

  if (read_len > requested_size) {
    read_len = requested_size;
  }

  if (read_len < requested_size) {
    ESP_LOGE(TAG, "Stream source underrun. Requested=%u, Read=%u",
             (unsigned)requested_size,
             (unsigned)read_len);
    return false;
  }

  if (requested_size < PACKET_1K_SIZE) {
    for (i = (uint16_t)(requested_size + PACKET_HEADER); i < PACKET_1K_SIZE + PACKET_HEADER; i++) {
      data[i] = 0x00;
    }
  }

  tempCRC = crc16(&data[PACKET_HEADER], PACKET_1K_SIZE);
  data[PACKET_1K_SIZE + PACKET_HEADER] = tempCRC >> 8;
  data[PACKET_1K_SIZE + PACKET_HEADER + 1] = tempCRC & 0xFF;
  *actual_read = read_len;
  return true;
}

//-------------------------------------------------------------
static uint8_t Ymodem_WaitResponse(uint8_t ackchr, uint8_t tmo)
{
  unsigned char receivedC;
  uint32_t errors = 0;
  uint32_t unexpected_count = 0;


  do {
    if (Receive_Byte(&receivedC, NAK_TIMEOUT) == 0) {
      if (receivedC == ackchr) {
        return 1;
      }
      else if (receivedC == CA) {
        send_CA();
        return 2; // CA received, Sender abort
      }
      else if (ackchr == ACK && receivedC == CRC16) {
        /*
         * Receiver can repeat 'C' while still waiting for the first valid
         * packet. Treat this as a resend request instead of a hard protocol
         * failure so packet 0 can be retried safely.
         */
        return 5;
      }
      else if (receivedC == NAK) {
        return 3;
      }
      else if (ymodem_try_handoff_control_frame(receivedC) != 0) {
        return YMODEM_WAIT_CTRL_TAKEOVER;
      }
      else {
        if (unexpected_count < 8U || ((unexpected_count % 16U) == 15U)) {
          ESP_LOGW(TAG, "Unexpected YMODEM response: got 0x%02X, expected 0x%02X",
                   receivedC,
                   ackchr);
        }
        unexpected_count++;
        if (unexpected_count >= YMODEM_MAX_UNEXPECTED_RESPONSES) {
          return 4;
        }
        vTaskDelay(pdMS_TO_TICKS(YMODEM_UNEXPECTED_RESPONSE_DELAYMS));
        continue;
      }
    }
    else {
      errors++;
    }
  }while (errors < tmo);
  return 0;
}

static void ymodem_log_tx_progress(uint32_t transferred,
                                   uint32_t total,
                                   int *last_percent_bucket)
{
  uint32_t percent = 0U;
  int current_bucket = 0;

  if (last_percent_bucket == NULL || total == 0U) {
    return;
  }

  if (transferred >= total) {
    percent = 100U;
  } else {
    percent = (transferred * 100U) / total;
  }

  current_bucket = (int)(percent / 5U);
  if (current_bucket != *last_percent_bucket || percent == 100U) {
    ESP_LOGI(TAG, "YMODEM TX %u%% (%u/%u)", (unsigned)percent, (unsigned)transferred, (unsigned)total);
    *last_percent_bucket = current_bucket;
  }
}


//------------------------------------------------------------------------
int Ymodem_TransmitStream(char *sendFileName,
                          unsigned int sizeFile,
                          ymodem_read_callback_t read_callback,
                          void *read_context)
{
  uint8_t packet_data[PACKET_1K_SIZE + PACKET_OVERHEAD];
  uint16_t blkNumber;
  unsigned char receivedC;
  int err;
  uint32_t size = 0;
  size_t actual_read = 0;
  uint32_t transferred = 0U;
  int last_percent_bucket = -1;
  int initial_wait_round = 0;
  int got_initial_crc_request = 0;
  int packet0_retry_count = 0;

  if (read_callback == NULL) {
    ESP_LOGE(TAG, "read_callback is NULL");
    return -11;
  }

  /*
   * After the OTA control GO frame, STM32 immediately switches to YModem
   * receiver mode and may send the initial 'C' handshake byte before ESP32
   * enters this function. Do not flush the UART here, otherwise that valid
   * handshake byte gets discarded and both sides wait until timeout.
   */

  /*
   * Prefer the standard initial 'C' handshake, but do not stall for minutes if
   * the receiver switches state a little late. STM32 receiver can still accept
   * packet 0 without the initial 'C', so after a short wait we optimistically
   * start the transfer.
   */
  for (initial_wait_round = 0; initial_wait_round < 6; ++initial_wait_round) {
    if (Receive_Byte(&receivedC, 500) < 0) {
      continue;
    }

    if (receivedC == CRC16) {
      got_initial_crc_request = 1;
      break;
    }

    if (receivedC == CA) {
      unsigned char next_char = 0;
      if (Receive_Byte(&next_char, 500) == 0 && next_char == CA) {
        ESP_LOGE(TAG, "Receiver aborted before packet 0");
        return -1;
      }
      receivedC = next_char != 0 ? next_char : CA;
    }

    ESP_LOGW(TAG, "Ignore unexpected handshake byte: 0x%02X", receivedC);
  }

  if (!got_initial_crc_request) {
    ESP_LOGW(TAG, "Initial receiver 'C' not observed, sending packet 0 optimistically");
  }
  else {
    int drained = ymodem_drain_initial_handshake_noise();
    if (drained < 0) {
      return -1;
    }
    if (drained > 0) {
      ESP_LOGW(TAG, "Drained %d duplicate handshake byte(s) before packet 0", drained);
    }
  }
  
  // === Prepare first block and send it =======================================
  /* When the receiving program receives this block and successfully
   * opened the output file, it shall acknowledge this block with an ACK
   * character and then proceed with a normal YMODEM file transfer
   * beginning with a "C" or NAK tranmsitted by the receiver.
   */
  Ymodem_PrepareIntialPacket(packet_data, sendFileName, sizeFile);
  do 
  {
    // Send Packet
	uart_write_bytes(EX_UART_NUM, (char *)packet_data, PACKET_SIZE + PACKET_OVERHEAD);
    uart_wait_tx_done(EX_UART_NUM, pdMS_TO_TICKS(2000));

	// Wait for Ack
    err = Ymodem_WaitResponse(ACK, 10);
    if (err == 5) {
      /*
       * A duplicated pre-session 'C' can still be in the UART while packet 0
       * is already on the wire. Give the receiver a short grace window to ACK
       * the current packet before we decide to resend and corrupt the stream
       * with an overlapping packet 0.
       */
      ESP_LOGW(TAG, "Packet 0 received duplicate 'C' before ACK, waiting briefly for ACK");
      err = Ymodem_WaitResponse(ACK, 2);
      if (err == 1) {
        break;
      }
      if (err == 5) {
        err = 3;
      }
    }
    if (err == YMODEM_WAIT_CTRL_TAKEOVER) {
      return -14;
    }
    if (err == 0 || err == 4) {
      send_CA();
      return -2;                  // timeout or wrong response
    }
    else if (err == 2) return 98; // abort
    else if (err == 3) {
      packet0_retry_count++;
      ESP_LOGW(TAG, "Resend packet 0, retry=%d", packet0_retry_count);
    }
    LED_toggle();
  }while (err != 1);

  // After initial block the receiver sends 'C' after ACK
  err = Ymodem_WaitResponse(CRC16, 10);
  if (err == YMODEM_WAIT_CTRL_TAKEOVER) {
    return -14;
  }
  if (err != 1) {
    send_CA();
    return -3;
  }
  
  // === Send file blocks ======================================================
  size = sizeFile;
  blkNumber = 0x01;
  
  // Resend packet if NAK  for a count of 10 else end of communication
  while (size)
  {
    // Prepare and send next packet
    if (!Ymodem_PreparePacketFromReader(packet_data,
                                        blkNumber,
                                        size,
                                        read_callback,
                                        read_context,
                                        &actual_read)) {
      send_CA();
      return -12;
    }

    if (actual_read != (size > PACKET_1K_SIZE ? PACKET_1K_SIZE : size)) {
      send_CA();
      return -13;
    }

    do
    {
      uart_write_bytes(EX_UART_NUM, (char *)packet_data, PACKET_1K_SIZE + PACKET_OVERHEAD);

      // Wait for Ack
      err = Ymodem_WaitResponse(ACK, 10);
      if (err == 1) {
        uint32_t sent_now = (size > PACKET_1K_SIZE) ? PACKET_1K_SIZE : size;
        blkNumber++;
        if (size > PACKET_1K_SIZE) size -= PACKET_1K_SIZE; // Next packet
        else size = 0; // Last packet sent
        transferred += sent_now;
        ymodem_log_tx_progress(transferred, sizeFile, &last_percent_bucket);
      }
      else if (err == YMODEM_WAIT_CTRL_TAKEOVER) {
        return -14;
      }
      else if (err == 0 || err == 4) {
        send_CA();
        return -4;                  // timeout or wrong response
      }
      else if (err == 5) {
        err = 3;
      }
      else if (err == 2) return -5; // abort
    }while(err != 1);
    LED_toggle();
  }
  
  // === Send EOT ==============================================================
  Send_Byte(EOT); // Send (EOT)
  // Wait for Ack
  do 
  {
    // Wait for Ack
    err = Ymodem_WaitResponse(ACK, 10);
    if (err == 3) {   // NAK
      Send_Byte(EOT); // Send (EOT)
    }
    else if (err == YMODEM_WAIT_CTRL_TAKEOVER) {
      return -14;
    }
    else if (err == 0 || err == 4) {
      send_CA();
      return -6;                  // timeout or wrong response
    }
    else if (err == 5) {
      err = 3;
    }
    else if (err == 2) return -7; // abort
  }while (err != 1);
  
  // === Receiver requests next file, prepare and send last packet =============
  err = Ymodem_WaitResponse(CRC16, 10);
  if (err == YMODEM_WAIT_CTRL_TAKEOVER) {
    return -14;
  }
  if (err != 1) {
    send_CA();
    return -8;
  }

  LED_toggle();
  Ymodem_PrepareLastPacket(packet_data);
  do 
  {
	// Send Packet
	uart_write_bytes(EX_UART_NUM, (char *)packet_data, PACKET_SIZE + PACKET_OVERHEAD);

	// Wait for Ack
    err = Ymodem_WaitResponse(ACK, 10);
    if (err == YMODEM_WAIT_CTRL_TAKEOVER) {
      return -14;
    }
    if (err == 0 || err == 4) {
      send_CA();
      return -9;                  // timeout or wrong response
    }
    else if (err == 5) {
      err = 3;
    }
    else if (err == 2) return -10; // abort
  }while (err != 1);
  
  #if YMODEM_LED_ACT
  gpio_set_level(YMODEM_LED_ACT, YMODEM_LED_ACT_ON ^ 1);
  #endif
  ymodem_log_tx_progress(sizeFile, sizeFile, &last_percent_bucket);
  return 0; // file transmitted successfully
}


