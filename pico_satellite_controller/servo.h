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
 * Sets the rotation speed immediately: -100 is full reverse, 0 stops, and
 * +100 is full forward. Values outside this range are clamped, then mapped
 * linearly from the configured neutral pulse to the corresponding endpoint.
 */
void servo_set_speed(int32_t speed_percent);

#endif
