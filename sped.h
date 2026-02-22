/*
 * sped.h — Simplest PNG ESP32 Decoder
 *
 * Minimal streaming PNG decoder for embedded systems.
 * Outputs RGB565 row-by-row via callback. Uses tinfl (from miniz)
 * for DEFLATE decompression. Supports 1/2 and 1/4 downscaling.
 *
 * Supports: 8-bit grayscale, RGB, RGBA, grayscale+alpha, indexed (palette).
 * Does not support: interlacing, 16-bit channels, CRC verification.
 *
 * MIT License — see LICENSE file.
 */
#ifndef SPED_H
#define SPED_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t width;
    uint32_t height;
} sped_info_t;

/* Row callback: y = row (0=top), w = width, rgb565 = pixel data.
 * Called once per row during decoding. */
typedef void (*sped_row_cb)(int y, int w, const uint16_t *rgb565, void *user);

/* Get image dimensions without decoding. Returns 0 on success. */
int sped_info(const void *png, size_t len, sped_info_t *info);

/* Decode PNG to RGB565. Calls cb for each row. Returns 0 on success.
 * scale: 1 = full, 2 = half, 4 = quarter resolution. */
int sped_decode(const void *png, size_t len, int scale,
                sped_row_cb cb, void *user);

#endif /* SPED_H */
