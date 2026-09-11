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
    int32_t roll_centi_deg;
    int32_t pitch_centi_deg;
    bool attitude_calibrated;
    uint32_t imu_sample_count;
    uint32_t imu_rejected_samples;
    uint16_t photodiode_adc[4];
    uint16_t photoreflector_adc;
    bool photoreflector_valid;
    int32_t wheel_rpm_centi;
    bool wheel_rpm_valid;
    uint8_t control_mode;
    uint8_t control_fault;
    int32_t control_target_centi_deg;
    int32_t control_error_centi_deg;
    int32_t control_rate_ref_centi_dps;
    int32_t control_wheel_command_centi_percent;
    uint32_t control_elapsed_ms;
    uint32_t control_settled_ms;
} telemetry_data_t;

void telemetry_init(void);
void telemetry_collect(telemetry_data_t *telemetry, int32_t command_value);

#endif
