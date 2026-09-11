#include "wheel_sensor.h"

#include <stddef.h>

#define WHEEL_SENSOR_MIN_CONTRAST 80u
#define WHEEL_SENSOR_MIN_HYSTERESIS 16u
#define WHEEL_SENSOR_MIN_PERIOD_US 100000u
#define WHEEL_SENSOR_MAX_PERIOD_US 5000000u
#define WHEEL_SENSOR_TIMEOUT_US 6000000u

void wheel_sensor_init(wheel_sensor_t *sensor) {
    if (sensor == NULL) return;
    sensor->minimum = 1023u;
    sensor->maximum = 0u;
    sensor->raw = 0u;
    sensor->level_high = false;
    sensor->level_initialized = false;
    sensor->valid = false;
    sensor->last_edge_us = 0u;
    sensor->period_us = 0u;
    sensor->rpm = 0.0f;
    sensor->direction = 0;
}

void wheel_sensor_set_direction(wheel_sensor_t *sensor, int32_t command_percent) {
    if (sensor == NULL) return;
    if (command_percent > 0) sensor->direction = 1;
    if (command_percent < 0) sensor->direction = -1;
}

void wheel_sensor_process(wheel_sensor_t *sensor, uint16_t raw, uint64_t now_us) {
    if (sensor == NULL || raw > 1023u) return;
    sensor->raw = raw;
    if (raw < sensor->minimum) sensor->minimum = raw;
    if (raw > sensor->maximum) sensor->maximum = raw;

    const uint16_t contrast = sensor->maximum - sensor->minimum;
    if (contrast < WHEEL_SENSOR_MIN_CONTRAST) {
        sensor->valid = false;
        return;
    }

    const uint16_t midpoint = (uint16_t)(sensor->minimum + contrast / 2u);
    uint16_t hysteresis = contrast / 8u;
    if (hysteresis < WHEEL_SENSOR_MIN_HYSTERESIS) {
        hysteresis = WHEEL_SENSOR_MIN_HYSTERESIS;
    }
    const uint16_t lower = midpoint > hysteresis
        ? (uint16_t)(midpoint - hysteresis) : 0u;
    const uint16_t upper = midpoint + hysteresis < 1023u
        ? midpoint + hysteresis : 1023u;

    if (!sensor->level_initialized) {
        sensor->level_high = raw >= midpoint;
        sensor->level_initialized = true;
    } else if (!sensor->level_high && raw >= upper) {
        sensor->level_high = true;
        if (sensor->last_edge_us != 0u) {
            const uint64_t period_us = now_us - sensor->last_edge_us;
            if (period_us >= WHEEL_SENSOR_MIN_PERIOD_US &&
                period_us <= WHEEL_SENSOR_MAX_PERIOD_US) {
                sensor->period_us = period_us;
                sensor->rpm = 60000000.0f / (float)period_us;
                sensor->valid = true;
            }
        }
        sensor->last_edge_us = now_us;
    } else if (sensor->level_high && raw <= lower) {
        sensor->level_high = false;
    }

    if (sensor->last_edge_us != 0u &&
        now_us - sensor->last_edge_us > WHEEL_SENSOR_TIMEOUT_US) {
        sensor->valid = false;
        sensor->rpm = 0.0f;
    }
}

float wheel_sensor_signed_rpm(const wheel_sensor_t *sensor) {
    if (sensor == NULL || !sensor->valid) return 0.0f;
    return sensor->rpm * (float)sensor->direction;
}

float wheel_sensor_rpm(const wheel_sensor_t *sensor) {
    if (sensor == NULL || !sensor->valid) return 0.0f;
    return sensor->rpm;
}
