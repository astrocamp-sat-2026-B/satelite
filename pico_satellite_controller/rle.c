#include "rle.h"

#include <string.h>

#define RLE_MAX_RUN 128u

static inline uint16_t read_pixel(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline void write_pixel(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

size_t rle_encode_rgb565_chunk(const uint8_t *pixels, size_t pixel_bytes,
                                size_t *src_pos, uint8_t *out,
                                size_t out_capacity) {
    size_t pixel_count = pixel_bytes / 2u;
    size_t i = *src_pos / 2u;
    size_t o = 0;

    while (i < pixel_count) {
        uint16_t value = read_pixel(pixels + i * 2u);
        size_t run = 1;
        while (i + run < pixel_count && run < RLE_MAX_RUN &&
               read_pixel(pixels + (i + run) * 2u) == value) {
            run++;
        }

        if (run >= 2u) {
            if (o + 3u > out_capacity) break; /* no room; resume here next call */
            out[o++] = (uint8_t)(0x80u | (run - 1u));
            write_pixel(out + o, value);
            o += 2u;
            i += run;
            continue;
        }

        /* No repeat here: gather a literal run until the next repeat of
         * 2+ pixels appears or the packet hits its 128-pixel limit. */
        size_t peek = i;
        size_t count = 0;
        while (peek < pixel_count && count < RLE_MAX_RUN) {
            uint16_t v = read_pixel(pixels + peek * 2u);
            size_t next_run = 1;
            while (peek + next_run < pixel_count && next_run < RLE_MAX_RUN &&
                   read_pixel(pixels + (peek + next_run) * 2u) == v) {
                next_run++;
            }
            if (next_run >= 2u) break;
            peek++;
            count++;
        }

        if (o + 1u + count * 2u > out_capacity) break; /* resume here next call */
        out[o++] = (uint8_t)(count - 1u);
        memcpy(out + o, pixels + i * 2u, count * 2u);
        o += count * 2u;
        i = peek;
    }

    *src_pos = i * 2u;
    return o;
}
