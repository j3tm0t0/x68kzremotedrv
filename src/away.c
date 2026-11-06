/*
 * away.c -- 外出用の設定 (家の外で SD カードから起動し、スマートフォンのテザリングにつなぐ)
 *
 * SD カードに AWAY_FILE (CONFIG.TXT と同じ「項目: 値」の行) があれば、起動時に Wi-Fi をスキャンして家の SSID
 * (フラッシュの WIFI_SSID) を探す。見つからなければ、そのファイルの項目でメモリ上の設定を上書きする。
 * Z が HDS を読みに来る (電源投入から約 35 秒) より前に決めるので、起動の途中で HDS を差し替えない。
 *
 *   AWAY: auto | always | off      auto (既定): 家の SSID が見えないときだけ / always: いつも (試験用) / off: 使わない
 *   WIFI_SSID: / WIFI_PASSWORD:    外でつなぐ Wi-Fi (パスワードは 63 文字まで。フラッシュの欄は 15 文字なので別に持つ)
 *   IPADDR: / NETMASK: / GATEWAY: / DNS:  外の Wi-Fi での固定 IP。書かなければ (空欄も) DHCP。家の設定 (フラッシュ) は上書きしない
 *   ほかの項目 (HDS0:、SMB2_SERVER: など): 書いた項目だけ上書き。空欄も空欄で上書きする (SMB2_SERVER: 空欄 = SMB を使わない)
 *
 * IP の設定はつなぐ SSID で決める (away_ip_for)。家の SSID (起動時のフラッシュの WIFI_SSID) なら家の固定 IP、away.txt の SSID なら
 * away.txt の IP (既定 DHCP)、それ以外 (設定画面で別の SSID を選んだ) は DHCP。家の固定 IP のまま設定画面でテザリングに
 * つなぎ直し、192.168.1.4 のままになっていた。家か外かを起動時に決めるのは今までどおり (動作中に自動では切り替えない)。
 *
 * SSID とパスワードは表示しない (UART のログにも出さない)。
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "config_file.h"
#include "vd_command.h"
#include "sdhds.h"
#include "away.h"

bool away;
bool away_nosmb;

static char home_ssid[sizeof config.wifi_ssid + 1];    /* 起動時のフラッシュの WIFI_SSID */
static bool have_file;                  /* away.txt を読んだ (AWAY: off でない) */
static bool user_wifi;                  /* 設定画面で Wi-Fi を選び直した: away.txt の SSID より設定画面のものを使う */
static char wifi_ssid[33];
static char wifi_passwd[64];
static struct away_ip away_ip;          /* away.txt の IPADDR: など (空 = DHCP) */
static char text[2048];
static volatile bool home_seen;

static int scan_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    if (r != NULL && r->ssid_len > 0 && r->ssid_len == strlen(config.wifi_ssid) &&
        memcmp(r->ssid, config.wifi_ssid, r->ssid_len) == 0)
        home_seen = true;
    return 0;
}

/* 家の SSID が見えるか。1 回のスキャンは数秒。見落としを避けて 2 回まで */
static bool home_visible(void)
{
    if (config.wifi_ssid[0] == '\0')
        return false;
    home_seen = false;
    for (int n = 0; n < 2 && !home_seen; n++) {
        cyw43_wifi_scan_options_t opt = {0};
        if (cyw43_wifi_scan(&cyw43_state, &opt, NULL, scan_cb) != 0) {
            printf("away: scan failed\n");
            return true;                /* 分からないときは家の設定のまま */
        }
        for (int i = 0; i < 50 && cyw43_wifi_scan_active(&cyw43_state); i++)
            vTaskDelay(pdMS_TO_TICKS(200));
    }
    return home_seen;
}

/* "ITEM:" の行の値を dst に取り出し、行を消す (config_parse に渡さない) */
static bool take(const char *item, char *dst, size_t size)
{
    size_t n = strlen(item);
    for (char *p = text; *p != '\0'; ) {
        char *e = p + strcspn(p, "\r\n");
        if (strncasecmp(p, item, n) == 0) {
            char *v = p + n;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t len = e - v;
            while (len > 0 && (v[len - 1] == ' ' || v[len - 1] == '\t'))
                len--;
            if (dst != NULL) {
                if (len >= size)
                    len = size - 1;
                memcpy(dst, v, len);
                dst[len] = '\0';
            }
            memset(p, ' ', e - p);      /* 行を空白にする (config_parse はどの項目にも当たらない行を飛ばす) */
            *p = '#';
            return true;
        }
        p = *e != '\0' ? e + 1 : e;
    }
    return false;
}

void away_check(void)
{
    char mode[16] = "auto";

    memcpy(home_ssid, config.wifi_ssid, sizeof config.wifi_ssid);
    sdc_init();
    if (sdc_read_file(AWAY_FILE, text, sizeof text) < 0)
        return;
    take("AWAY:", mode, sizeof mode);
    if (strcasecmp(mode, "off") == 0) {
        printf("away: off\n");
        return;
    }
    /* SSID とパスワードと IP は家にいても控える (設定画面で away.txt の SSID を選んだときに使う) */
    have_file = true;
    take("WIFI_SSID:", wifi_ssid, sizeof wifi_ssid);
    take("WIFI_PASSWORD:", wifi_passwd, sizeof wifi_passwd);
    take("IPADDR:", away_ip.ipaddr, sizeof away_ip.ipaddr);
    take("NETMASK:", away_ip.netmask, sizeof away_ip.netmask);
    take("GATEWAY:", away_ip.gateway, sizeof away_ip.gateway);
    take("DNS:", away_ip.dns, sizeof away_ip.dns);
    if (strcasecmp(mode, "always") != 0 && home_visible()) {
        printf("away: home Wi-Fi seen, normal settings\n");
        return;
    }
    /* "#" の行は空白にする (config_parse は行の途中の漢字で止まり、その後ろの「項目:」を拾うことがある) */
    for (char *p = text; *p != '\0'; ) {
        char *e = p + strcspn(p, "\r\n");
        if (*p == '#')
            memset(p, ' ', e - p);
        p = *e != '\0' ? e + 1 : e;
    }
    config_parse((uint8_t *)text);
    away = true;
    away_nosmb = config.smb2_server[0] == '\0';
    printf("away: on (%s)%s%s, HDS0 %s, %s\n", mode,
           wifi_ssid[0] ? ", own Wi-Fi" : "", away_nosmb ? ", no SMB" : "",
           config.hds[0], away_ip.ipaddr[0] ? away_ip.ipaddr : "DHCP");
}

void away_user_wifi(void)
{
    user_wifi = true;
}

/* ssid が away.txt の Wi-Fi か (away.txt に SSID が無ければ、外出中の家の SSID が外の Wi-Fi) */
static bool is_away_ssid(const char *ssid)
{
    if (!have_file)
        return false;
    if (wifi_ssid[0])
        return strcmp(ssid, wifi_ssid) == 0;
    return away && strcmp(ssid, home_ssid) == 0;
}

const char *away_wifi_ssid(void)
{
    return away && wifi_ssid[0] && !user_wifi ? wifi_ssid : config.wifi_ssid;
}

/* away.txt の SSID なら away.txt のパスワード (63 文字まで。設定画面の欄は 15 文字) */
const char *away_wifi_passwd(void)
{
    return wifi_ssid[0] && is_away_ssid(away_wifi_ssid()) ? wifi_passwd : config.wifi_passwd;
}

/* 家の Wi-Fi は今までどおり WPA2。外 (スマートフォン) は WPA3 だけのこともあるので、WPA2 と WPA2/WPA3 を交互に */
uint32_t away_wifi_auth(int attempt)
{
    if (strcmp(away_wifi_ssid(), home_ssid) == 0 || (attempt & 1) == 0)
        return CYW43_AUTH_WPA2_AES_PSK;
    return CYW43_AUTH_WPA3_WPA2_AES_PSK;
}

const struct away_ip *away_ip_for(const char *ssid)
{
    static struct away_ip ip;
    if (is_away_ssid(ssid)) {
        ip = away_ip;
    } else if (strcmp(ssid, home_ssid) == 0) {
        /* 家: フラッシュ (設定画面) の固定 IP */
        snprintf(ip.ipaddr, sizeof ip.ipaddr, "%s", config.ipaddr);
        snprintf(ip.netmask, sizeof ip.netmask, "%s", config.netmask);
        snprintf(ip.gateway, sizeof ip.gateway, "%s", config.gateway);
        snprintf(ip.dns, sizeof ip.dns, "%s", config.dns);
    } else {
        return NULL;
    }
    return ip.ipaddr[0] ? &ip : NULL;
}
