#ifndef ACCEL_OPS_H
#define ACCEL_OPS_H
#include <stdint.h>
/* 要求 (ACCEL_MARK から reqlen バイト) を計算して応答を res (ACCEL_RES_MAX 以上) に作る。戻り値: 応答の長さ。
   usec_now はマイクロ秒の時計 (無ければ NULL。応答の usec は 0) */
int accel_compute(const uint8_t *req, int reqlen, uint8_t *res, uint32_t (*usec_now)(void));
#endif
