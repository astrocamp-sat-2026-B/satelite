#include "icm42688.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/i2c.h"
#include "pico/critical_section.h"
#include "pico/stdlib.h"

#define I2C_PORT i2c0
#define I2C_SDA_PIN 20
#define I2C_SCL_PIN 21
#define I2C_BAUDRATE 400000
#define I2C_TIMEOUT_US 10000

#define ICM42688_ADDR 0x69
#define REG_DEVICE_CONFIG 0x11
#define REG_TEMP_DATA1 0x1D
#define REG_PWR_MGMT0 0x4E
#define REG_GYRO_CONFIG0 0x4F
#define REG_ACCEL_CONFIG0 0x50
#define REG_GYRO_CONFIG1 0x51
#define REG_GYRO_ACCEL_CONFIG0 0x52
#define REG_ACCEL_CONFIG1 0x53
#define REG_WHO_AM_I 0x75
#define WHO_AM_I_VALUE 0x47

/* 200 Hz, gyro +/-250 dps (131 LSB/dps), accel +/-2 g. */
#define GYRO_CONFIG0_VALUE 0x67
#define ACCEL_CONFIG0_VALUE 0x67
#define GYRO_SENSITIVITY_LSB_PER_DPS 131.0f
#define ACCEL_SENSITIVITY_LSB_PER_G 16384.0f

#define CALIBRATION_SAMPLES 400u
#define CALIBRATION_GYRO_STDDEV_MAX_DPS 0.50f
#define ACCEL_STATIONARY_TOLERANCE_G 0.08f
#define GYRO_STATIONARY_LIMIT_DPS 1.0f
#define STATIONARY_SAMPLES_FOR_BIAS 100u
#define BIAS_TRACKING_GAIN 0.001f
#define SOFTWARE_LPF_HZ 20.0f
#define MAHONY_KP 1.5f
#define MAX_VALID_DT_S 0.050f
#define DEG_TO_RAD 0.01745329251994329577f
#define RAD_TO_DEG 57.295779513082320876f

static const float gyro_scale[3] = {1.0f, 1.0f, 1.0f};
static const float gyro_temperature_slope[3] = {0.0f, 0.0f, 0.0f};

typedef struct {
    float quaternion[4];
    float gyro_bias[3];
    float calibration_temperature_c;
    float filtered_gyro[3];
    float filtered_accel[3];
    float median_history[3][3];
    uint8_t median_count;
    uint8_t median_index;
    uint32_t stationary_count;
    uint64_t last_timestamp_us;
    bool filter_initialized;
    bool calibrated;
    angle_integrator_t yaw_integrator;
    icm42688_attitude_t output;
} estimator_state_t;

static estimator_state_t estimator;
static critical_section_t estimator_lock;
static bool lock_initialized;

static int16_t decode_i16(const uint8_t *bytes) {
    return (int16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static bool write_register(uint8_t reg, uint8_t value) {
    const uint8_t buffer[] = {reg, value};
    return i2c_write_timeout_us(I2C_PORT, ICM42688_ADDR, buffer,
                                sizeof(buffer), false, I2C_TIMEOUT_US) ==
           (int)sizeof(buffer);
}

static bool read_registers(uint8_t reg, uint8_t *buffer, size_t length) {
    if (i2c_write_timeout_us(I2C_PORT, ICM42688_ADDR, &reg, 1, true,
                             I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(I2C_PORT, ICM42688_ADDR, buffer, length, false,
                               I2C_TIMEOUT_US) == (int)length;
}

bool icm42688_read_sample(icm42688_sample_t *sample) {
    uint8_t raw[14];
    if (sample == NULL || !read_registers(REG_TEMP_DATA1, raw, sizeof(raw))) {
        return false;
    }
    sample->timestamp_us = time_us_64();
    sample->temperature_c = (float)decode_i16(&raw[0]) / 132.48f + 25.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        sample->acceleration_g[axis] =
            (float)decode_i16(&raw[2 + 2 * axis]) /
            ACCEL_SENSITIVITY_LSB_PER_G;
        sample->angular_rate_dps[axis] =
            (float)decode_i16(&raw[8 + 2 * axis]) /
            GYRO_SENSITIVITY_LSB_PER_DPS;
    }
    return true;
}

static float median3(float a, float b, float c) {
    if (a > b) { const float t = a; a = b; b = t; }
    if (b > c) { const float t = b; b = c; c = t; }
    if (a > b) { const float t = a; a = b; b = t; }
    return b;
}

static void quaternion_update(float gyro_dps[3], const float accel_g[3],
                              float dt_s) {
    float q0 = estimator.quaternion[0];
    float q1 = estimator.quaternion[1];
    float q2 = estimator.quaternion[2];
    float q3 = estimator.quaternion[3];
    const float magnitude = sqrtf(accel_g[0] * accel_g[0] +
                                  accel_g[1] * accel_g[1] +
                                  accel_g[2] * accel_g[2]);
    if (magnitude > 1.0f - ACCEL_STATIONARY_TOLERANCE_G &&
        magnitude < 1.0f + ACCEL_STATIONARY_TOLERANCE_G) {
        const float ax = accel_g[0] / magnitude;
        const float ay = accel_g[1] / magnitude;
        const float az = accel_g[2] / magnitude;
        const float vx = 2.0f * (q1 * q3 - q0 * q2);
        const float vy = 2.0f * (q0 * q1 + q2 * q3);
        const float vz = q0*q0 - q1*q1 - q2*q2 + q3*q3;
        gyro_dps[0] += MAHONY_KP * (ay * vz - az * vy) * RAD_TO_DEG;
        gyro_dps[1] += MAHONY_KP * (az * vx - ax * vz) * RAD_TO_DEG;
        gyro_dps[2] += MAHONY_KP * (ax * vy - ay * vx) * RAD_TO_DEG;
    }

    const float gx = gyro_dps[0] * DEG_TO_RAD;
    const float gy = gyro_dps[1] * DEG_TO_RAD;
    const float gz = gyro_dps[2] * DEG_TO_RAD;
    const float half_dt = 0.5f * dt_s;
    const float dq0 = (-q1 * gx - q2 * gy - q3 * gz) * half_dt;
    const float dq1 = ( q0 * gx + q2 * gz - q3 * gy) * half_dt;
    const float dq2 = ( q0 * gy - q1 * gz + q3 * gx) * half_dt;
    const float dq3 = ( q0 * gz + q1 * gy - q2 * gx) * half_dt;
    q0 += dq0;
    q1 += dq1;
    q2 += dq2;
    q3 += dq3;
    const float inverse_norm = 1.0f / sqrtf(q0*q0 + q1*q1 + q2*q2 + q3*q3);
    estimator.quaternion[0] = q0 * inverse_norm;
    estimator.quaternion[1] = q1 * inverse_norm;
    estimator.quaternion[2] = q2 * inverse_norm;
    estimator.quaternion[3] = q3 * inverse_norm;
}

static bool calibrate_stationary(void) {
    double mean[3] = {0.0, 0.0, 0.0};
    double m2[3] = {0.0, 0.0, 0.0};
    double accel_magnitude_sum = 0.0;
    double temperature_sum = 0.0;
    uint32_t count = 0;
    absolute_time_t next = get_absolute_time();
    for (uint32_t i = 0; i < CALIBRATION_SAMPLES; ++i) {
        next = delayed_by_us(next, ICM42688_SAMPLE_PERIOD_US);
        sleep_until(next);
        icm42688_sample_t sample;
        if (!icm42688_read_sample(&sample)) continue;
        ++count;
        for (size_t axis = 0; axis < 3; ++axis) {
            const double delta = sample.angular_rate_dps[axis] - mean[axis];
            mean[axis] += delta / (double)count;
            m2[axis] += delta * (sample.angular_rate_dps[axis] - mean[axis]);
        }
        const double ax = sample.acceleration_g[0];
        const double ay = sample.acceleration_g[1];
        const double az = sample.acceleration_g[2];
        accel_magnitude_sum += sqrt(ax*ax + ay*ay + az*az);
        temperature_sum += sample.temperature_c;
    }
    if (count < CALIBRATION_SAMPLES * 9u / 10u) return false;

    const double accel_mean = accel_magnitude_sum / (double)count;
    bool stationary = fabs(accel_mean - 1.0) < ACCEL_STATIONARY_TOLERANCE_G;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (sqrt(m2[axis] / (double)(count - 1)) >
            CALIBRATION_GYRO_STDDEV_MAX_DPS) stationary = false;
    }
    if (!stationary) return false;
    for (size_t axis = 0; axis < 3; ++axis) {
        estimator.gyro_bias[axis] = (float)mean[axis];
    }
    estimator.calibration_temperature_c = (float)(temperature_sum / count);
    return true;
}

bool icm42688_init(void) {
    memset(&estimator, 0, sizeof(estimator));
    estimator.quaternion[0] = 1.0f;
    angle_integrator_init(&estimator.yaw_integrator,
                          ANGLE_INTEGRATION_SIMPSON);
    if (!lock_initialized) {
        critical_section_init(&estimator_lock);
        lock_initialized = true;
    }
    i2c_init(I2C_PORT, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    sleep_ms(100);

    uint8_t who_am_i;
    if (!read_registers(REG_WHO_AM_I, &who_am_i, 1) ||
        who_am_i != WHO_AM_I_VALUE) return false;
    if (!write_register(REG_DEVICE_CONFIG, 0x01)) return false;
    sleep_ms(2);
    if (!write_register(REG_PWR_MGMT0, 0x0F)) return false;
    sleep_ms(1);
    if (!write_register(REG_GYRO_CONFIG0, GYRO_CONFIG0_VALUE) ||
        !write_register(REG_ACCEL_CONFIG0, ACCEL_CONFIG0_VALUE)) return false;
    /* Third-order UI filters, about 24 Hz bandwidth at 200 Hz ODR. */
    if (!write_register(REG_GYRO_CONFIG1, 0x0A) ||
        !write_register(REG_ACCEL_CONFIG1, 0x14) ||
        !write_register(REG_GYRO_ACCEL_CONFIG0, 0x55)) return false;
    sleep_ms(50);
    estimator.calibrated = calibrate_stationary();
    estimator.output.calibrated = estimator.calibrated;
    return true;
}

bool icm42688_update(void) {
    icm42688_sample_t sample;
    if (!icm42688_read_sample(&sample)) {
        critical_section_enter_blocking(&estimator_lock);
        ++estimator.output.rejected_samples;
        critical_section_exit(&estimator_lock);
        return false;
    }

    critical_section_enter_blocking(&estimator_lock);
    float dt_s = (float)ICM42688_SAMPLE_PERIOD_US * 1.0e-6f;
    if (estimator.last_timestamp_us != 0) {
        dt_s = (float)(sample.timestamp_us - estimator.last_timestamp_us) * 1.0e-6f;
    }
    estimator.last_timestamp_us = sample.timestamp_us;
    if (!(dt_s > 0.0f) || dt_s > MAX_VALID_DT_S) {
        ++estimator.output.rejected_samples;
        critical_section_exit(&estimator_lock);
        return false;
    }

    for (size_t axis = 0; axis < 3; ++axis) {
        estimator.median_history[axis][estimator.median_index] =
            sample.angular_rate_dps[axis] * gyro_scale[axis];
    }
    if (estimator.median_count < 3) ++estimator.median_count;
    estimator.median_index = (uint8_t)((estimator.median_index + 1u) % 3u);

    const float alpha = 1.0f - expf(-2.0f * 3.14159265358979323846f *
                                    SOFTWARE_LPF_HZ * dt_s);
    for (size_t axis = 0; axis < 3; ++axis) {
        float gyro = sample.angular_rate_dps[axis] * gyro_scale[axis];
        if (estimator.median_count == 3) {
            gyro = median3(estimator.median_history[axis][0],
                           estimator.median_history[axis][1],
                           estimator.median_history[axis][2]);
        }
        if (!estimator.filter_initialized) {
            estimator.filtered_gyro[axis] = gyro;
            estimator.filtered_accel[axis] = sample.acceleration_g[axis];
        } else {
            estimator.filtered_gyro[axis] +=
                alpha * (gyro - estimator.filtered_gyro[axis]);
            estimator.filtered_accel[axis] +=
                alpha * (sample.acceleration_g[axis] -
                         estimator.filtered_accel[axis]);
        }
    }
    estimator.filter_initialized = true;

    const float ax = estimator.filtered_accel[0];
    const float ay = estimator.filtered_accel[1];
    const float az = estimator.filtered_accel[2];
    const float accel_magnitude = sqrtf(ax*ax + ay*ay + az*az);
    bool stationary = fabsf(accel_magnitude - 1.0f) <
                      ACCEL_STATIONARY_TOLERANCE_G;
    float corrected_gyro[3];
    for (size_t axis = 0; axis < 3; ++axis) {
        const float temperature_bias = gyro_temperature_slope[axis] *
            (sample.temperature_c - estimator.calibration_temperature_c);
        corrected_gyro[axis] = estimator.filtered_gyro[axis] -
                               estimator.gyro_bias[axis] - temperature_bias;
        if (fabsf(corrected_gyro[axis]) > GYRO_STATIONARY_LIMIT_DPS) {
            stationary = false;
        }
    }

    if (stationary) {
        ++estimator.stationary_count;
        if (estimator.stationary_count >= STATIONARY_SAMPLES_FOR_BIAS) {
            for (size_t axis = 0; axis < 3; ++axis) {
                estimator.gyro_bias[axis] += BIAS_TRACKING_GAIN *
                    (estimator.filtered_gyro[axis] - estimator.gyro_bias[axis]);
                corrected_gyro[axis] = 0.0f;
            }
            estimator.calibrated = true;
        }
    } else {
        estimator.stationary_count = 0;
    }

    quaternion_update(corrected_gyro, estimator.filtered_accel, dt_s);
    const double yaw = angle_integrator_update(&estimator.yaw_integrator,
                                                sample.timestamp_us,
                                                corrected_gyro[2]);
    const float q0 = estimator.quaternion[0];
    const float q1 = estimator.quaternion[1];
    const float q2 = estimator.quaternion[2];
    const float q3 = estimator.quaternion[3];
    const float sin_pitch = 2.0f * (q0 * q2 - q3 * q1);
    estimator.output.roll_deg = atan2f(2.0f * (q0*q1 + q2*q3),
        1.0f - 2.0f * (q1*q1 + q2*q2)) * RAD_TO_DEG;
    estimator.output.pitch_deg = asinf(fmaxf(-1.0f, fminf(1.0f, sin_pitch))) *
                                 RAD_TO_DEG;
    estimator.output.yaw_deg = (float)yaw;
    for (size_t axis = 0; axis < 3; ++axis) {
        estimator.output.gyro_dps[axis] = corrected_gyro[axis];
        estimator.output.gyro_bias_dps[axis] = estimator.gyro_bias[axis];
    }
    estimator.output.calibrated = estimator.calibrated;
    estimator.output.valid = true;
    ++estimator.output.sample_count;
    critical_section_exit(&estimator_lock);
    return true;
}

bool icm42688_get_attitude(icm42688_attitude_t *attitude) {
    if (attitude == NULL || !lock_initialized) return false;
    critical_section_enter_blocking(&estimator_lock);
    *attitude = estimator.output;
    critical_section_exit(&estimator_lock);
    return attitude->valid;
}

void icm42688_set_integration_method(angle_integration_method_t method) {
    if (method > ANGLE_INTEGRATION_GAUSS) return;
    critical_section_enter_blocking(&estimator_lock);
    angle_integrator_init(&estimator.yaw_integrator, method);
    estimator.output.yaw_deg = 0.0f;
    critical_section_exit(&estimator_lock);
}

bool icm42688_read_gyro_z_centi_dps(int32_t *gyro_z_centi_dps) {
    icm42688_attitude_t attitude;
    if (gyro_z_centi_dps == NULL || !icm42688_get_attitude(&attitude)) {
        return false;
    }
    *gyro_z_centi_dps = (int32_t)lroundf(attitude.gyro_dps[2] * 100.0f);
    return true;
}

float icm42688_gyro_z_dps(void) {
    icm42688_attitude_t attitude;
    return icm42688_get_attitude(&attitude) ? attitude.gyro_dps[2] : NAN;
}

float icm42688_gyro_z_angle_deg(void) {
    icm42688_attitude_t attitude;
    return icm42688_get_attitude(&attitude) ? attitude.yaw_deg : NAN;
}

void icm42688_gyro_z_angle_reset(void) {
    critical_section_enter_blocking(&estimator_lock);
    angle_integrator_reset(&estimator.yaw_integrator);
    estimator.output.yaw_deg = 0.0f;
    critical_section_exit(&estimator_lock);
}
