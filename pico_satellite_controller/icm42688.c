#include <math.h>
#include <stdint.h>
#include <stddef.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include "icm42688.h"

// ===== 配線・I2C設定 =====
// I2C0: GP20 = SDA, GP21 = SCL
#define I2C_PORT       i2c0
#define I2C_SDA_PIN    20
#define I2C_SCL_PIN    21
#define I2C_BAUDRATE   400000
#define I2C_TIMEOUT_US 10000

// AD0 = 3.3 V: 0x69, AD0 = GND: 0x68
#define ICM42688_ADDR              0x69
#define ICM42688_REG_DEVICE_CONFIG 0x11
#define ICM42688_REG_GYRO_CONFIG0  0x4F
#define ICM42688_REG_PWR_MGMT0     0x4E
#define ICM42688_REG_WHO_AM_I      0x75
#define ICM42688_REG_GYRO_DATA_Z1  0x29
#define ICM42688_WHO_AM_I_VALUE    0x47

static bool icm42688_write(uint8_t reg, uint8_t value) {
    // 指定レジスタへ1バイト書き込む共通処理。
    const uint8_t buffer[] = {reg, value};
    return i2c_write_timeout_us(I2C_PORT, ICM42688_ADDR, buffer, sizeof(buffer),
                                false, I2C_TIMEOUT_US) == (int)sizeof(buffer);
}

static bool icm42688_read(uint8_t reg, uint8_t *buffer, size_t length) {
    // 指定レジスタから連続データを読む共通処理。
    if (i2c_write_timeout_us(I2C_PORT, ICM42688_ADDR, &reg, 1, true,
                             I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(I2C_PORT, ICM42688_ADDR, buffer, length, false,
                               I2C_TIMEOUT_US) == (int)length;
}

bool icm42688_init(void) {
    // ===== ICM-42688の初期化 =====
    // 接続確認後、ジャイロを低ノイズ・±2000 dps・1 kHzに設定する。
    i2c_init(I2C_PORT, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    sleep_ms(100);

    uint8_t who_am_i;
    if (!icm42688_read(ICM42688_REG_WHO_AM_I, &who_am_i, 1) ||
        who_am_i != ICM42688_WHO_AM_I_VALUE) return false;

    if (!icm42688_write(ICM42688_REG_DEVICE_CONFIG, 0x01)) return false;
    sleep_ms(2);
    if (!icm42688_write(ICM42688_REG_PWR_MGMT0, 0x0F)) return false;
    sleep_ms(1);
    // Gyroscope: ±2000 dps, 1 kHz.
    if (!icm42688_write(ICM42688_REG_GYRO_CONFIG0, 0x06)) return false;
    sleep_ms(50);
    return true;
}

bool icm42688_read_gyro_z_centi_dps(int32_t *gyro_z_centi_dps) {
    uint8_t raw[2];

    if (gyro_z_centi_dps == NULL ||
        !icm42688_read(ICM42688_REG_GYRO_DATA_Z1, raw, sizeof(raw))) {
        return false;
    }

    const int16_t gyro_z = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
    *gyro_z_centi_dps = (int32_t)(gyro_z * (100.0f / 16.4f));
    return true;
}

// ===== Z軸角速度を返す関数 =====
// Z軸ジャイロ値を dps（度/秒）で返す。
// I2C読み取りに失敗した場合は NAN を返す。
float icm42688_gyro_z_dps(void) {
    uint8_t raw[2];
    if (!icm42688_read(ICM42688_REG_GYRO_DATA_Z1, raw, sizeof(raw))) return NAN;

    // センサの上位・下位バイトを符号付き16ビット値へ戻す。
    const int16_t gyro_z = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
    return gyro_z / 16.4f; // ±2000 dps時の感度で dps へ換算
}

// ===== Z軸角度算出用の状態 =====
static float gyro_z_angle_deg = 0.0f;
static absolute_time_t gyro_z_angle_last_time;
static bool gyro_z_angle_initialized = false;

// ===== Z軸角速度を積分して角度を算出する関数 =====
// icm42688_gyro_z_dps() で得た角速度を前回呼び出しからの経過時間で積分し、
// 起動（または icm42688_gyro_z_angle_reset）からの累積角度を度で返す。
// I2C読み取りに失敗した場合は積分を行わず NAN を返す。
float icm42688_gyro_z_angle_deg(void) {
    const float gyro_z = icm42688_gyro_z_dps();
    if (isnan(gyro_z)) return NAN;

    const absolute_time_t now = get_absolute_time();
    if (!gyro_z_angle_initialized) {
        gyro_z_angle_last_time = now;
        gyro_z_angle_initialized = true;
        return gyro_z_angle_deg;
    }

    const float dt_s = (float)absolute_time_diff_us(gyro_z_angle_last_time, now) / 1000000.0f;
    gyro_z_angle_last_time = now;
    gyro_z_angle_deg += gyro_z * dt_s;

    return gyro_z_angle_deg;
}

// ===== 累積角度をリセットする関数 =====
void icm42688_gyro_z_angle_reset(void) {
    gyro_z_angle_deg = 0.0f;
    gyro_z_angle_initialized = false;
}

