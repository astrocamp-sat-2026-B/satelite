/*
 * mcp3008_pot_test.c
 *
 * 目的：MCP3008(ADC) の動作確認のみを行うテストコード。
 *      可変抵抗（ポテンショメータ）を分圧器として CH7 に接続し、
 *      つまみを回したときに読み取り値(0-1023)と電圧(V)が
 *      追従して変化することを確認する。
 *
 * 対象ボード: Raspberry Pi Pico W (RP2040)
 * SDK       : Raspberry Pi Pico C/C++ SDK
 *
 * 配線 (模擬衛星 MAIN基板のMCP3008配線に準拠):
 *   Pico GP16 (物理21pin, SPI0 RX / MISO)  - MCP3008 pin12 DOUT
 *   Pico GP17 (物理22pin, 汎用GPIO / CS)   - MCP3008 pin10 CS/SHDN
 *   Pico GP18 (物理24pin, SPI0 SCK)        - MCP3008 pin13 CLK
 *   Pico GP19 (物理25pin, SPI0 TX / MOSI)  - MCP3008 pin11 DIN
 *   Pico 3V3(OUT) (物理36pin)              - MCP3008 pin16 VDD, pin15 VREF
 *   Pico GND                               - MCP3008 pin9 DGND, pin14 AGND
 *
 *   ポテンショメータ(3端子):
 *     端子A -> 3.3V
 *     端子B(ワイパー、中央) -> MCP3008 pin8 CH7
 *     端子C -> GND
 *
 *   ※ CH7 を選んだ理由: 実機PCBではCH0-3=フォトダイオード,
 *     CH4=フォトリフレクタ, CH5=バッテリ電圧監視に使用中で、
 *     CH6/CH7 は未接続。ブレッドボード試験でここを使えば
 *     実機の配線を汚さずに済む。
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

// ---- ピン定義 ----
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19

#define SPI_PORT spi0

// MCP3008の最大クロックはVDD=5Vで3.6MHz、VDD=2.7Vで1.35MHz
// (データシート記載)。今回はVDD=3.3V運用なので、まずは
// 1MHzという十分低い値で確実に動かすことを優先する。
// 動作確認が取れたら上げてみて壊れないか試すのもアリ。
#define SPI_BAUD (1000 * 1000)

// 実機PCBで未使用のCH7を使う(上記コメント参照)
#define ADC_CHANNEL 7

// Pico側のVREFは3.3V (VDDと共通)。ここを変えたら合わせて直すこと。
#define VREF_VOLT 3.3f

static inline void cs_select(void) {
    gpio_put(PIN_CS, 0);
}

static inline void cs_deselect(void) {
    gpio_put(PIN_CS, 1);
}

// MCP3008からシングルエンドで1チャンネル読む。
// 戻り値は 0〜1023 (10bit)。
static uint16_t mcp3008_read(uint8_t channel) {
    uint8_t tx[3];
    uint8_t rx[3];

    tx[0] = 0x01;                                  // スタートビット
    tx[1] = (uint8_t)((0x08 | (channel & 0x07)) << 4); // SGL/DIFF=1(シングルエンド) + チャンネル選択
    tx[2] = 0x00;                                  // ダミー(残りのクロックを送るだけ)

    cs_select();
    spi_write_read_blocking(SPI_PORT, tx, rx, 3);
    cs_deselect();

    // rx[1]の下位2bit + rx[2]の8bit = 10bit結果
    uint16_t result = (uint16_t)((rx[1] & 0x03) << 8) | rx[2];
    return result;
}

int main(void) {
    stdio_init_all();

    // SPI0を1MHz、モード0,0(CPOL=0, CPHA=0)で初期化。
    // MCP3008データシートは "mode 0,0 and 1,1" に対応と記載があるので0,0を使う。
    spi_init(SPI_PORT, SPI_BAUD);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // CSはハードウェアSPIに任せず、自分でH/Lを制御する
    // (MCP3008は1回の変換に3バイトのやり取りが必要で、
    //  その間だけCSをLに保つ必要があるため)
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1); // アイドル時はH(未選択)

    sleep_ms(2000); // USBシリアルの接続待ち(PCで確認するため)

    printf("MCP3008 ADC test start. CH%d を読み続けます。\n", ADC_CHANNEL);
    printf("可変抵抗のつまみを回して、値が追従して変わるか確認してください。\n");

    while (true) {
        uint16_t raw = mcp3008_read(ADC_CHANNEL);
        float voltage = ((float)raw / 1023.0f) * VREF_VOLT;

        printf("CH%d  raw=%4u / 1023   voltage=%.3f V\n", ADC_CHANNEL, raw, voltage);

        sleep_ms(200);
    }

    return 0;
}
