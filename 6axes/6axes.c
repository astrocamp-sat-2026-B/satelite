#include <stdio.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

// I2C0: GP20 = SDA, GP21 = SCL. Change these to match your wiring.
#define I2C_PORT       i2c0
#define I2C_SDA_PIN    20
#define I2C_SCL_PIN    21
#define I2C_BAUDRATE   400000
#define I2C_TIMEOUT_US 10000
// AD0/GND -> 0x68, AD0/VDDIO -> 0x69.
#define ICM42688_ADDR  0x68

#define ICM42688_REG_DEVICE_CONFIG  0x11
#define ICM42688_REG_TEMP_DATA1     0x1D
#define ICM42688_REG_WHO_AM_I       0x75
#define ICM42688_REG_PWR_MGMT0      0x4E
#define ICM42688_REG_GYRO_CONFIG0   0x4F
#define ICM42688_REG_ACCEL_CONFIG0  0x50
#define ICM42688_WHO_AM_I_VALUE     0x47

typedef struct {
    float temperature_c;
    float accel_g[3];
    float gyro_dps[3];
} icm42688_data_t;

static bool icm42688_write(uint8_t reg, uint8_t value) {
    uint8_t buffer[] = {reg, value};
    return i2c_write_timeout_us(I2C_PORT, ICM42688_ADDR, buffer, 2, false,
                                I2C_TIMEOUT_US) == 2;
}

static bool icm42688_read(uint8_t reg, uint8_t *buffer, size_t length) {
    if (i2c_write_timeout_us(I2C_PORT, ICM42688_ADDR, &reg, 1, true,
                             I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(I2C_PORT, ICM42688_ADDR, buffer, length, false,
                               I2C_TIMEOUT_US) == (int)length;
}

static int16_t to_int16(uint8_t msb, uint8_t lsb) {
    return (int16_t)(((uint16_t)msb << 8) | lsb);
}

static bool icm42688_init(void) {
    uint8_t who_am_i;
    // Soft reset, then wait for the device to restart.
    if (!icm42688_write(ICM42688_REG_DEVICE_CONFIG, 0x01)) return false;
    sleep_ms(2);
    if (!icm42688_read(ICM42688_REG_WHO_AM_I, &who_am_i, 1) || who_am_i != ICM42688_WHO_AM_I_VALUE) return false;

    // Accel and gyro: low-noise mode.
    if (!icm42688_write(ICM42688_REG_PWR_MGMT0, 0x0F)) return false;
    sleep_ms(1);
    // ±2000 dps / ±16 g, both at 1 kHz.
    if (!icm42688_write(ICM42688_REG_GYRO_CONFIG0, 0x06)) return false;
    if (!icm42688_write(ICM42688_REG_ACCEL_CONFIG0, 0x06)) return false;
    sleep_ms(50);
    return true;
}

static bool icm42688_read_data(icm42688_data_t *data) {
    // TEMP_DATA1 through GYRO_DATA_Z0: 14 consecutive bytes.
    uint8_t raw[14];
    if (!icm42688_read(ICM42688_REG_TEMP_DATA1, raw, sizeof(raw))) return false;

    const int16_t temp = to_int16(raw[0], raw[1]);
    const int16_t ax = to_int16(raw[2], raw[3]);
    const int16_t ay = to_int16(raw[4], raw[5]);
    const int16_t az = to_int16(raw[6], raw[7]);
    const int16_t gx = to_int16(raw[8], raw[9]);
    const int16_t gy = to_int16(raw[10], raw[11]);
    const int16_t gz = to_int16(raw[12], raw[13]);

    // Sensitivities for the selected ±16 g and ±2000 dps ranges.
    data->temperature_c = (temp / 132.48f) + 25.0f;
    data->accel_g[0] = ax / 2048.0f;
    data->accel_g[1] = ay / 2048.0f;
    data->accel_g[2] = az / 2048.0f;
    data->gyro_dps[0] = gx / 16.4f;
    data->gyro_dps[1] = gy / 16.4f;
    data->gyro_dps[2] = gz / 16.4f;
    return true;
}

int main(void) {
    stdio_init_all();
    i2c_init(I2C_PORT, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    sleep_ms(100);
    printf("Starting ICM-42688 I2C test on GP20/GP21...\n");
    if (!icm42688_init()) {
        // Repeat this so the message is visible even when the terminal opens late.
        while (true) {
            printf("ICM-42688 not found: check 3.3V, GND, SDA=GP20, SCL=GP21, and address.\n");
            sleep_ms(1000);
        }
    }

    printf("ICM-42688 ready\n");
    while (true) {
        icm42688_data_t imu;
        if (icm42688_read_data(&imu)) {
            printf("T=%6.2f C  A=[%7.3f %7.3f %7.3f] g  G=[%8.2f %8.2f %8.2f] dps\n",
                   imu.temperature_c, imu.accel_g[0], imu.accel_g[1], imu.accel_g[2],
                   imu.gyro_dps[0], imu.gyro_dps[1], imu.gyro_dps[2]);
        } else {
            printf("I2C read error\n");
        }
        sleep_ms(100);
    }
}
