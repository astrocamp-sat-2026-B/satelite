#ifndef SERVO_H
#define SERVO_H

#include <stdint.h>
#include <stdbool.h>

/* Initializes the continuous-rotation servo PWM output on GPIO11. */
void servo_init(void);

/*
 * Calibrates an individual FS90R. neutral_pulse_us is the measured pulse
 * that stops the wheel. deadband_us is the full no-motion pulse-width band.
 * Call only while attitude control is idle and the frame is restrained.
 */
bool servo_configure(uint32_t neutral_pulse_us, uint32_t deadband_us);
uint32_t servo_get_neutral_pulse_us(void);
uint32_t servo_get_deadband_us(void);

/*
 * Sets the target rotation speed in percent: -100 is full reverse, 0 stops,
 * and +100 is full forward. Values outside this range are clamped. The PWM
 * output approaches this target gradually when servo_update() is called.
 */
void servo_set_speed(int32_t speed_percent);

/* Advances the PWM output toward the target speed. Call this from the loop. */
void servo_update(void);

#endif
