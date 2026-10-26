#ifndef _AWAY_H_
#define _AWAY_H_

#include <stdbool.h>
#include <stdint.h>

/* 外出用の設定 (SD カードの AWAY_FILE)。家の SSID が見えないとき、起動時にメモリ上の設定を上書きする (フラッシュには書かない) */
#define AWAY_FILE   "/x68000z/away.txt"

extern bool away;                       /* 外出用の設定で動いている */
extern bool away_nosmb;                 /* 外出用の設定で SMB2_SERVER が空欄: SMB を使わない */

/* main_task で Wi-Fi の初期化のあと、net_setup と vd_init の前に 1 回 */
void away_check(void);
/* つなぐ Wi-Fi (外出中なら away.txt の SSID / パスワード) */
const char *away_wifi_ssid(void);
const char *away_wifi_passwd(void);
uint32_t away_wifi_auth(int attempt);

#endif  /* _AWAY_H_ */
