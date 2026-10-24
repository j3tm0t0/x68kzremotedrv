/* 
 * The MIT License (MIT)
 *
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

#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdbool.h>
#include "smb2.h"
#include "libsmb2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define LOGSIZE         1024
extern char log_txt[LOGSIZE];

extern TaskHandle_t main_th;
extern TaskHandle_t connect_th;
extern TaskHandle_t keepalive_th;
extern SemaphoreHandle_t remote_sem;

extern uint64_t boottime;
extern volatile int sysstatus;
void connect_task(void *params);
void keepalive_task(void *params);

void net_setup(void);
void keepalive_poll(void);
/* SMB の 1 回の要求を待つ上限 (秒)。Z の Linux は USB の 1 命令を 30 秒で諦めてリセットするので、USB タスクの中の SMB は
   これで打ち切り、読み書きは NOT READY にして出し直させる (smb_lost で裏でつなぎ直す) */
#define SMB_IO_TIMEOUT  5
extern volatile bool smb_down;
void smb_lost(const char *why);
const char *net_hostname(void);
struct smb2_context *connect_smb2(const char *share);
void disconnect_smb2(struct smb2_context *smb2);
struct smb2_context *path2smb2(const char *path, const char **shpath);
struct smb2_context *connect_smb2_path(const char *path, const char **shpath);
void disconnect_smb2_path(const char *path);
void disconnect_smb2_all(void);
void keepalive_smb2_all(void);

void hds_cache_init(void);
int hds_cache_read(struct smb2_context *smb2, struct smb2fh *sfh, uint32_t lba, uint8_t *buf);
int hds_cache_write(struct smb2_context *smb2, struct smb2fh *sfh, uint32_t lba, uint8_t *buf);

#endif /* _MAIN_H_ */
