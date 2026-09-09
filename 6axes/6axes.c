#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

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
    const uint8_t buffer[] = {reg, value};
    return i2c_write_timeout_us(I2C_PORT, ICM42688_ADDR, buffer, sizeof(buffer),
                                false, I2C_TIMEOUT_US) == (int)sizeof(buffer);
}

static bool icm42688_read(uint8_t reg, uint8_t *buffer, size_t length) {
    if (i2c_write_timeout_us(I2C_PORT, ICM42688_ADDR, &reg, 1, true,
                             I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(I2C_PORT, ICM42688_ADDR, buffer, length, false,
                               I2C_TIMEOUT_US) == (int)length;
}

static bool icm42688_init(void) {
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

// Returns the Z-axis angular velocity in dps. NAN indicates an I2C read error.
float icm42688_gyro_z_dps(void) {
    uint8_t raw[2];
    if (!icm42688_read(ICM42688_REG_GYRO_DATA_Z1, raw, sizeof(raw))) return NAN;

    const int16_t gyro_z = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
    return gyro_z / 16.4f; // ±2000 dps sensitivity
}

int main(void) {
    // 1. Setup
    stdio_init_all();
    i2c_init(I2C_PORT, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    sleep_ms(100);
    if (!icm42688_init()) {
        while (true) {
            printf("ICM-42688 initialization failed\n");
            sleep_ms(1000);
        }
    }

    // 2. Print only the Z-axis angular velocity (dps) continuously.
    while (true) printf("%.2f\n", icm42688_gyro_z_dps()), sleep_ms(100);
}
