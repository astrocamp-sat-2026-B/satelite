#include "attitude_control.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static float clampf(float value, float minimum, float maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static float wrap_error_deg(float error_deg) {
    const float unwrapped_error_deg = error_deg;
    error_deg = fmodf(error_deg + 180.0f, 360.0f);
    if (error_deg < 0.0f) error_deg += 360.0f;
    error_deg -= 180.0f;
    if (error_deg == -180.0f && unwrapped_error_deg > 0.0f) {
        return 180.0f;
    }
    return error_deg;
}

static int32_t round_command(float command_percent) {
    if (command_percent >= 0.0f) {
        return (int32_t)(command_percent + 0.5f);
    }
    return (int32_t)(command_percent - 0.5f);
}

static void set_fault(attitude_control_t *control,
                      attitude_control_fault_t fault) {
    control->status.mode = ATTITUDE_CONTROL_FAULT;
    control->status.fault = fault;
    control->status.wheel_command_percent = 0.0f;
    control->status.servo_command_percent = 0;
}

void attitude_control_default_config(attitude_control_config_t *config) {
    if (config == NULL) return;
    config->angle_gain_per_s = 0.70f;
    config->angle_integral_gain_per_s2 = 0.10f;
    config->max_integral_rate_dps = 2.0f;
    config->integral_zone_deg = 12.0f;
    config->rate_gain_per_s = 1.50f;
    config->wheel_command_gain = 3.00f;
    config->max_body_rate_dps = 5.0f;
    config->max_wheel_command_percent = 70.0f;
    config->settle_angle_deg = 3.0f;
    config->settle_rate_dps = 0.8f;
    config->settle_time_ms = 1000u;
    config->slew_timeout_ms = 90000u;
    config->saturation_timeout_ms = 1500u;
}

void attitude_control_init(attitude_control_t *control,
                           const attitude_control_config_t *config) {
    if (control == NULL) return;
    memset(control, 0, sizeof(*control));
    if (config != NULL) {
        control->config = *config;
    } else {
        attitude_control_default_config(&control->config);
    }
    control->status.mode = ATTITUDE_CONTROL_IDLE;
}

static bool start_control(attitude_control_t *control, float target_yaw_deg,
                          attitude_control_mode_t mode) {
    if (control == NULL || !isfinite(target_yaw_deg)) return false;
    control->status.mode = mode;
    control->status.fault = ATTITUDE_CONTROL_FAULT_NONE;
    control->status.target_yaw_deg = target_yaw_deg;
    control->status.angle_error_deg = 0.0f;
    control->status.target_rate_dps = 0.0f;
    control->status.integral_rate_dps = 0.0f;
    control->status.body_rate_dps = 0.0f;
    control->status.wheel_command_percent = 0.0f;
    control->status.servo_command_percent = 0;
    control->status.elapsed_ms = 0u;
    control->status.settled_ms = 0u;
    control->status.saturated_ms = 0u;
    return true;
}

bool attitude_control_start(attitude_control_t *control, float target_yaw_deg) {
    return start_control(control, target_yaw_deg, ATTITUDE_CONTROL_SLEW);
}

bool attitude_control_hold(attitude_control_t *control, float target_yaw_deg) {
    return start_control(control, target_yaw_deg, ATTITUDE_CONTROL_HOLD);
}

void attitude_control_abort(attitude_control_t *control) {
    if (control == NULL) return;
    control->status.mode = ATTITUDE_CONTROL_ABORTED;
    control->status.fault = ATTITUDE_CONTROL_FAULT_NONE;
    control->status.wheel_command_percent = 0.0f;
    control->status.servo_command_percent = 0;
}

void attitude_control_update(attitude_control_t *control,
                             bool attitude_valid,
                             float yaw_deg,
                             float body_rate_dps,
                             uint32_t dt_ms) {
    if (control == NULL || !attitude_control_is_active(control)) return;
    if (!attitude_valid || !isfinite(yaw_deg) || !isfinite(body_rate_dps)) {
        set_fault(control, ATTITUDE_CONTROL_FAULT_IMU);
        return;
    }
    if (dt_ms == 0u) return;

    attitude_control_status_t *status = &control->status;
    const attitude_control_config_t *config = &control->config;
    const uint32_t elapsed_increment_ms = dt_ms;
    const bool missed_control_deadline = dt_ms > 100u;
    if (dt_ms > 100u) dt_ms = 100u;
    const float dt_s = (float)dt_ms * 0.001f;

    status->elapsed_ms += elapsed_increment_ms;
    status->body_rate_dps = body_rate_dps;
    status->angle_error_deg = wrap_error_deg(status->target_yaw_deg - yaw_deg);

    /*
     * A constant restoring torque (suspension twist, bearing side-load, or
     * contact) leaves a steady angle error with proportional control alone.
     * Learn the small rate bias needed to keep accelerating the reaction
     * wheel against that torque. Only learn close to the target and while
     * momentum margin remains, otherwise a long slew or saturation would
     * wind the integrator up.
     */
    const bool inside_integral_zone =
        fabsf(status->angle_error_deg) <= config->integral_zone_deg;
    const bool wheel_has_margin =
        fabsf(status->wheel_command_percent) <
            0.90f * config->max_wheel_command_percent;
    if (inside_integral_zone && wheel_has_margin &&
        !missed_control_deadline) {
        status->integral_rate_dps = clampf(
            status->integral_rate_dps +
                config->angle_integral_gain_per_s2 *
                status->angle_error_deg * dt_s,
            -config->max_integral_rate_dps,
            config->max_integral_rate_dps);
    }
    status->target_rate_dps = clampf(
        config->angle_gain_per_s * status->angle_error_deg +
            status->integral_rate_dps,
        -config->max_body_rate_dps,
        config->max_body_rate_dps);

    /*
     * The rate loop requests body angular acceleration.  A reaction wheel
     * must accelerate in the opposite direction, so integrate the negative
     * request into the wheel-speed command accepted by the FS90R.
     */
    const float body_accel_request = config->rate_gain_per_s *
        (status->target_rate_dps - body_rate_dps);
    const float wheel_delta = -config->wheel_command_gain *
        body_accel_request * dt_s;
    const float requested_wheel_command =
        status->wheel_command_percent + wheel_delta;
    status->wheel_command_percent = clampf(
        requested_wheel_command,
        -config->max_wheel_command_percent,
        config->max_wheel_command_percent);

    const bool pushing_positive_limit =
        requested_wheel_command > config->max_wheel_command_percent &&
        wheel_delta > 0.0f;
    const bool pushing_negative_limit =
        requested_wheel_command < -config->max_wheel_command_percent &&
        wheel_delta < 0.0f;
    if (pushing_positive_limit || pushing_negative_limit) {
        status->saturated_ms += dt_ms;
    } else {
        status->saturated_ms = 0u;
    }
    if (status->saturated_ms >= config->saturation_timeout_ms) {
        set_fault(control, ATTITUDE_CONTROL_FAULT_SATURATION);
        return;
    }

    status->servo_command_percent = round_command(
        status->wheel_command_percent);

    const bool inside_settle_window =
        fabsf(status->angle_error_deg) <= config->settle_angle_deg &&
        fabsf(body_rate_dps) <= config->settle_rate_dps;
    if (inside_settle_window && !missed_control_deadline) {
        status->settled_ms += dt_ms;
    } else {
        status->settled_ms = 0u;
    }

    if (status->mode == ATTITUDE_CONTROL_SLEW &&
        status->settled_ms >= config->settle_time_ms) {
        status->mode = ATTITUDE_CONTROL_HOLD;
    }

    if (status->mode == ATTITUDE_CONTROL_SLEW &&
        status->elapsed_ms >= config->slew_timeout_ms) {
        set_fault(control, ATTITUDE_CONTROL_FAULT_TIMEOUT);
    }
}

bool attitude_control_is_active(const attitude_control_t *control) {
    if (control == NULL) return false;
    return control->status.mode == ATTITUDE_CONTROL_SLEW ||
           control->status.mode == ATTITUDE_CONTROL_HOLD;
}

const attitude_control_status_t *attitude_control_get_status(
    const attitude_control_t *control) {
    return control == NULL ? NULL : &control->status;
}

const char *attitude_control_mode_name(attitude_control_mode_t mode) {
    switch (mode) {
        case ATTITUDE_CONTROL_IDLE: return "IDLE";
        case ATTITUDE_CONTROL_SLEW: return "SLEW";
        case ATTITUDE_CONTROL_HOLD: return "HOLD";
        case ATTITUDE_CONTROL_ABORTED: return "ABORTED";
        case ATTITUDE_CONTROL_FAULT: return "FAULT";
        default: return "UNKNOWN";
    }
}

const char *attitude_control_fault_name(attitude_control_fault_t fault) {
    switch (fault) {
        case ATTITUDE_CONTROL_FAULT_NONE: return "NONE";
        case ATTITUDE_CONTROL_FAULT_IMU: return "IMU";
        case ATTITUDE_CONTROL_FAULT_TIMEOUT: return "TIMEOUT";
        case ATTITUDE_CONTROL_FAULT_SATURATION: return "SATURATION";
        default: return "UNKNOWN";
    }
}
