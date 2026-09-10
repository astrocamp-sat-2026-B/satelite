#include "servo.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"

#define SERVO_PIN 11
#define SERVO_PERIOD_US 20000
#define SERVO_STOP_PULSE_US 1500
#define SERVO_MIN_PULSE_US 1000
#define SERVO_MAX_PULSE_US 2000

void servo_init(void) {
    uint slice = pwm_gpio_to_slice_num(SERVO_PIN);

    gpio_set_function(SERVO_PIN, GPIO_FUNC_PWM);
    pwm_set_clkdiv(slice, 125.0f);
    pwm_set_wrap(slice, SERVO_PERIOD_US);
    pwm_set_gpio_level(SERVO_PIN, SERVO_STOP_PULSE_US);
    pwm_set_enabled(slice, true);
}

void servo_set_speed(int32_t speed_percent) {
    if (speed_percent > 100) speed_percent = 100;
    if (speed_percent < -100) speed_percent = -100;

    uint pulse_width_us = SERVO_STOP_PULSE_US + (speed_percent * 5);
    pwm_set_gpio_level(SERVO_PIN, pulse_width_us);
}
