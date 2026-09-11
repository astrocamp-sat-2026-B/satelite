#ifndef ICM42688_H
#define ICM42688_H

#include <stdbool.h>
#include <stdint.h>

#include "angle_integrator.h"

#define ICM42688_SAMPLE_PERIOD_US 5000u

typedef struct {
    float acceleration_g[3];
    float angular_rate_dps[3];
    float temperature_c;
    uint64_t timestamp_us;
} icm42688_sample_t;

typedef struct {
    float roll_deg;
    float pitch_deg;
    float yaw_deg;
    float gyro_dps[3];
    float gyro_bias_dps[3];
    bool calibrated;
    bool valid;
    uint32_t sample_count;
    uint32_t rejected_samples;
} icm42688_attitude_t;

bool icm42688_init(void);
bool icm42688_read_sample(icm42688_sample_t *sample);
bool icm42688_update(void);
bool icm42688_get_attitude(icm42688_attitude_t *attitude);
void icm42688_set_integration_method(angle_integration_method_t method);

bool icm42688_read_gyro_z_centi_dps(int32_t *gyro_z_centi_dps);
float icm42688_gyro_z_dps(void);
float icm42688_gyro_z_angle_deg(void);
void icm42688_gyro_z_angle_reset(void);

#endif
