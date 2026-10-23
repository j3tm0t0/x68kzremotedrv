/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2023 Yuichi Nakamura
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board_api.h"
#include "tusb.h"
#include "virtual_disk.h"
#include "msclog.h"
#include "pico/stdlib.h"

enum
{
  DISK_BLOCK_NUM  = VOLUME_SECTOR_COUNT,
  DISK_BLOCK_SIZE = SECTOR_SIZE
};

#if HID_KEYBOARD
volatile uint32_t msc_reads, msc_writes;    // 読み書きしたセクタ数 (hidkbd の status 用)
#endif

// Invoked when received SCSI_CMD_INQUIRY
// Application fill vendor id, product id and revision with string up to 8, 16, 4 characters respectively
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
  (void) lun;

  const char vid[] = "X68000Z";
  const char pid[] = "Remote Drive Mass Storage";
  const char rev[] = "1.0";

  /* 欄の長さ (8 / 16 / 4) を超えて写さない。pid は 25 文字あり、以前は product_id から 9 バイトはみ出していた
     (TinyUSB 0.18 では応答が tud_task のスタックにあり、はみ出しがタスクの保存したレジスタを壊して落ちた) */
  memcpy(vendor_id  , vid, strlen(vid) < 8 ? strlen(vid) : 8);
  memcpy(product_id , pid, strlen(pid) < 16 ? strlen(pid) : 16);
  memcpy(product_rev, rev, strlen(rev) < 4 ? strlen(rev) : 4);
}

// Invoked when received Test Unit Ready command.
// return true allowing host to read/write this LUN e.g SD card inserted
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
  (void) lun;

  return true; // RAM disk is always ready
}

// Invoked when received SCSI_CMD_READ_CAPACITY_10 and SCSI_CMD_READ_FORMAT_CAPACITY to determine the disk size
// Application update block count and block size
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
{
  (void) lun;

  *block_count = DISK_BLOCK_NUM;
  *block_size  = DISK_BLOCK_SIZE;
}

// Invoked when received Start Stop Unit command
// - Start = 0 : stopped power mode, if load_eject = 1 : unload disk storage
// - Start = 1 : active mode, if load_eject = 1 : load disk storage
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
  (void) lun;
  (void) power_condition;

  if ( load_eject )
  {
    if (start)
    {
      // load disk storage
    }else
    {
      // unload disk storage
    }
  }

  return true;
}

#if BOOTWAIT
//--------------------------------------------------------------------+
// HDS がまだ開けていないときの待たせ方
//
// USB タスクは止めない (Z の Linux は 1 命令 30 秒で諦めてリセットする。HID も止まる)。0 を返すと TinyUSB が
// 同じ命令でもう一度呼ぶので、DEFER_MS まではそうして待ち、まだなら失敗にして sense を NOT READY, LOGICAL UNIT
// IS IN PROCESS OF BECOMING READY (02/04/01) にする。Linux の sd はこれを見て間を置いて出し直す (既定で約 180 秒)。
// 失敗した読み込みはキャッシュに残らないので、0 埋めを返していた今までと違い、つながったあとに正しく読み直す。
// TinyUSB は失敗に MEDIUM NOT PRESENT (02/3A/00) を付ける (Linux は媒体を外されたと扱う) ので
// tud_msc_request_sense_cb で書き換える。
//--------------------------------------------------------------------+

#define DEFER_MS    5000

static uint32_t defer_lba = 0xffffffff, defer_from;
static uint8_t sense_key, sense_asc, sense_ascq;   // 次の REQUEST SENSE で返す (0 なら TinyUSB のまま)
volatile uint32_t msc_notready, msc_ioerr;          // 回数 (status 用)

static uint32_t now_ms(void)
{
  return to_ms_since_boot(get_absolute_time());
}

static void set_sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
  sense_key = key;
  sense_asc = asc;
  sense_ascq = ascq;
}

// vd_read_block / vd_write_block の結果を TinyUSB の戻り値にする。-1 は従来どおり成功として見せる
static int32_t vd_result(char dir, uint8_t lun, uint32_t lba, int rc, uint32_t bufsize)
{
  if (rc == VD_NOTREADY) {
    if (defer_lba != lba) {
      defer_lba = lba;
      defer_from = now_ms();
      msclog_mark("defer %c 0x%lx", dir, (unsigned long)lba);
      return 0;
    }
    if (now_ms() - defer_from < DEFER_MS)
      return 0;                     // もう一度呼ばれる
    defer_lba = 0xffffffff;
    msc_notready++;
    msclog_mark("notready %c 0x%lx -> 04/01", dir, (unsigned long)lba);
    set_sense(SCSI_SENSE_NOT_READY, 0x04, 0x01);
    return -1;
  }
  if (defer_lba == lba) {
    msclog_mark("ready after %lums", (unsigned long)(now_ms() - defer_from));
  }
  defer_lba = 0xffffffff;
  if (rc == VD_IOERR) {
    msc_ioerr++;
    msclog_mark("ioerr %c 0x%lx -> 03/11", dir, (unsigned long)lba);
    set_sense(SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);   // UNRECOVERED READ ERROR
    return -1;
  }
  return bufsize;
}

// Invoked when received REQUEST SENSE: TinyUSB の既定 (失敗なら MEDIUM NOT PRESENT) を書き換える
int32_t tud_msc_request_sense_cb(uint8_t lun, void* buffer, uint16_t bufsize)
{
  (void) lun;
  uint8_t *b = (uint8_t *) buffer;
  if (sense_key != 0 && bufsize >= sizeof(scsi_sense_fixed_resp_t)) {
    b[2] = sense_key;
    b[12] = sense_asc;
    b[13] = sense_ascq;
  }
  sense_key = 0;
  return bufsize < sizeof(scsi_sense_fixed_resp_t) ? bufsize : sizeof(scsi_sense_fixed_resp_t);
}
#endif

// Callback invoked when received READ10 command.
// Copy disk's data to buffer (up to bufsize) and return number of copied bytes.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
  // out of ramdisk
  if ( lba >= DISK_BLOCK_NUM ) return -1;

  int rc = vd_read_block(lba, buffer);
#if BOOTWAIT
  if (rc == VD_NOTREADY && lba == defer_lba)
    return vd_result('R', lun, lba, rc, bufsize);     // 待っている間は数えない・記録しない
#endif
  msclog_rw('R', lun, lba, rc, buffer);
#if HID_KEYBOARD
  msc_reads++;
#endif
#if BOOTWAIT
  return vd_result('R', lun, lba, rc, bufsize);
#else
  (void) rc;
  return bufsize;
#endif
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and return number of written bytes
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
{
  // out of ramdisk
  if ( lba >= DISK_BLOCK_NUM ) return -1;

  int rc = vd_write_block(lba, buffer);
#if BOOTWAIT
  if (rc == VD_NOTREADY && lba == defer_lba)
    return vd_result('W', lun, lba, rc, bufsize);
#endif
  msclog_rw('W', lun, lba, rc, NULL);
#if HID_KEYBOARD
  msc_writes++;
#endif
#if BOOTWAIT
  return vd_result('W', lun, lba, rc, bufsize);
#else
  (void) rc;
  return bufsize;
#endif
}

// Callback invoked when received an SCSI command not in built-in list below
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, MODE_SENSE6, REQUEST_SENSE
// - READ10 and WRITE10 has their own callbacks
int32_t tud_msc_scsi_cb (uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
  // read10 & write10 has their own callback and MUST not be handled here

  void const* response = NULL;
  int32_t resplen = 0;

  // most scsi handled is input
  bool in_xfer = true;

  switch (scsi_cmd[0])
  {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
      // Host is about to read/write etc ... better not to disconnect disk
      resplen = 0;
    break;

    default:
      // Set Sense = Invalid Command Operation
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);

      // negative means error -> tinyusb could stall and/or response with failed status
      resplen = -1;
    break;
  }

  // return resplen must not larger than bufsize
  if ( resplen > bufsize ) resplen = bufsize;

  if ( response && (resplen > 0) )
  {
    if(in_xfer)
    {
      memcpy(buffer, response, resplen);
    }else
    {
      // SCSI output
    }
  }

  return resplen;
}
