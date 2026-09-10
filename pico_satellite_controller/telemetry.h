#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

/* Add future sensor fields here without changing the TCP implementation. */
typedef struct {
    uint32_t uptime_s;
    int temperature_centi_c;
    uint32_t random_value;
    int32_t command_value;
    int32_t gyro_z_centi_dps;
    bool gyro_z_valid;
    int32_t gyro_z_angle_centi_deg;
    bool gyro_z_angle_valid;
    uint16_t photodiode_adc[4];
    uint16_t photoreflector_adc;
    bool photoreflector_valid;
} telemetry_data_t;

void telemetry_init(void);
void telemetry_collect(telemetry_data_t *telemetry, int32_t command_value);

#endif
