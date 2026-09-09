/*
 * ref_monitor.c
 *
 * 目的:
 *   MCP3008(ADC) 経由でフォトリフレクタ(LBR-127HLD, ネット名 REF)の
 *   生の読み値をひたすら出力し続けるだけの動作確認用プログラム。
 *
 * やらないこと:
 *   しきい値判定、エッジ検出、RPM算出、他チャンネル同時読み取り、
 *   ログのファイル保存や通信への載せ替え。
 *   (これらは今回の実測結果を見てから別途実装する)
 */

#include <stdio.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"

/* ------------------------------------------------------------------ */
/* ハードウェア設定 (MAIN基板の配線に合わせて固定。変更しないこと)      */
/* ------------------------------------------------------------------ */

#define SPI_PORT      spi0
#define SPI_BAUD_HZ   (1000 * 1000)   /* 1MHz */

#define PIN_MISO      16   /* MCP3008 DOUT (pin12) */
#define PIN_CS        17   /* MCP3008 CS/SHDN (pin10) */
#define PIN_SCK       18   /* MCP3008 CLK (pin13) */
#define PIN_MOSI      19   /* MCP3008 DIN (pin11) */

/* 読み取り対象チャンネル。
 * ブレッドボードで単体確認する場合は CH7 (実機未使用の空きチャンネル) に
 * 差し替える想定なので、ここ1箇所だけ変更すればよい。 */
#define REF_ADC_CH    4

#define VREF_MV       3300   /* MCP3008 VREF = VDD = 3.3V */
#define ADC_MAX       1023   /* 10bit */

/* ------------------------------------------------------------------ */
/* サンプリング設定                                                    */
/* ------------------------------------------------------------------ */

#define SAMPLE_INTERVAL_MS  2    /* 1サンプルあたりの間隔 */
#define WINDOW_SAMPLES       50   /* 1ウィンドウのサンプル数(≒100ms) */
#define BAR_WIDTH             40  /* ASCIIバーグラフの幅 */

/* MCP3008から1チャンネル分(10bit, 0〜1023)を読み出す。
 *
 * MCP3008は「スタートビット + SGL/DIFF + チャンネル番号」を送りながら
 * 3バイトのSPIトランザクションで変換結果を受け取る仕組みになっている。
 * この3バイトのやり取りの間、CSをずっとLowに保っておく必要がある。
 * Pico SDKのSPIペリフェラルによるハードウェア自動CS制御を使うと、
 * 実装によっては1バイト転送ごとにCSをHigh/Lowしてしまい、MCP3008側の
 * 変換シーケンスが途中でリセットされて壊れた値が返ってくる。
 * そのためCSはハードウェア自動制御を使わず、GP17をGPIOとして自前で
 * 制御し、3バイト分のspi_write_read_blockingが終わるまでLowを維持する。
 */
static uint16_t mcp3008_read(uint8_t channel)
{
    uint8_t tx[3];
    uint8_t rx[3];

    tx[0] = 0x01;                          /* スタートビット */
    tx[1] = 0x80 | (uint8_t)(channel << 4); /* SGL/DIFF=1 + チャンネル(D2D1D0) */
    tx[2] = 0x00;                          /* ダミー(下位バイト受信用) */

    gpio_put(PIN_CS, 0);
    spi_write_read_blocking(SPI_PORT, tx, rx, 3);
    gpio_put(PIN_CS, 1);

    /* rx[1]の下位2bitが結果の上位2bit(B9,B8)、rx[2]が下位8bit(B7-B0) */
    uint16_t value = (uint16_t)((rx[1] & 0x03) << 8) | rx[2];

    return value; /* 0〜1023 に収まる */
}

static void print_header(void)
{
    printf("\r\n");
    printf("================================================\r\n");
    printf(" REF (photoreflector) ADC raw monitor\r\n");
    printf("================================================\r\n");
    printf(" SPI      : %s, %d Hz, mode 0,0 (CPOL=0/CPHA=0)\r\n",
           "spi0", SPI_BAUD_HZ);
    printf(" GP16 MISO -> MCP3008 pin12 (DOUT)\r\n");
    printf(" GP17 CS   -> MCP3008 pin10 (CS/SHDN) *manual GPIO control*\r\n");
    printf(" GP18 SCK  -> MCP3008 pin13 (CLK)\r\n");
    printf(" GP19 MOSI -> MCP3008 pin11 (DIN)\r\n");
    printf(" ADC channel : CH%d (REF / LBR-127HLD)\r\n", REF_ADC_CH);
    printf(" VREF = VDD = %d.%03d V, 10bit (0-%d)\r\n",
           VREF_MV / 1000, VREF_MV % 1000, ADC_MAX);
    printf(" sampling : %dms interval, %d samples/window (=%dms)\r\n",
           SAMPLE_INTERVAL_MS, WINDOW_SAMPLES,
           SAMPLE_INTERVAL_MS * WINDOW_SAMPLES);
    printf("------------------------------------------------\r\n");
    printf(" NOTE: this program only prints raw values.\r\n");
    printf("       no threshold / edge detection / RPM here.\r\n");
    printf("================================================\r\n\r\n");
}

int main(void)
{
    stdio_init_all();

    /* USBシリアル接続直後は最初の数行が欠けやすいため、
     * ホスト側がシリアルモニタを開くまでの猶予として待つ。 */
    sleep_ms(3000);

    /* SPI0初期化: 1MHz, モード0,0 */
    spi_init(SPI_PORT, SPI_BAUD_HZ);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    /* CSはSPIペリフェラルの自動制御に任せず、単純なGPIO出力として使う。
     * 理由は mcp3008_read() のコメントを参照。 */
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1); /* アイドル時はHigh(未選択) */

    print_header();

    while (true) {
        uint16_t v_min = ADC_MAX;
        uint16_t v_max = 0;
        uint32_t sum = 0;

        for (int i = 0; i < WINDOW_SAMPLES; i++) {
            uint16_t raw = mcp3008_read(REF_ADC_CH);

            if (raw < v_min) v_min = raw;
            if (raw > v_max) v_max = raw;
            sum += raw;

            sleep_ms(SAMPLE_INTERVAL_MS);
        }

        uint32_t avg = sum / WINDOW_SAMPLES;
        uint32_t avg_mv = (avg * VREF_MV) / ADC_MAX;
        uint32_t pp = v_max - v_min; /* peak-to-peak (振れ幅) */

        /* 平均値を示すASCIIバーグラフ(0〜1023を0〜BAR_WIDTH文字に換算) */
        char bar[BAR_WIDTH + 1];
        int bar_len = (int)((avg * BAR_WIDTH) / ADC_MAX);
        if (bar_len > BAR_WIDTH) bar_len = BAR_WIDTH; /* 念のための安全域 */
        for (int i = 0; i < BAR_WIDTH; i++) {
            bar[i] = (i < bar_len) ? '#' : ' ';
        }
        bar[BAR_WIDTH] = '\0';

        printf("raw min=%4u max=%4u avg=%4u  ( %d.%03d V )  p-p=%4u  |%s|\r\n",
               v_min, v_max, (unsigned)avg,
               (int)(avg_mv / 1000), (int)(avg_mv % 1000),
               pp, bar);
    }

    return 0;
}
