#ifndef SERVO_H
#define SERVO_H

#include <stdint.h>

/* Initializes the continuous-rotation servo PWM output on GPIO11. */
void servo_init(void);

/*
 * Sets rotation speed in percent: -100 is full reverse, 0 stops, and
 * +100 is full forward. Values outside this range are clamped.
 */
void servo_set_speed(int32_t speed_percent);

#endif
