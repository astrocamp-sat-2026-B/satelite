#include "jpeg_encoder.h"

#include <string.h>

#include "third_party/JPEGENC/JPEGENC.h"

/* Color is retained. 4:2:0 reduces chroma traffic and Q_LOW is deliberate:
 * the telemetry link benefits more from a small, regular JPEG than from a
 * near-lossless RGB565 representation. */
#define JPEG_SUBSAMPLE JPEGE_SUBSAMPLE_420
#define JPEG_QUALITY JPEGE_Q_LOW

static struct {
    JPEGE_IMAGE image;
    JPEGENCODE encode;
    const uint8_t *frame;
    unsigned width;
    unsigned height;
    bool started;
    bool ended;
    bool failed;
    uint8_t pending[JPEG_ENCODER_OUTPUT_CAPACITY];
    size_t pending_offset;
    size_t pending_length;
} encoder;

static int32_t send_write(JPEGE_FILE *file, uint8_t *data, int32_t length) {
    (void)file;
    if (length < 0 || (size_t)length > sizeof(encoder.pending) ||
        encoder.pending_length != 0) {
        encoder.failed = true;
        return 0;
    }
    memcpy(encoder.pending, data, (size_t)length);
    encoder.pending_offset = 0;
    encoder.pending_length = (size_t)length;
    return length;
}

static bool begin(const uint8_t *frame, unsigned width, unsigned height) {
    if (frame == NULL || width == 0 || height == 0 ||
        (width & 15u) != 0 || (height & 15u) != 0) {
        return false;
    }

    memset(&encoder, 0, sizeof(encoder));
    encoder.frame = frame;
    encoder.width = width;
    encoder.height = height;
    encoder.image.pfnWrite = send_write;
    encoder.image.pHighWater =
        &encoder.image.ucFileBuf[JPEGE_FILE_BUF_SIZE - 512u];

    if (JPEGEncodeBegin(&encoder.image, &encoder.encode, (int)width, (int)height,
                        JPEGE_PIXEL_RGB565, JPEG_SUBSAMPLE, JPEG_QUALITY) !=
        JPEGE_SUCCESS) {
        encoder.failed = true;
        return false;
    }
    encoder.started = true;
    return true;
}

bool jpeg_encoder_begin_send(const uint8_t *frame, unsigned width, unsigned height) {
    return begin(frame, width, height);
}

static jpeg_encoder_status_t step(unsigned max_mcus) {
    if (!encoder.started || encoder.failed) return JPEG_ENCODER_ERROR;

    while (max_mcus != 0 && encoder.encode.y < (int)encoder.height &&
           encoder.pending_length == 0) {
        const size_t pixel_offset =
            ((size_t)encoder.encode.y * encoder.width + (unsigned)encoder.encode.x) * 2u;
        if (JPEGAddMCU(&encoder.image, &encoder.encode,
                       (uint8_t *)(encoder.frame + pixel_offset),
                       (int)(encoder.width * 2u)) != JPEGE_SUCCESS || encoder.failed) {
            encoder.failed = true;
            return JPEG_ENCODER_ERROR;
        }
        --max_mcus;
    }

    if (encoder.encode.y >= (int)encoder.height && encoder.pending_length == 0 &&
        !encoder.ended) {
        if (JPEGEncodeEnd(&encoder.image) <= 0 || encoder.failed) {
            encoder.failed = true;
            return JPEG_ENCODER_ERROR;
        }
        encoder.ended = true;
    }

    if (encoder.failed) return JPEG_ENCODER_ERROR;
    if (encoder.ended && encoder.pending_length == 0) return JPEG_ENCODER_DONE;
    return JPEG_ENCODER_IN_PROGRESS;
}

jpeg_encoder_status_t jpeg_encoder_send_step(unsigned max_mcus) {
    return step(max_mcus);
}

const uint8_t *jpeg_encoder_pending_data(void) {
    return encoder.pending + encoder.pending_offset;
}

size_t jpeg_encoder_pending_size(void) {
    return encoder.pending_length;
}

void jpeg_encoder_consume_pending(size_t count) {
    if (count > encoder.pending_length) {
        encoder.failed = true;
        return;
    }
    encoder.pending_offset += count;
    encoder.pending_length -= count;
    if (encoder.pending_length == 0) encoder.pending_offset = 0;
}
