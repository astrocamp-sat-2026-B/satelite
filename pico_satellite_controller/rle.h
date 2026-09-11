#ifndef ASTROCAMP_RLE_H
#define ASTROCAMP_RLE_H

#include <stddef.h>
#include <stdint.h>

/* Lossless run-length encoding of RGB565 pixel data (2 bytes/pixel),
 * produced one bounded chunk at a time so the caller never needs a
 * second full-frame buffer alongside the raw capture. A run of 2-128
 * identical pixels is packed as {0x80|(run-1), lo, hi}; otherwise up to
 * 128 pixels are stored literally as {count-1, pixel...}.
 *
 * Encodes starting at *src_pos (a byte offset into `pixels`) and writes
 * as many whole packets as fit within out_capacity, advancing *src_pos
 * to match what was actually consumed. Call repeatedly, feeding each
 * call's output onward (e.g. into a socket), until *src_pos reaches
 * pixel_bytes. Returns the number of bytes written to `out` (0 if
 * out_capacity was too small for even one packet, or if *src_pos is
 * already at pixel_bytes). */
size_t rle_encode_rgb565_chunk(const uint8_t *pixels, size_t pixel_bytes,
                                size_t *src_pos, uint8_t *out,
                                size_t out_capacity);

#endif
