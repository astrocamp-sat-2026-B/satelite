#ifndef ICM42688_H
#define ICM42688_H

#include <stdbool.h>
#include <stdint.h>

/* Initializes I2C0 and the ICM-42688 gyroscope. */
bool icm42688_init(void);

/* Reads Z-axis angular velocity in centi-dps. Returns false on I2C failure. */
bool icm42688_read_gyro_z_centi_dps(int32_t *gyro_z_centi_dps);

/* Reads Z-axis angular velocity in dps. Returns NAN on I2C failure. */
float icm42688_gyro_z_dps(void);

/* Integrates Z-axis angular velocity into a cumulative angle in degrees.
   Returns NAN on I2C failure (accumulated angle is left unchanged). */
float icm42688_gyro_z_angle_deg(void);

/* Resets the accumulated Z-axis angle to zero. */
void icm42688_gyro_z_angle_reset(void);

#endif
