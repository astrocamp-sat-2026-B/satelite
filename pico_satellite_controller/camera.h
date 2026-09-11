#ifndef ASTROCAMP_CAMERA_H
#define ASTROCAMP_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAMERA_WIDTH 320u
#define CAMERA_HEIGHT 240u
#define CAMERA_BYTES_PER_PIXEL 2u
#define CAMERA_FRAME_BYTES (CAMERA_WIDTH * CAMERA_HEIGHT * CAMERA_BYTES_PER_PIXEL)

typedef enum {
    CAMERA_OK = 0,
    CAMERA_ERROR_SCCB,
    CAMERA_ERROR_SENSOR_ID,
    CAMERA_ERROR_REGISTER_WRITE,
    CAMERA_ERROR_NOT_INITIALIZED,
    CAMERA_ERROR_CAPTURE,
} camera_status_t;

camera_status_t camera_init(void);
camera_status_t camera_set_test_pattern(bool enabled);
camera_status_t camera_capture_frame(void);
const uint8_t *camera_get_frame(void);
size_t camera_get_frame_size(void);
void camera_get_sensor_id(uint8_t *pid, uint8_t *ver);
const char *camera_status_string(camera_status_t status);

#endif
