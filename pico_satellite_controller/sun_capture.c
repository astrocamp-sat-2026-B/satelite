#include "sun_capture.h"

#define SUN_CAPTURE_DEFAULT_THRESHOLD 700u
#define SUN_CAPTURE_DEFAULT_TOLERANCE 50u

static uint16_t threshold;
static uint16_t tolerance;
static bool previously_aligned;

static uint16_t clamp_adc(uint16_t value) {
    return value > SUN_CAPTURE_ADC_MAX ? SUN_CAPTURE_ADC_MAX : value;
}

void sun_capture_init(void) {
    threshold = SUN_CAPTURE_DEFAULT_THRESHOLD;
    tolerance = SUN_CAPTURE_DEFAULT_TOLERANCE;
    previously_aligned = false;
}

bool sun_capture_update(const uint16_t photodiode_adc[4]) {
    uint16_t a = photodiode_adc[SUN_CAPTURE_CHANNEL_A];
    uint16_t b = photodiode_adc[SUN_CAPTURE_CHANNEL_B];
    uint16_t diff = a > b ? (uint16_t)(a - b) : (uint16_t)(b - a);

    bool aligned = diff <= tolerance && a >= threshold && b >= threshold;
    bool rising_edge = aligned && !previously_aligned;
    previously_aligned = aligned;

    return rising_edge;
}

uint16_t sun_capture_get_threshold(void) { return threshold; }
uint16_t sun_capture_get_tolerance(void) { return tolerance; }

void sun_capture_set_threshold(uint16_t value) { threshold = clamp_adc(value); }
void sun_capture_set_tolerance(uint16_t value) { tolerance = clamp_adc(value); }
