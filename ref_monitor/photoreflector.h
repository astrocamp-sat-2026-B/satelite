/* photoreflector.h
 *
 * フォトリフレクタ LBR-127HLD (Letex Technology)
 * MAIN基板 / MCP3008 CH4 (ネット名 REF)
 * リアクションホイールの回転を光学的に検出する。
 *
 * 使い方:
 *     #include "photoreflector.h"
 *
 *     photoreflector_init();                        // 起動時に1回
 *     uint16_t raw = photoreflector_read_raw();     // 以降は何度でも
 *
 * MCP3008(ADC)をSPIで叩いているが、それはこのモジュールの内部事情なので
 * 呼び出し側は意識しなくてよい。
 */

#ifndef PHOTOREFLECTOR_H
#define PHOTOREFLECTOR_H

#include <stdint.h>

/* 読み出し異常を表す値。有効値は 0-1023 なので衝突しない。 */
#define PHOTOREFLECTOR_INVALID   0xFFFFu

/* 初期化する。読み出しより先に、起動時に1回だけ呼ぶこと。
 * 複数回呼んでも害は無い。 */
void photoreflector_init(void);

/* 生のADC値を返す。
 *   正常時 : 0-1023  (VREF=3.3V なので raw/1023*3.3 [V] に相当)
 *   異常時 : PHOTOREFLECTOR_INVALID
 *            (未初期化 / 受信ビット位置ずれ)
 *
 * 1回の呼び出しで約24us ブロックする。
 * 電圧換算・しきい値判定・回転数算出は呼び出し側の責務。 */
uint16_t photoreflector_read_raw(void);

#endif /* PHOTOREFLECTOR_H */
