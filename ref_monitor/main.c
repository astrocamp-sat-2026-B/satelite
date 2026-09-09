/* main.c
 *
 * 模擬衛星 フォトリフレクタ生データ取得 — おおもとのファイル
 * ターゲット: Raspberry Pi Pico W / Pico SDK (C)
 *
 * センサモジュールの関数を呼ぶだけで生データが取れる。
 * 周期制御・出力形式・単位換算・判定は、すべてこのファイルの責務。
 *
 * 出力: raw値 (0-1023) のみ。1行1サンプル。
 *       異常値は 65535 (PHOTOREFLECTOR_INVALID) として出る。
 */

#include <stdio.h>
#include <stdint.h>

#include "pico/stdlib.h"

#include "photoreflector.h"

/* サンプリング周期。周期制御は呼び出し側=ここに置く方針。 */
#define SAMPLE_INTERVAL_MS   2

int main(void)
{
    stdio_init_all();

    /* USBシリアルがホストに認識され、シリアルモニタが開かれるまでの猶予。
     * これが無いと最初の数行が欠ける。 */
    sleep_ms(3000);

    /* センサの初期化。読み出しより先に1回だけ。 */
    photoreflector_init();

    /* 周期制御は sleep_ms ではなく、次回の起床時刻を積み上げる方式にする。
     * sleep_ms(2) を回すと ADC読み出し(約24us)と printf の所要時間が
     * 毎回上乗せされ、実際の周期が 2ms より長くなるうえ printf の長さで
     * 変動する。累積するとサンプル数と実時間の対応がずれるため、
     * 後で回転数を算出する段になって周波数の誤差として効いてくる。 */
    absolute_time_t next = get_absolute_time();

    while (true) {
        next = delayed_by_ms(next, SAMPLE_INTERVAL_MS);

        /* --- ここが「関数を呼ぶだけ」の部分 --- */
        uint16_t raw = photoreflector_read_raw();
        /* ------------------------------------- */

        printf("%u\n", raw);

        /* 処理が周期に間に合わなかった場合、sleep_until は即座に戻り
         * 次の周期で追いつこうとする。恒常的に間に合わないなら
         * SAMPLE_INTERVAL_MS を延ばすか、出力を減らすこと。 */
        sleep_until(next);
    }

    return 0;
}
