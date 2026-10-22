/*
 * accel_proto.h -- X68000 Z と Pico W (x68kzremotedrv netpipe) の間の暗号アクセラレータの記録形式
 *
 * npd.x の X6NR 交換 (IP パケットを長さ付きレコードで運ぶ) に「制御レコード」を混ぜる。
 * レコードの枠は IP のものと同じで、中身の先頭 1 バイトが ACCEL_MARK (0xA5) なら制御レコード:
 *   Z  -> Pico: [長さ 2][ACCEL_MARK][op][id hi][id lo][引数...][偶数詰め物]
 *   Pico -> Z: [長さ 2][空き 14][ACCEL_MARK][op][id hi][id lo][status][reserved][usec 4 (BE)][結果...][詰め物]
 * IPv4 のバージョン nibble (4) と重ならないので、制御レコードを知らない側 (古い npd.x / 古いファーム) は
 * それを不正な IP として捨てて次のレコードに進む (交換そのものは壊れない)。
 *
 * 値はすべて明示的なバイト列 (ビッグエンディアン) で、X68000 (BE) と RP2040 (LE) で同じこのヘッダを使う。
 * 同時に飛ばせる要求は 1 つ。応答の id が要求の id と違えば捨てる。
 *
 * このファイルは net/accel (Z 側・ホストテスト) と x68kzremotedrv の src/ (Pico 側) に同じものを置く。
 */
#ifndef ACCEL_PROTO_H
#define ACCEL_PROTO_H

#include <stdint.h>

#define ACCEL_MARK          0xa5
#define ACCEL_VERSION       1

/* op */
#define ACCEL_OP_PING           0   /* 引数なし。応答: status 0, reserved = caps (ACCEL_CAP_*), 結果 = [version] */
#define ACCEL_OP_X25519         1   /* 引数: scalar 32, point 32   -> 結果 32 (共有秘密 / 任意の点) */
#define ACCEL_OP_X25519_BASE    2   /* 引数: scalar 32             -> 結果 32 (公開鍵、基点 9) */
#define ACCEL_OP_ED25519_VERIFY 3   /* 引数: pk 32, sig 64, msg    -> 結果なし (status が 0 なら正しい署名) */
#define ACCEL_OP_ED25519_SIGN   4   /* 引数: seed 32, pk 32, msg   -> 結果 64 (署名) */

/* status */
#define ACCEL_ST_OK         0
#define ACCEL_ST_BADSIG     1   /* ED25519_VERIFY: 署名が合わない */
#define ACCEL_ST_BADREQ     2   /* 引数の長さが変 */
#define ACCEL_ST_BADOP      3   /* 知らない op */
#define ACCEL_ST_BUSY       4   /* 別の要求を計算中 */
#define ACCEL_ST_DISABLED   5   /* ファームでその op が無効 (ACCEL_CAP_* を参照) */

/* caps (PING の応答の reserved バイト): ファームが受け付ける段階 */
#define ACCEL_CAP_VERIFY    0x01    /* 段階 1: ed25519 verify (公開情報だけ) */
#define ACCEL_CAP_X25519    0x02    /* 段階 2: X25519 keygen / shared (Pico がセッション鍵を知る) */
#define ACCEL_CAP_SIGN      0x04    /* 段階 3: ed25519 sign (署名鍵の seed を渡す) */

#define ACCEL_REQ_HDR       4       /* MARK op id(2) */
#define ACCEL_RES_HDR       10      /* MARK op id(2) status reserved usec(4) */
#define ACCEL_MSG_MAX       1024    /* 署名 / 検証するメッセージの上限。ssh クライアントが扱うのは
                                       ホスト鍵の検証 = 交換ハッシュ 32 バイト、認証の署名 = session_id + USERAUTH_REQUEST
                                       (ユーザ名が短ければ 150〜250 バイト) */
#define ACCEL_REQ_MAX       (ACCEL_REQ_HDR + 32 + 64 + ACCEL_MSG_MAX)   /* 1124 */
#define ACCEL_RES_MAX       (ACCEL_RES_HDR + 64)                        /* 74 */

static inline void accel_put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline unsigned accel_get16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static inline void accel_put32(uint8_t *p, uint32_t v) { accel_put16(p, v >> 16); accel_put16(p + 2, v & 0xffff); }
static inline uint32_t accel_get32(const uint8_t *p) { return ((uint32_t)accel_get16(p) << 16) | accel_get16(p + 2); }

/* 要求の固定引数の長さ (メッセージを除く)。知らない op は -1 */
static inline int accel_req_fixed(int op)
{
  switch (op) {
  case ACCEL_OP_PING:           return 0;
  case ACCEL_OP_X25519:         return 64;
  case ACCEL_OP_X25519_BASE:    return 32;
  case ACCEL_OP_ED25519_VERIFY: return 96;
  case ACCEL_OP_ED25519_SIGN:   return 64;
  default:                      return -1;
  }
}

/* op がメッセージ (可変長) を伴うか */
static inline int accel_req_has_msg(int op)
{
  return op == ACCEL_OP_ED25519_VERIFY || op == ACCEL_OP_ED25519_SIGN;
}

/* 応答の結果の長さ (status 0 のとき) */
static inline int accel_res_len(int op)
{
  switch (op) {
  case ACCEL_OP_PING:           return 1;
  case ACCEL_OP_X25519:         return 32;
  case ACCEL_OP_X25519_BASE:    return 32;
  case ACCEL_OP_ED25519_VERIFY: return 0;
  case ACCEL_OP_ED25519_SIGN:   return 64;
  default:                      return 0;
  }
}

/* 要求を組む。buf は ACCEL_REQ_MAX 以上。fixed は固定引数 (連結済み)、msg はメッセージ (無ければ NULL, 0)。
   戻り値: 要求の長さ、引数が変なら -1 */
static inline int accel_req_build(uint8_t *buf, int op, unsigned id,
                                  const uint8_t *fixed, const uint8_t *msg, int mlen)
{
  int f = accel_req_fixed(op), i;
  if (f < 0 || mlen < 0 || mlen > ACCEL_MSG_MAX || (mlen && !accel_req_has_msg(op)))
    return -1;
  buf[0] = ACCEL_MARK;
  buf[1] = (uint8_t)op;
  accel_put16(buf + 2, id);
  for (i = 0; i < f; i++)
    buf[ACCEL_REQ_HDR + i] = fixed[i];
  for (i = 0; i < mlen; i++)
    buf[ACCEL_REQ_HDR + f + i] = msg[i];
  return ACCEL_REQ_HDR + f + mlen;
}

/* 要求をほどく。戻り値: メッセージの長さ (0 以上)、形が変なら -1。*fixed / *msg は buf の中を指す */
static inline int accel_req_parse(const uint8_t *buf, int len, int *op, unsigned *id,
                                  const uint8_t **fixed, const uint8_t **msg)
{
  int f, mlen;
  if (len < ACCEL_REQ_HDR || buf[0] != ACCEL_MARK)
    return -1;
  *op = buf[1];
  *id = accel_get16(buf + 2);
  f = accel_req_fixed(*op);
  if (f < 0 || len < ACCEL_REQ_HDR + f)
    return -1;
  mlen = len - ACCEL_REQ_HDR - f;
  if (mlen > ACCEL_MSG_MAX || (mlen && !accel_req_has_msg(*op)))
    return -1;
  *fixed = buf + ACCEL_REQ_HDR;
  *msg = buf + ACCEL_REQ_HDR + f;
  return mlen;
}

/* 応答を組む。buf は ACCEL_RES_MAX 以上。戻り値: 応答の長さ */
static inline int accel_res_build(uint8_t *buf, int op, unsigned id, int status, int reserved,
                                  uint32_t usec, const uint8_t *result, int rlen)
{
  int i;
  buf[0] = ACCEL_MARK;
  buf[1] = (uint8_t)op;
  accel_put16(buf + 2, id);
  buf[4] = (uint8_t)status;
  buf[5] = (uint8_t)reserved;
  accel_put32(buf + 6, usec);
  for (i = 0; i < rlen; i++)
    buf[ACCEL_RES_HDR + i] = result[i];
  return ACCEL_RES_HDR + rlen;
}

/* 応答をほどく。戻り値: 結果の長さ、形が変なら -1 */
static inline int accel_res_parse(const uint8_t *buf, int len, int *op, unsigned *id, int *status,
                                  int *reserved, uint32_t *usec, const uint8_t **result)
{
  if (len < ACCEL_RES_HDR || buf[0] != ACCEL_MARK)
    return -1;
  *op = buf[1];
  *id = accel_get16(buf + 2);
  *status = buf[4];
  *reserved = buf[5];
  *usec = accel_get32(buf + 6);
  *result = buf + ACCEL_RES_HDR;
  return len - ACCEL_RES_HDR;
}

#endif /* ACCEL_PROTO_H */
