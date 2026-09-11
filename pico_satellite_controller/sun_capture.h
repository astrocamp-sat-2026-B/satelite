#ifndef SUN_CAPTURE_H
#define SUN_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#define SUN_CAPTURE_ADC_MAX 1023u
#define SUN_CAPTURE_CHANNEL_A 2u
#define SUN_CAPTURE_CHANNEL_B 3u
#define SUN_CAPTURE_DEFAULT_THRESHOLD 700u
#define SUN_CAPTURE_DEFAULT_TOLERANCE 50u

void sun_capture_init(void);
bool sun_capture_update(const uint16_t photodiode_adc[4]);
uint16_t sun_capture_get_threshold(void);
uint16_t sun_capture_get_tolerance(void);
void sun_capture_set_threshold(uint16_t value);
void sun_capture_set_tolerance(uint16_t value);

#endif
