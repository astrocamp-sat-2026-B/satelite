#include "servo.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

#define SERVO_PIN 11
#define SERVO_PERIOD_US 20000
#define SERVO_STOP_PULSE_US 1500
#define SERVO_DEFAULT_DEADBAND_US 90
#define SERVO_MIN_PULSE_US 700
#define SERVO_MAX_PULSE_US 2300
#define SERVO_UPDATE_INTERVAL_MS 20
#define SERVO_SLEW_STEP_US 16

static uint current_pulse_width_us = SERVO_STOP_PULSE_US;
static uint target_pulse_width_us = SERVO_STOP_PULSE_US;
static uint neutral_pulse_width_us = SERVO_STOP_PULSE_US;
static uint deadband_width_us = SERVO_DEFAULT_DEADBAND_US;
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
    neutral_pulse_width_us = SERVO_STOP_PULSE_US;
    deadband_width_us = SERVO_DEFAULT_DEADBAND_US;
    next_update = make_timeout_time_ms(SERVO_UPDATE_INTERVAL_MS);
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
    target_pulse_width_us = neutral_pulse_width_us;
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

    if (speed_percent == 0) {
        target_pulse_width_us = neutral_pulse_width_us;
        return;
    }

    /*
     * Map non-zero speed outside the calibrated deadband.  Without this,
     * small feedback commands produce no wheel acceleration and cause a
     * large stick-slip limit cycle around the target attitude.
     */
    const uint half_deadband_us = (deadband_width_us + 1u) / 2u;
    if (speed_percent > 0) {
        const uint first_motion_us = neutral_pulse_width_us + half_deadband_us;
        target_pulse_width_us = first_motion_us +
            (uint)((speed_percent - 1) *
                   (SERVO_MAX_PULSE_US - first_motion_us) / 99);
    } else {
        const uint magnitude = (uint)(-speed_percent);
        const uint first_motion_us = neutral_pulse_width_us - half_deadband_us;
        target_pulse_width_us = first_motion_us -
            (uint)((magnitude - 1u) *
                   (first_motion_us - SERVO_MIN_PULSE_US) / 99u);
    }
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