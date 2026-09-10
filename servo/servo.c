#include "stdio.h"              // 標準入出力用のライブラリ
#include "stdint.h"
#include "pico/stdlib.h"        // Picoの標準ライブラリ
#include "hardware/gpio.h"
#include "hardware/pwm.h"


#define SERVO_PIN 11  // 使用する GPIO ピン番号（PWM 可能なピンを指定）


// サーボ信号の基本仕様
// 周期：20ms（50Hz）
// パルス幅：1000〜2000us（逆転〜正転）
// 1500us が停止

// パルス幅を設定する関数
void servo_write(uint pin, uint pulse_width_us) {
    uint slice = pwm_gpio_to_slice_num(pin);
    pwm_set_gpio_level(pin, pulse_width_us);  // wrap=20000 のとき、値はそのまま us
}

void main(void) {
    // USB通信の初期化
    stdio_init_all();

    // GPIO ピンを PWM 用に初期化
    gpio_set_function(SERVO_PIN, GPIO_FUNC_PWM);  // GPIO を PWM に設定
    uint slice = pwm_gpio_to_slice_num(SERVO_PIN);

    // 50Hz を作るための設定
    // 125MHz / clkdiv / (wrap + 1) = 50Hz
    pwm_set_clkdiv(slice, 125.0);   // 分周
    pwm_set_wrap(slice, 20000);     // 20ms = 20000us

    pwm_set_enabled(slice, true);

    while (true) {
        // 速くなる（正転方向）
        for (uint pulse = 1500; pulse <= 2000; pulse += 10) {
            servo_write(SERVO_PIN, pulse);
            sleep_ms(20);
        }

        // 遅くなる（正転 → 停止方向）
        for (uint pulse = 2000; pulse >= 1500; pulse -= 10) {
            servo_write(SERVO_PIN, pulse);
            sleep_ms(20);
        }

        // 逆転方向に速くなる
        for (uint pulse = 1500; pulse >= 1000; pulse -= 10) {
            servo_write(SERVO_PIN, pulse);
            sleep_ms(20);
        }

        // 逆転方向に遅くなる（停止へ戻る）
        for (uint pulse = 1000; pulse <= 1500; pulse += 10) {
            servo_write(SERVO_PIN, pulse);
            sleep_ms(20);
        }
    }
}