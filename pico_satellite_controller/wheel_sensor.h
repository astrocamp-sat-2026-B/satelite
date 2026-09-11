#ifndef WHEEL_SENSOR_H
#define WHEEL_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t minimum;
    uint16_t maximum;
    uint16_t raw;
    bool level_high;
    bool level_initialized;
    bool valid;
    uint64_t last_edge_us;
    uint64_t period_us;
    float rpm;
    int8_t direction;
} wheel_sensor_t;

void wheel_sensor_init(wheel_sensor_t *sensor);
void wheel_sensor_set_direction(wheel_sensor_t *sensor, int32_t command_percent);
void wheel_sensor_process(wheel_sensor_t *sensor, uint16_t raw, uint64_t now_us);
float wheel_sensor_rpm(const wheel_sensor_t *sensor);
float wheel_sensor_signed_rpm(const wheel_sensor_t *sensor);

#endif
