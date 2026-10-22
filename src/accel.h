#ifndef _ACCEL_H_
#define _ACCEL_H_
#include <stdint.h>
#include <stdbool.h>

/* ACCEL_ENABLE=0 (cmake -DACCEL_ENABLE=0) でアクセラレータを丸ごと外す (Monocypher も組み込まない。A/B 比較用) */
#ifndef ACCEL_ENABLE
#define ACCEL_ENABLE 1
#endif
#if ACCEL_ENABLE

/* 暗号アクセラレータ (X25519 / Ed25519 を Z の代わりに計算する)。netpipe.c から呼ぶ。詳細は accel.c */
void accel_init(void);
void accel_submit(const uint8_t *payload, int len);     /* 要求レコードの中身 (ACCEL_MARK から)。USB タスクから */
int accel_result(uint8_t *dst);                         /* 応答があれば dst に写して長さを返す (無ければ 0)。USB タスクから */
#else
#define accel_init()            ((void)0)
#define accel_submit(p, n)      ((void)0)
#define accel_result(d)         0
#endif

#endif
