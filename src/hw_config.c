/*
 * hw_config.c -- SD カード (SPI 接続) の配線 (no-OS-FatFS-SD-SDIO-SPI-RPi-Pico 用)
 *
 *   SD の基板    Pico W / Pico 2 W (既定。cmake -DSD_PINS=usb なら GP2 SCK / GP3 MOSI / GP4 MISO / GP5 CS。
 *                ほかの番号は SD_SCK / SD_MOSI / SD_MISO / SD_CS / SD_SPI_HZ を定義する)
 *   3V3          36 番ピン (3V3 OUT)
 *   GND          38 番ピン
 *   SCK          GP18 (24 番ピン)
 *   MOSI (DI)    GP19 (25 番ピン)
 *   MISO (DO)    GP20 (26 番ピン)
 *   CS           GP21 (27 番ピン)
 * Pico 2 WH (2026-10-23): DAT1・DAT2・CS・MISO に 10k の外付けプルアップ (RP2350 の E9)。25MHz で 1MB の書き・読み・照合を
 * 330 回続けて失敗 0 (x68k-zos pico/test/sdsmoke)、37.5MHz はマウントできなかった
 * GP0 / GP1 は UART のログ、Wi-Fi (CYW43) は内部の GPIO を使うので、SPI0 の既定の位置が空いている
 */
#include "hw_config.h"

#ifndef SD_SCK
#define SD_SCK      18
#endif
#ifndef SD_MOSI
#define SD_MOSI     19
#endif
#ifndef SD_MISO
#define SD_MISO     20
#endif
#ifndef SD_CS
#define SD_CS       21
#endif
#ifndef SD_SPI_HZ
#define SD_SPI_HZ   (25 * 1000 * 1000)
#endif

static spi_t spi = {
    .hw_inst = spi0,
    .sck_gpio = SD_SCK,
    .mosi_gpio = SD_MOSI,
    .miso_gpio = SD_MISO,
    .baud_rate = SD_SPI_HZ,
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = SD_CS,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num() { return 1; }

sd_card_t *sd_get_by_num(size_t num)
{
    return num == 0 ? &sd_card : NULL;
}
