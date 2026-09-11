#ifndef SERVO_H
#define SERVO_H

#include <stdint.h>

/* Initializes the continuous-rotation servo PWM output on GPIO11. */
void servo_init(void);

/*
 * Sets the target rotation speed in percent: -100 is full reverse, 0 stops,
 * and +100 is full forward. Values outside this range are clamped. The PWM
 * output approaches this target gradually when servo_update() is called.
 */
void servo_set_speed(int32_t speed_percent);

/* Advances the PWM output toward the target speed. Call this from the loop. */
void servo_update(void);

#endif
