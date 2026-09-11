#ifndef JPEG_ENCODER_H
#define JPEG_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The JPEG core emits at most one of these blocks for each MCU step. */
#define JPEG_ENCODER_OUTPUT_CAPACITY 2048u

typedef enum {
    JPEG_ENCODER_IN_PROGRESS,
    JPEG_ENCODER_DONE,
    JPEG_ENCODER_ERROR,
} jpeg_encoder_status_t;

/* JPEG is emitted in small blocks which the caller drains with
 * jpeg_encoder_consume_pending(). */
bool jpeg_encoder_begin_send(const uint8_t *frame, unsigned width, unsigned height);
jpeg_encoder_status_t jpeg_encoder_send_step(unsigned max_mcus);
const uint8_t *jpeg_encoder_pending_data(void);
size_t jpeg_encoder_pending_size(void);
void jpeg_encoder_consume_pending(size_t count);

#endif
