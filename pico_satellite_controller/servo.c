#include "servo.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"

#define SERVO_PIN 11
#define SERVO_PERIOD_US 20000
#define SERVO_STOP_PULSE_US 1500
#define SERVO_DEFAULT_DEADBAND_US 90
#define SERVO_MIN_PULSE_US 700
#define SERVO_MAX_PULSE_US 2300

static uint neutral_pulse_width_us = SERVO_STOP_PULSE_US;
static uint deadband_width_us = SERVO_DEFAULT_DEADBAND_US;

void servo_init(void) {
    uint slice = pwm_gpio_to_slice_num(SERVO_PIN);

    gpio_set_function(SERVO_PIN, GPIO_FUNC_PWM);
    pwm_set_clkdiv(slice, 125.0f);
    pwm_set_wrap(slice, SERVO_PERIOD_US);
    pwm_set_gpio_level(SERVO_PIN, SERVO_STOP_PULSE_US);
    pwm_set_enabled(slice, true);

    neutral_pulse_width_us = SERVO_STOP_PULSE_US;
    deadband_width_us = SERVO_DEFAULT_DEADBAND_US;
}

bool servo_configure(uint32_t neutral_pulse_us, uint32_t deadband_us) {
    const uint32_t half_deadband_us = (deadband_us + 1u) / 2u;
    if (neutral_pulse_us < 1400u || neutral_pulse_us > 1600u ||
        deadband_us > 200u ||
        neutral_pulse_us <= SERVO_MIN_PULSE_US + half_deadband_us ||
        neutral_pulse_us + half_deadband_us >= SERVO_MAX_PULSE_US) {
        return false;
    }

    neutral_pulse_width_us = neutral_pulse_us;
    deadband_width_us = deadband_us;
    pwm_set_gpio_level(SERVO_PIN, neutral_pulse_width_us);
    return true;
}

uint32_t servo_get_neutral_pulse_us(void) {
    return neutral_pulse_width_us;
}

uint32_t servo_get_deadband_us(void) {
    return deadband_width_us;
}

void servo_set_speed(int32_t speed_percent) {
    if (speed_percent > 100) speed_percent = 100;
    if (speed_percent < -100) speed_percent = -100;

    uint pulse_width_us;
    if (speed_percent == 0) {
        pulse_width_us = neutral_pulse_width_us;
    } else if (speed_percent > 0) {
        pulse_width_us = neutral_pulse_width_us +
            (uint)((uint32_t)speed_percent *
                   (SERVO_MAX_PULSE_US - neutral_pulse_width_us) / 100u);
    } else {
        const uint32_t magnitude = (uint32_t)(-speed_percent);
        pulse_width_us = neutral_pulse_width_us -
            (uint)(magnitude *
                   (neutral_pulse_width_us - SERVO_MIN_PULSE_US) / 100u);
    }

    pwm_set_gpio_level(SERVO_PIN, pulse_width_us);
}
