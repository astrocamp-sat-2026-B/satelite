/*
 * ICM-42688-P I2C example for Raspberry Pi Pico / Pico SDK.
 *
 * Wiring (default settings):
 *   Pico GP4 (I2C0 SDA) -> ICM SDA
 *   Pico GP5 (I2C0 SCL) -> ICM SCL
 *   Pico 3V3             -> ICM VDD/VDDIO
 *   Pico GND             -> ICM GND
 *
 * AD0 low:  7-bit I2C address 0x68 (default below)
 * AD0 high: 7-bit I2C address 0x69
 *
 * Add this source file to target_sources() and link pico_stdlib and
 * hardware_i2c in CMake.  Enable USB stdio if output to a serial terminal
 * is required.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define ICM42688_I2C          i2c0
#define ICM42688_SDA_PIN      4
#define ICM42688_SCL_PIN      5
#define ICM42688_I2C_BAUDRATE 400000u
#define ICM42688_ADDR         0x68u

/* Bank 0 registers used here. */
#define ICM42688_REG_DEVICE_CONFIG 0x11u
#define ICM42688_REG_WHO_AM_I      0x75u
#define ICM42688_REG_PWR_MGMT0     0x4Eu
#define ICM42688_REG_GYRO_CONFIG0  0x4Fu
#define ICM42688_REG_ACCEL_CONFIG0 0x50u
#define ICM42688_REG_TEMP_DATA1    0x1Du

#define ICM42688_WHO_AM_I_VALUE    0x47u

/* Configuration selected by icm42688_init(): 16 g, 2000 dps, 1 kHz. */
#define ICM42688_ACCEL_CONFIG_16G_1KHZ 0x06u
#define ICM42688_GYRO_CONFIG_2000DPS_1KHZ 0x06u
#define ICM42688_PWR_MGMT0_ACCEL_GYRO_LN 0x0Fu

typedef struct {
    float temperature_c;
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;
} icm42688_data_t;

static bool icm42688_write_reg(uint8_t reg, uint8_t value) {
    uint8_t buffer[2] = {reg, value};
    return i2c_write_blocking(ICM42688_I2C, ICM42688_ADDR, buffer,
                              sizeof(buffer), false) == (int)sizeof(buffer);
}

static bool icm42688_read_regs(uint8_t reg, uint8_t *buffer, size_t length) {
    if (i2c_write_blocking(ICM42688_I2C, ICM42688_ADDR, &reg, 1, true) != 1) {
        return false;
    }
    return i2c_read_blocking(ICM42688_I2C, ICM42688_ADDR, buffer,
                             length, false) == (int)length;
}

static int16_t be16_to_i16(const uint8_t *bytes) {
    return (int16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

/* Returns false if the device is absent or does not identify as ICM-42688-P. */
bool icm42688_init(void) {
    uint8_t who_am_i;

    i2c_init(ICM42688_I2C, ICM42688_I2C_BAUDRATE);
    gpio_set_function(ICM42688_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(ICM42688_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(ICM42688_SDA_PIN);
    gpio_pull_up(ICM42688_SCL_PIN);

    /* Soft reset, then give the internal oscillator time to restart. */
    if (!icm42688_write_reg(ICM42688_REG_DEVICE_CONFIG, 0x01u)) {
        return false;
    }
    sleep_ms(2);

    if (!icm42688_read_regs(ICM42688_REG_WHO_AM_I, &who_am_i, 1) ||
        who_am_i != ICM42688_WHO_AM_I_VALUE) {
        return false;
    }

    if (!icm42688_write_reg(ICM42688_REG_GYRO_CONFIG0,
                            ICM42688_GYRO_CONFIG_2000DPS_1KHZ) ||
        !icm42688_write_reg(ICM42688_REG_ACCEL_CONFIG0,
                            ICM42688_ACCEL_CONFIG_16G_1KHZ) ||
        !icm42688_write_reg(ICM42688_REG_PWR_MGMT0,
                            ICM42688_PWR_MGMT0_ACCEL_GYRO_LN)) {
        return false;
    }

    /* Allow the accelerometer and gyro to enter low-noise mode. */
    sleep_ms(50);
    return true;
}

/* Reads temperature, acceleration and angular velocity in one I2C transfer. */
bool icm42688_read(icm42688_data_t *data) {
    uint8_t raw[14];

    if (data == NULL || !icm42688_read_regs(ICM42688_REG_TEMP_DATA1,
                                             raw, sizeof(raw))) {
        return false;
    }

    const int16_t raw_temp = be16_to_i16(&raw[0]);
    const int16_t raw_ax = be16_to_i16(&raw[2]);
    const int16_t raw_ay = be16_to_i16(&raw[4]);
    const int16_t raw_az = be16_to_i16(&raw[6]);
    const int16_t raw_gx = be16_to_i16(&raw[8]);
    const int16_t raw_gy = be16_to_i16(&raw[10]);
    const int16_t raw_gz = be16_to_i16(&raw[12]);

    /* Datasheet conversions for the ranges configured above. */
    data->temperature_c = (float)raw_temp / 132.48f + 25.0f;
    data->accel_x_g = (float)raw_ax * (16.0f / 32768.0f);
    data->accel_y_g = (float)raw_ay * (16.0f / 32768.0f);
    data->accel_z_g = (float)raw_az * (16.0f / 32768.0f);
    data->gyro_x_dps = (float)raw_gx * (2000.0f / 32768.0f);
    data->gyro_y_dps = (float)raw_gy * (2000.0f / 32768.0f);
    data->gyro_z_dps = (float)raw_gz * (2000.0f / 32768.0f);
    return true;
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);  /* Gives USB serial a moment to enumerate. */

    if (!icm42688_init()) {
        printf("ICM-42688-P was not found (check I2C wiring/address).\\n");
        while (true) {
            sleep_ms(1000);
        }
    }

    while (true) {
        icm42688_data_t sensor;
        if (icm42688_read(&sensor)) {
            printf("T=%6.2f C  A[g]=%7.3f %7.3f %7.3f  G[dps]=%8.2f %8.2f %8.2f\\n",
                   sensor.temperature_c,
                   sensor.accel_x_g, sensor.accel_y_g, sensor.accel_z_g,
                   sensor.gyro_x_dps, sensor.gyro_y_dps, sensor.gyro_z_dps);
        } else {
            printf("ICM-42688-P I2C read error\\n");
        }
        sleep_ms(100);
    }
}
