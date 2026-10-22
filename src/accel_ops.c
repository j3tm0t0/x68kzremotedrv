/*
 * accel_ops.c -- Pico 側の計算本体 (Monocypher)。FreeRTOS や lwIP に依存しない純粋な関数なので、
 * ホスト (Mac) でも同じソースをコンパイルして試験する (tests/)。
 *
 * accel_compute(): 要求レコードの中身 (ACCEL_MARK から) を受け取り、応答レコードの中身を作る。
 * 戻り値は応答の長さ。要求の形が変でも、できる限り id を返した応答を作る (Z 側が待ち続けないように)。
 */
#include <string.h>
#include "accel_proto.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "accel_ops.h"

#ifndef ACCEL_CAPS
#define ACCEL_CAPS (ACCEL_CAP_VERIFY | ACCEL_CAP_X25519 | ACCEL_CAP_SIGN)
#endif

static int cap_of(int op)
{
  switch (op) {
  case ACCEL_OP_PING:           return 0;
  case ACCEL_OP_X25519:
  case ACCEL_OP_X25519_BASE:    return ACCEL_CAP_X25519;
  case ACCEL_OP_ED25519_VERIFY: return ACCEL_CAP_VERIFY;
  case ACCEL_OP_ED25519_SIGN:   return ACCEL_CAP_SIGN;
  default:                      return 0;
  }
}

int accel_compute(const uint8_t *req, int reqlen, uint8_t *res, uint32_t (*usec_now)(void))
{
  int op = 0, status = ACCEL_ST_OK, rlen = 0, mlen, reserved = 0;
  unsigned id = 0;
  const uint8_t *fixed, *msg;
  uint8_t out[64];
  uint32_t t0 = usec_now ? usec_now() : 0;

  mlen = accel_req_parse(req, reqlen, &op, &id, &fixed, &msg);
  if (mlen < 0) {
    /* 形が変: id だけでも返す (ヘッダがあれば) */
    if (reqlen >= ACCEL_REQ_HDR && req[0] == ACCEL_MARK) {
      op = req[1];
      id = accel_get16(req + 2);
    }
    status = accel_req_fixed(op) < 0 ? ACCEL_ST_BADOP : ACCEL_ST_BADREQ;
  } else if (cap_of(op) && !(ACCEL_CAPS & cap_of(op))) {
    status = ACCEL_ST_DISABLED;
  } else {
    switch (op) {
    case ACCEL_OP_PING:
      reserved = ACCEL_CAPS;
      out[0] = ACCEL_VERSION;
      rlen = 1;
      break;
    case ACCEL_OP_X25519:
      crypto_x25519(out, fixed, fixed + 32);        /* scalar の clamp は中でする (TweetNaCl と同じ) */
      rlen = 32;
      break;
    case ACCEL_OP_X25519_BASE:
      crypto_x25519_public_key(out, fixed);
      rlen = 32;
      break;
    case ACCEL_OP_ED25519_VERIFY:
      if (crypto_ed25519_check(fixed + 32, fixed, msg, (size_t)mlen) != 0)
        status = ACCEL_ST_BADSIG;
      break;
    case ACCEL_OP_ED25519_SIGN: {
      uint8_t sk[64];                               /* Monocypher の秘密鍵 = seed || pk (dropbear の priv || pub と同じ並び) */
      memcpy(sk, fixed, 64);
      crypto_ed25519_sign(out, sk, msg, (size_t)mlen);
      crypto_wipe(sk, sizeof sk);
      rlen = 64;
      break;
    }
    default:
      status = ACCEL_ST_BADOP;
      break;
    }
  }
  {
    uint32_t dt = usec_now ? usec_now() - t0 : 0;
    int n = accel_res_build(res, op, id, status, reserved, dt, out, rlen);
    crypto_wipe(out, sizeof out);
    return n;
  }
}
