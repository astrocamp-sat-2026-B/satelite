#include "servo.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

#define SERVO_PIN 11
#define SERVO_PERIOD_US 20000
#define SERVO_STOP_PULSE_US 1500
#define SERVO_MIN_PULSE_US 700
#define SERVO_MAX_PULSE_US 2300
#define SERVO_UPDATE_INTERVAL_MS 20
#define SERVO_SLEW_STEP_US 16

static uint current_pulse_width_us = SERVO_STOP_PULSE_US;
static uint target_pulse_width_us = SERVO_STOP_PULSE_US;
static absolute_time_t next_update;

void servo_init(void) {
    uint slice = pwm_gpio_to_slice_num(SERVO_PIN);

    gpio_set_function(SERVO_PIN, GPIO_FUNC_PWM);
    pwm_set_clkdiv(slice, 125.0f);
    pwm_set_wrap(slice, SERVO_PERIOD_US);
    pwm_set_gpio_level(SERVO_PIN, SERVO_STOP_PULSE_US);
    pwm_set_enabled(slice, true);

    current_pulse_width_us = SERVO_STOP_PULSE_US;
    target_pulse_width_us = SERVO_STOP_PULSE_US;
    next_update = make_timeout_time_ms(SERVO_UPDATE_INTERVAL_MS);
}

void servo_set_speed(int32_t speed_percent) {
    if (speed_percent > 100) speed_percent = 100;
    if (speed_percent < -100) speed_percent = -100;

    // -100〜100 → 700〜2300 に線形変換
    target_pulse_width_us =
        SERVO_MIN_PULSE_US +
        (uint)((speed_percent + 100) * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) / 200);
}

void servo_update(void) {
    if (!time_reached(next_update)) return;

    next_update = make_timeout_time_ms(SERVO_UPDATE_INTERVAL_MS);

    if (current_pulse_width_us < target_pulse_width_us) {
        uint remaining = target_pulse_width_us - current_pulse_width_us;
        current_pulse_width_us +=
            remaining < SERVO_SLEW_STEP_US ? remaining : SERVO_SLEW_STEP_US;
    } else if (current_pulse_width_us > target_pulse_width_us) {
        uint remaining = current_pulse_width_us - target_pulse_width_us;
        current_pulse_width_us -=
            remaining < SERVO_SLEW_STEP_US ? remaining : SERVO_SLEW_STEP_US;
    } else {
        return;
    }

    pwm_set_gpio_level(SERVO_PIN, current_pulse_width_us);
}

