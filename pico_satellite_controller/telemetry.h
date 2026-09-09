#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

/* Add future sensor fields here without changing the TCP implementation. */
typedef struct {
    uint32_t uptime_s;
    int temperature_centi_c;
    uint32_t random_value;
    int32_t command_value;
    int32_t gyro_z_centi_dps;
} telemetry_data_t;

void telemetry_init(void);
void telemetry_collect(telemetry_data_t *telemetry, int32_t command_value);

#endif
