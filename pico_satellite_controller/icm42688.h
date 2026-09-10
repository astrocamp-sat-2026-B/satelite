#ifndef ICM42688_H
#define ICM42688_H

#include <stdbool.h>
#include <stdint.h>

/* Initializes I2C0 and the ICM-42688 gyroscope. */
bool icm42688_init(void);

/* Reads Z-axis angular velocity in centi-dps. Returns false on I2C failure. */
bool icm42688_read_gyro_z_centi_dps(int32_t *gyro_z_centi_dps);

#endif
