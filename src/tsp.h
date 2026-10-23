/*
 * tsp.h -- tis.x (X68000 Z) と Pico のソケット窓口 (pico/tsp_svc.c) の間の記録形式 ("TSP")
 *
 * npd.x の X6NR 交換 (IP パケットを長さ付きレコードで運ぶ) に、先頭 1 バイトが TSP_MARK (0xA6) のレコードを混ぜる。
 * 暗号アクセラレータの 0xA5 (net/accel/accel_proto.h) と同じ流儀で、IPv4 のバージョン nibble (4) と重ならない。
 *   Z  -> Pico: [長さ 2][TSP 要求レコード][偶数詰め物]
 *   Pico -> Z: [長さ 2][空き 14][TSP 応答レコード][偶数詰め物]
 * 1 回の交換に要求 1 個・応答 1 個。どちらも見出し (12 バイト) + メッセージの列。
 *
 *   要求の見出し: [MARK][ver][flags][0][zseq 2][0 2][zepoch 4]
 *   応答の見出し: [MARK][ver][flags][0][zack 2][0 2][pepoch 4]
 *   メッセージ:   [type][slot][gen][0][len 2][本体 len][偶数詰め物]
 *
 * 信頼性 (止まって待つ): Z は要求に番号 zseq を振り、応答の zack が zseq と一致するまで **同じ要求を送り直す**
 * (npd.x の交換は書き込み / 読み込みの失敗でレコードを失うことがある)。Pico は最後の応答を持っておき、
 * zseq が前回と同じなら中身を適用せずに同じ応答を返す。1 回の交換が要求 1 個と応答 1 個なので窓は 1 で足りる。
 *
 * epoch: 要求の zepoch は tis.x の常駐ごと、応答の pepoch は Pico の起動ごとに変わる乱数。Pico は知らない zepoch を
 * 見たら持っているソケットを全部捨てる。Z は pepoch が変わったら (Pico の再起動) Pico 側のソケットを全部
 * "Network is down" にする。
 *
 * slot = Z のソケット番号 - 128 (0〜31)、gen = その番号を使い直すたびに増やす数。Pico からのメッセージの gen が
 * Z の今の gen と違えば捨てる (交換の途中で閉じて開き直した番号に、前のソケットのデータを入れないため)。
 *
 * 値はすべて明示的なバイト列 (ビッグエンディアン)。Z (68000) と Pico (ARM) で同じこのヘッダを使う。
 */
#ifndef TSP_H
#define TSP_H
#include <stdint.h>

#define TSP_MARK        0xa6
#define TSP_VERSION     1
#define TSP_HDR         12
#define TSP_MHDR        6
#define TSP_NSLOT       32
#define TSP_REQ_MAX     4078            /* npd の 1 交換 (8 セクタ - 見出し 16 = 4080) - レコードの長さ 2 */
#define TSP_RES_MAX     4064            /* 4080 - Pico → Z のレコードの [長さ 2][空き 14] */
#define TSP_NAME_MAX    255

/* 要求の flags */
#define TSP_ZF_RESET    0x01            /* Pico 側のソケットを全部捨てる (tis.x -r) */
/* 応答の flags */
#define TSP_PF_MORE     0x01            /* Pico に Z へ送れるものがまだある (すぐ次の交換を) */

/* Z -> Pico */
#define TSP_C_CONNECT   0x01            /* [proto][flags][lport 2][rport 2][addr 4][名前...]  -> CONNECTED / ERROR */
#define TSP_C_LISTEN    0x02            /* [backlog][0][lport 2][laddr 4]                   -> BOUND / ERROR, ACCEPT ... */
#define TSP_C_UDPOPEN   0x03            /* [0][0][lport 2][laddr 4]                         -> BOUND / ERROR */
#define TSP_C_DATA      0x04            /* TCP のデータ (CREDIT の範囲で) */
#define TSP_C_SENDTO    0x05            /* [addr 4][port 2][データ]  UDP (UDPOPEN が済んでいなければ自動で開く) */
#define TSP_C_WINDOW    0x06            /* [free 4]  Z の受信リングの空き (この要求を作った時点の絶対値) */
#define TSP_C_SHUT      0x07            /* [how]  1 = 送信を閉じる (FIN)。0 は Z の中だけで済む */
#define TSP_C_CLOSE     0x08            /* 閉じる (Pico は残りを送ってから FIN)。返事は無い */
#define TSP_C_ADOPT     0x09            /* [handle][aflags][0][0][free 4]  ACCEPT された子をこの slot / gen で引き取る */
#define TSP_C_REJECT    0x0a            /* [handle]  引き取れない (Z の表が一杯): Pico は切る */
#define TSP_C_RESOLVE   0x0b            /* slot = 問い合わせ番号 (0〜TSP_NQUERY-1)、本体 = 名前  -> RESOLVED */

/* Pico -> Z */
#define TSP_E_CONNECTED 0x81            /* [laddr 4][lport 2][raddr 4][rport 2] */
#define TSP_E_ACCEPT    0x82            /* (slot = listen) [handle][0][laddr 4][lport 2][raddr 4][rport 2] */
#define TSP_E_DATA      0x83            /* TCP のデータ (WINDOW の範囲で) */
#define TSP_E_DGRAM     0x84            /* [addr 4][port 2][データ]  UDP 1 個 */
#define TSP_E_CREDIT    0x85            /* [credit 4]  Z がこの応答のあと送ってよいバイト数 (絶対値) */
#define TSP_E_EOF       0x86            /* 相手が送信を閉じた (データの後に来る) */
#define TSP_E_ERROR     0x87            /* [code]  Pico 側のソケットは無くなった */
#define TSP_E_RESOLVED  0x88            /* (slot = 問い合わせ番号) [code][0][addr 4] */
#define TSP_E_BOUND     0x89            /* [lport 2]  LISTEN / UDPOPEN が済んだ (実際のポート) */

#define TSP_PROTO_TCP   0
#define TSP_CF_NAME     0x01            /* CONNECT に名前がある (addr が 0 なら Pico が名前を引いてつなぐ。IPv6 もここ) */
#define TSP_CF_TLS      0x02            /* (予約) Pico が TLS を話す。名前は SNI。いまは NOTSUP */
#define TSP_AF_LISTENER 0x01            /* ADOPT: listen したソケット自身が接続になる (backlog 0)。Pico は listen をやめる */

#define TSP_NQUERY      4

/* npd.x が tis.x の入口 (text + 8、d0 = 命令, a0 = 引数) を呼ぶ命令。tis.x -p は常駐するとき入口を npd.sys に
   預け (ETD 25)、npd.x は交換のたびに ETD 26 で受け取る (tis.x -r で 0 に戻すので、解放された入口は呼ばない) */
#define TIS_NP_OUT      0x7470          /* a[0] = 書く先, a[1] = 最大 (0 なら問い合わせだけ), a[2] <- 急ぎ。戻り値 = 長さ */
#define TIS_NP_IN       0x7471          /* a[0] = 長さ, a[1] = TSP レコード。戻り値 bit 0 = Pico に続きあり */
#define NPD_ETD_TSP_SET 25
#define NPD_ETD_TSP_GET 26

/* ERROR / RESOLVED の code */
#define TSP_OK          0
#define TSP_ERR_REFUSED 1
#define TSP_ERR_TIMEOUT 2
#define TSP_ERR_UNREACH 3
#define TSP_ERR_RESET   4
#define TSP_ERR_NOMEM   5
#define TSP_ERR_NONAME  6
#define TSP_ERR_NOTSUP  7
#define TSP_ERR_INUSE   8
#define TSP_ERR_ABORTED 9
#define TSP_ERR_BADREQ  10
#define TSP_ERR_NETDOWN 11

static inline void tsp_put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline unsigned tsp_get16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static inline void tsp_put32(uint8_t *p, uint32_t v) { tsp_put16(p, v >> 16); tsp_put16(p + 2, v & 0xffff); }
static inline uint32_t tsp_get32(const uint8_t *p) { return ((uint32_t)tsp_get16(p) << 16) | tsp_get16(p + 2); }

/* メッセージの見出しを書く。戻り値: 本体の先頭 */
static inline uint8_t *tsp_msg(uint8_t *p, int type, int slot, int gen, unsigned len)
{
  p[0] = (uint8_t)type;
  p[1] = (uint8_t)slot;
  p[2] = (uint8_t)gen;
  p[3] = 0;
  tsp_put16(p + 4, len);
  return p + TSP_MHDR;
}

/* メッセージ 1 個の大きさ (見出しと詰め物を含む) */
static inline unsigned tsp_msize(unsigned len) { return TSP_MHDR + ((len + 1) & ~1u); }

static inline const char *tsp_errstr(int code)
{
  switch (code) {
  case TSP_ERR_REFUSED: return "Connection refused";
  case TSP_ERR_TIMEOUT: return "Connection timed out";
  case TSP_ERR_UNREACH: return "Network unreachable";
  case TSP_ERR_RESET:   return "Connection reset";
  case TSP_ERR_NOMEM:   return "No buffer space";
  case TSP_ERR_NONAME:  return "Host not found";
  case TSP_ERR_NOTSUP:  return "Operation not supported";
  case TSP_ERR_INUSE:   return "Address already in use";
  case TSP_ERR_ABORTED: return "Connection aborted";
  case TSP_ERR_NETDOWN: return "Network is down";
  default:              return "I/O error";
  }
}

#endif
