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

/*
 * Initialise the fixed Astrocamp OV7675 camera wiring and sensor.
 * Safe to call again after an error. The component owns PIO0 state machine,
 * one DMA channel, I2C1, GP0..7, GP14/15, GP22, GP26..28.
 */
camera_status_t camera_init(void);

/* Enable or disable the sensor's colour-bar test pattern. */
camera_status_t camera_set_test_pattern(bool enabled);

/* Capture one QVGA RGB565 frame into the component-owned frame buffer. */
camera_status_t camera_capture_frame(void);

/* The returned buffer remains owned by the component and is overwritten next capture. */
const uint8_t *camera_get_frame(void);
size_t camera_get_frame_size(void);

/* IDs read during the latest camera_init() attempt. */
void camera_get_sensor_id(uint8_t *pid, uint8_t *ver);

const char *camera_status_string(camera_status_t status);

#endif
