/*
 * tsp_svc.h -- Pico のソケット窓口 (tis.x の Pico backend の相手)
 *
 * netpipe の交換 (np_exchange) から、Z のレコードのうち先頭が TSP_MARK (0xA6) のものを tsp_exchange() に渡し、
 * 戻ってきた応答を Z へのレコードとして載せる ([長さ 2][空き 14][応答][詰め物])。応答は 1 交換に 1 個。
 * Z が TSP レコードを送ってこない交換では何も載せない (応答は要求 1 個に 1 個)。
 */
#ifndef TSP_SVC_H
#define TSP_SVC_H
#include <stdint.h>

/* 起動時に 1 回 (lwIP の初期化のあと)。epoch は起動ごとに変わる数 (乱数か、フラッシュの起動回数) */
void tsp_init(uint32_t epoch);

/* Z の要求 1 個を処理して応答を res に書く。戻り値: 応答の長さ (0 = 載せない)。
   *more = 1: Z に送るものがまだある (netpipe の応答見出しの more に足す)。resmax は TSP_RES_MAX まで */
int tsp_exchange(const uint8_t *req, int reqlen, uint8_t *res, int resmax, int *more);

/* 全部のソケットを捨てる (Wi-Fi が切れた / 再起動の前)。そのあと別の epoch で tsp_init する */
void tsp_shutdown(void);

/* 使っている Pico 側のソケットの数 (試験・設定画面) */
int tsp_nsock(void);

/* 調べもの (Pico の設定画面 / ログ) */
struct tsp_stat {
  uint32_t xchg, dup, badreq, aborts;
  uint32_t connects, accepts, refused, dns;
  uint32_t tx_bytes, rx_bytes, dgrams_in, dgrams_out, udp_drop;
  uint32_t errmem;                      /* tcp_write の ERR_MEM (CREDIT の計算が正しければ 0) */
  uint32_t held_peak, sndq_peak;        /* Z に渡していない受信 / lwIP の送信キューの合計の最大 (バイト) */
};
extern struct tsp_stat tsp_stat;

#endif
