#ifndef ATTITUDE_CONTROL_H
#define ATTITUDE_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ATTITUDE_CONTROL_IDLE = 0,
    ATTITUDE_CONTROL_SLEW = 1,
    ATTITUDE_CONTROL_HOLD = 2,
    ATTITUDE_CONTROL_ABORTED = 3,
    ATTITUDE_CONTROL_FAULT = 4,
} attitude_control_mode_t;

typedef enum {
    ATTITUDE_CONTROL_FAULT_NONE = 0,
    ATTITUDE_CONTROL_FAULT_IMU = 1,
    ATTITUDE_CONTROL_FAULT_TIMEOUT = 2,
    ATTITUDE_CONTROL_FAULT_SATURATION = 3,
} attitude_control_fault_t;

typedef struct {
    float angle_gain_per_s;
    float rate_gain_per_s;
    float wheel_command_gain;
    float max_body_rate_dps;
    float max_wheel_command_percent;
    float settle_angle_deg;
    float settle_rate_dps;
    uint32_t settle_time_ms;
    uint32_t slew_timeout_ms;
    uint32_t saturation_timeout_ms;
} attitude_control_config_t;

typedef struct {
    attitude_control_mode_t mode;
    attitude_control_fault_t fault;
    float target_yaw_deg;
    float angle_error_deg;
    float target_rate_dps;
    float body_rate_dps;
    float wheel_command_percent;
    int32_t servo_command_percent;
    uint32_t elapsed_ms;
    uint32_t settled_ms;
    uint32_t saturated_ms;
    bool capture_pending;
    bool capture_issued;
} attitude_control_status_t;

typedef struct {
    attitude_control_config_t config;
    attitude_control_status_t status;
} attitude_control_t;

void attitude_control_default_config(attitude_control_config_t *config);
void attitude_control_init(attitude_control_t *control,
                           const attitude_control_config_t *config);
bool attitude_control_start(attitude_control_t *control, float target_yaw_deg);
/* Starts holding the supplied yaw without issuing a camera capture request. */
bool attitude_control_hold(attitude_control_t *control, float target_yaw_deg);
void attitude_control_abort(attitude_control_t *control);
void attitude_control_update(attitude_control_t *control,
                             bool attitude_valid,
                             float yaw_deg,
                             float body_rate_dps,
                             uint32_t dt_ms);
bool attitude_control_take_capture_request(attitude_control_t *control);
bool attitude_control_is_active(const attitude_control_t *control);
const attitude_control_status_t *attitude_control_get_status(
    const attitude_control_t *control);
const char *attitude_control_mode_name(attitude_control_mode_t mode);
const char *attitude_control_fault_name(attitude_control_fault_t fault);

#endif
