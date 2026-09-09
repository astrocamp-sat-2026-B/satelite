/* photoreflector.c
 *
 * フォトリフレクタ LBR-127HLD (MCP3008 CH4) の実装。
 * ADC(MCP3008)を叩く処理もこのファイルに内包している。
 *
 * 2通りの使い方ができる。
 *
 *   [1] 部品として使う (通常)
 *       main.c から photoreflector_init() / photoreflector_read_raw()
 *       を呼ぶ。ファイル末尾のテスト用 main() はコンパイルされない。
 *
 *   [2] 単体動作確認 (このファイルだけでビルドする)
 *       PHOTOREFLECTOR_TEST_MAIN を定義してビルドすると、
 *       ファイル末尾のテスト用 main() が有効になり、この1ファイルだけで
 *       動く実行ファイルになる。
 *           cmake -B build -DPHOTOREFLECTOR_TEST=ON
 */

#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"

#include "photoreflector.h"

/* ==================================================================
 * 内部設定 — このファイルの外には出さない
 * ================================================================== */

/* MCP3008の配線。MAIN基板に合わせて固定。変更しないこと。 */
#define SPI_PORT        spi0
#define SPI_BAUD_HZ     (1000 * 1000)   /* 1MHz */
#define PIN_MISO        16   /* Pico物理21 -> MCP3008 pin12 (DOUT)    */
#define PIN_CS          17   /* Pico物理22 -> MCP3008 pin10 (CS/SHDN) */
#define PIN_SCK         18   /* Pico物理24 -> MCP3008 pin13 (CLK)     */
#define PIN_MOSI        19   /* Pico物理25 -> MCP3008 pin11 (DIN)     */

/* このセンサが繋がっているADCチャンネル。
 * ブレッドボードで単体確認する場合は、実機で未使用の 7 に変更する。 */
#define CH_PHOTOREFLECTOR   4

/* 初期化済みフラグ。
 * 未初期化のまま読むと、設定されていないSPIから「それらしい数字」が
 * 返ってきてしまい、原因の特定が難しいバグになる。明示的に弾く。 */
static bool s_initialized = false;

/* ==================================================================
 * 内部処理 : MCP3008 (ADC)
 *
 * static を付けてこのファイルの外から見えないようにしている。
 * 将来フォトダイオード(CH0-3)やバッテリ電圧(CH5)を足すときは、
 * このセクションを mcp3008.c / mcp3008.h として切り出し、
 * static を外して共有する。
 * ================================================================== */

/* MCP3008は「スタートビット + SGL/DIFF + チャンネル番号」を送りながら
 * 3バイトのSPIトランザクションで変換結果を受け取る。この3バイトの
 * やり取りの間、CSをずっとLowに保っておく必要がある。
 * SPIペリフェラルのハードウェア自動CS制御を使うと、1バイト転送ごとに
 * CSがHigh/Lowしてしまい、MCP3008側の変換シーケンスが途中でリセット
 * されて壊れた値が返る。そのためCSは自前でGPIO制御している。 */
static uint16_t adc_read(uint8_t channel)
{
    if (!s_initialized || channel > 7) {
        return PHOTOREFLECTOR_INVALID;
    }

    uint8_t tx[3];
    uint8_t rx[3];

    tx[0] = 0x01;                               /* スタートビット      */
    tx[1] = (uint8_t)(0x80 | (channel << 4));   /* SGL/DIFF=1 + D2D1D0 */
    tx[2] = 0x00;                               /* 受信用のダミー      */

    gpio_put(PIN_CS, 0);
    spi_write_read_blocking(SPI_PORT, tx, rx, 3);
    gpio_put(PIN_CS, 1);

    /* MCP3008は10bit値の直前に必ず0のヌルビットを出す。
     * このフレーミングでは rx[1] の bit2 がそれにあたる。
     * ここが1なら受信ビットの位置がずれている証拠なので
     * (SPIモード違い / CS制御ミス / 配線不良 / クロック速すぎ 等)、
     * 「それらしい数値」を返さずに異常として扱う。
     *
     * ※正常な配線でも常に異常判定になる場合は、このifを一時的に外し、
     *   rx[] を直接printfしてビット位置を実測で確認すること。 */
    if (rx[1] & 0x04) {
        return PHOTOREFLECTOR_INVALID;
    }

    return (uint16_t)(((rx[1] & 0x03) << 8) | rx[2]);
}

/* ==================================================================
 * 公開API
 * ================================================================== */

void photoreflector_init(void)
{
    spi_init(SPI_PORT, SPI_BAUD_HZ);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    /* CSはSPIペリフェラルの自動制御に任せず、ただのGPIO出力として使う。
     * 理由は adc_read() のコメントを参照。 */
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);   /* アイドル時はHigh(未選択) */

    s_initialized = true;
}

uint16_t photoreflector_read_raw(void)
{
    return adc_read(CH_PHOTOREFLECTOR);
}

/* ==================================================================
 * 単体動作確認用の main()
 *
 * PHOTOREFLECTOR_TEST_MAIN が定義されているときだけコンパイルされる。
 * 定義されていなければ、この下は丸ごと存在しないのと同じになるので、
 * main.c と一緒にビルドしても main() が二重定義になることはない。
 *
 * 生データが読めているかを確認するための最小構成。
 * 出力は raw値 (0-1023) のみ、1行1サンプル、100ms間隔。
 * 異常時は 65535 が出る。
 * ================================================================== */
#ifdef PHOTOREFLECTOR_TEST_MAIN

#include <stdio.h>

int main(void)
{
    stdio_init_all();

    /* USBシリアルがホストに認識されるまでの猶予。
     * これが無いと最初の数行が欠ける。 */
    sleep_ms(3000);

    photoreflector_init();

    while (true) {
        printf("%u\n", photoreflector_read_raw());

        /* 目視で追える速さにする。ここは動作確認用なので
         * 周期の正確さは問わず sleep_ms で十分。 */
        sleep_ms(100);
    }

    return 0;
}

#endif /* PHOTOREFLECTOR_TEST_MAIN */
