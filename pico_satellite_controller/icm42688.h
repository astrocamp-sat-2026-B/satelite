#ifndef ICM42688_H
#define ICM42688_H

#include <stdbool.h>

/* Initializes I2C0 and the ICM-42688 gyroscope. */
bool icm42688_init(void);

/* Returns Z-axis angular velocity in dps, or NAN if the I2C read fails. */
float icm42688_gyro_z_dps(void);

#endif
