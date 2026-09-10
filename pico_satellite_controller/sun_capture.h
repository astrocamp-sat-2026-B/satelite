#ifndef SUN_CAPTURE_H
#define SUN_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

/* photodiode_adc[] 内で「カメラ側」太陽センサーのペアとして使うチャンネル。 */
#define SUN_CAPTURE_CHANNEL_A 2
#define SUN_CAPTURE_CHANNEL_B 3

/* MCP3008は10bit ADCなので有効範囲は0〜1023。 */
#define SUN_CAPTURE_ADC_MAX 1023u

void sun_capture_init(void);

/* 最新のphotodiode_adc[4]を渡して自動撮影条件を評価する。
   「2chの差がtolerance以内」かつ「両方threshold以上」の状態に
   *新規に* 入った瞬間（エッジ）だけtrueを返す。
   条件が成立し続けている間はfalseを返し続けるため、
   呼び出し側は戻り値がtrueのときだけ撮影をキューイングすればよい。 */
bool sun_capture_update(const uint16_t photodiode_adc[4]);

uint16_t sun_capture_get_threshold(void);
uint16_t sun_capture_get_tolerance(void);

/* 0〜SUN_CAPTURE_ADC_MAX の範囲外はクランプする。 */
void sun_capture_set_threshold(uint16_t threshold);
void sun_capture_set_tolerance(uint16_t tolerance);

#endif
