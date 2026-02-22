/*
 * sped.c — Smallest PNG ESP32 Decoder
 *
 * Streaming PNG decoder: parses chunks, inflates IDAT data via tinfl,
 * reconstructs scanline filters, converts to RGB565 row-by-row.
 * Supports 1/2 and 1/4 downscaling via pixel averaging.
 *
 * Requires: miniz.h (tinfl) — available in ESP-IDF via esp_rom,
 * or from https://github.com/richgel999/miniz
 */

#include "sped.h"
#include <string.h>
#include <stdlib.h>

#ifndef SPED_INFLATE_INCLUDE
#define SPED_INFLATE_INCLUDE "miniz.h"
#endif
#include SPED_INFLATE_INCLUDE

/* PNG file signature */
static const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};

/* Read 4-byte big-endian integer */
static uint32_t r32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Paeth predictor (PNG filter type 4) */
static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = (int)a + (int)b - (int)c;
    int pa = p - (int)a; if (pa < 0) pa = -pa;
    int pb = p - (int)b; if (pb < 0) pb = -pb;
    int pc = p - (int)c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Extract RGB from decoded scanline based on color type and bit depth.
 * For 16-bit channels, takes the high byte (lossy but correct for RGB565). */
static void get_pixel(const uint8_t *cur, uint32_t x, uint8_t ctype,
                      int bpc, const uint8_t pal[][3],
                      uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (bpc == 1) {
        switch (ctype) {
            case 0: *r = *g = *b = cur[x]; break;
            case 2: *r = cur[x*3]; *g = cur[x*3+1]; *b = cur[x*3+2]; break;
            case 3: { uint8_t idx = cur[x];
                      *r = pal[idx][0]; *g = pal[idx][1]; *b = pal[idx][2]; break; }
            case 4: *r = *g = *b = cur[x*2]; break;
            case 6: *r = cur[x*4]; *g = cur[x*4+1]; *b = cur[x*4+2]; break;
            default: *r = *g = *b = 0;
        }
    } else {
        /* 16-bit: take high byte of each channel */
        switch (ctype) {
            case 0: *r = *g = *b = cur[x*2]; break;
            case 2: *r = cur[x*6]; *g = cur[x*6+2]; *b = cur[x*6+4]; break;
            case 4: *r = *g = *b = cur[x*4]; break;
            case 6: *r = cur[x*8]; *g = cur[x*8+2]; *b = cur[x*8+4]; break;
            default: *r = *g = *b = 0;
        }
    }
}

/* Max IDAT chunks we track */
#define SPED_MAX_IDAT 64

int sped_info(const void *png, size_t len, sped_info_t *info)
{
    const uint8_t *p = png;
    if (len < 33 || memcmp(p, png_sig, 8) != 0) return -1;
    p += 8;
    if (r32(p) != 13 || memcmp(p + 4, "IHDR", 4) != 0) return -1;
    info->width = r32(p + 8);
    info->height = r32(p + 12);
    return 0;
}

int sped_decode(const void *png, size_t len, int scale,
                sped_row_cb cb, void *user)
{
    if (scale != 1 && scale != 2 && scale != 4) return -1;

    const uint8_t *base = png;
    const uint8_t *end = base + len;

    /* Signature */
    if (len < 33 || memcmp(base, png_sig, 8) != 0) return -1;

    /* IHDR must be the first chunk */
    const uint8_t *p = base + 8;
    if (r32(p) != 13 || memcmp(p + 4, "IHDR", 4) != 0) return -1;

    const uint8_t *ihdr = p + 8;
    uint32_t w = r32(ihdr);
    uint32_t h = r32(ihdr + 4);
    uint8_t depth = ihdr[8];
    uint8_t ctype = ihdr[9];

    /* Reject unsupported features */
    if (ihdr[10] != 0) return -1;  /* compression must be 0 */
    if (ihdr[11] != 0) return -1;  /* filter must be 0 */
    if (ihdr[12] != 0) return -1;  /* interlace not supported */
    if (depth != 8 && depth != 16) return -1;
    if (depth == 16 && ctype == 3) return -1;  /* 16-bit indexed doesn't exist */
    if (w == 0 || h == 0) return -1;

    /* Bytes per pixel (16-bit channels = 2 bytes each) */
    int bpc = depth / 8;  /* bytes per channel: 1 or 2 */
    int bpp;
    switch (ctype) {
        case 0: bpp = 1 * bpc; break;  /* grayscale */
        case 2: bpp = 3 * bpc; break;  /* RGB */
        case 3: bpp = 1;       break;  /* indexed (always 8-bit) */
        case 4: bpp = 2 * bpc; break;  /* grayscale + alpha */
        case 6: bpp = 4 * bpc; break;  /* RGBA */
        default: return -1;
    }
    int stride = (int)(w * bpp);

    /* Output dimensions */
    uint32_t out_w = w / (uint32_t)scale;
    uint32_t out_h = h / (uint32_t)scale;
    if (out_w == 0 || out_h == 0) return -1;

    /* Scan chunks: collect PLTE, tRNS, IDAT pointers */
    uint8_t pal[256][3];
    uint8_t pal_a[256];
    memset(pal_a, 255, sizeof(pal_a));

    struct { const uint8_t *data; uint32_t len; } idat[SPED_MAX_IDAT];
    int nidat = 0;

    const uint8_t *cp = base + 8 + 25; /* after signature + IHDR (25 = 4+4+13+4) */
    while (cp + 12 <= end) {
        uint32_t clen = r32(cp);
        if (cp + 12 + clen > end) break;

        if (memcmp(cp + 4, "PLTE", 4) == 0) {
            int n = (int)(clen / 3);
            if (n > 256) n = 256;
            for (int i = 0; i < n; i++) {
                pal[i][0] = cp[8 + i * 3];
                pal[i][1] = cp[8 + i * 3 + 1];
                pal[i][2] = cp[8 + i * 3 + 2];
            }
        } else if (memcmp(cp + 4, "tRNS", 4) == 0) {
            if (ctype == 3) {
                for (uint32_t i = 0; i < clen && i < 256; i++)
                    pal_a[i] = cp[8 + i];
            }
        } else if (memcmp(cp + 4, "IDAT", 4) == 0) {
            if (nidat < SPED_MAX_IDAT) {
                idat[nidat].data = cp + 8;
                idat[nidat].len = clen;
                nidat++;
            }
        } else if (memcmp(cp + 4, "IEND", 4) == 0) {
            break;
        }
        cp += 12 + clen;
    }
    if (nidat == 0) return -1;

    /* Allocate work buffers */
    uint8_t *cur = calloc(1, stride);
    uint8_t *prev = calloc(1, stride);
    uint16_t *out = malloc(out_w * sizeof(uint16_t));
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);

    /* Accumulator for downscaling: sum of R, G, B per output pixel */
    uint16_t *acc = NULL;
    if (scale > 1)
        acc = calloc(out_w * 3, sizeof(uint16_t));

    if (!cur || !prev || !out || !dict || (scale > 1 && !acc)) {
        free(cur); free(prev); free(out); free(dict); free(acc);
        return -1;
    }

    /* Init inflate */
    tinfl_decompressor decomp;
    tinfl_init(&decomp);
    size_t dict_ofs = 0;

    /* IDAT feed state */
    int ci = 0;                       /* current IDAT index */
    const uint8_t *in_ptr = idat[0].data;
    size_t in_remain = idat[0].len;

    /* Scanline assembly state */
    int sl_pos = 0;        /* 0 = expecting filter byte, 1..stride = pixel data */
    uint8_t filter = 0;
    int row = 0;
    int out_row = 0;

    while (row < (int)h) {
        /* Determine flags for tinfl */
        int more = (ci < nidat - 1) || (in_remain > 0);
        uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
        if (more) flags |= TINFL_FLAG_HAS_MORE_INPUT;

        size_t in_bytes = in_remain;
        size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;

        tinfl_status st = tinfl_decompress(&decomp, in_ptr, &in_bytes,
                                           dict, dict + dict_ofs, &out_bytes,
                                           flags);
        in_ptr += in_bytes;
        in_remain -= in_bytes;

        /* Process decompressed output */
        const uint8_t *dp = dict + dict_ofs;
        size_t avail = out_bytes;
        dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);

        while (avail > 0 && row < (int)h) {
            if (sl_pos == 0) {
                filter = *dp++;
                avail--;
                sl_pos = 1;
            } else {
                size_t need = (size_t)(stride - (sl_pos - 1));
                size_t take = (avail < need) ? avail : need;
                memcpy(cur + (sl_pos - 1), dp, take);
                dp += take;
                avail -= take;
                sl_pos += (int)take;

                if (sl_pos > stride) {
                    /* Scanline complete — apply inverse filter */
                    for (int i = 0; i < stride; i++) {
                        uint8_t a = (i >= bpp) ? cur[i - bpp] : 0;
                        uint8_t b = prev[i];
                        uint8_t c_val = (i >= bpp) ? prev[i - bpp] : 0;
                        switch (filter) {
                            case 1: cur[i] += a; break;
                            case 2: cur[i] += b; break;
                            case 3: cur[i] += (uint8_t)((a + b) >> 1); break;
                            case 4: cur[i] += paeth(a, b, c_val); break;
                        }
                    }

                    if (scale == 1) {
                        /* Convert to RGB565 and emit directly */
                        for (uint32_t x = 0; x < w; x++) {
                            uint8_t r, g, bl;
                            get_pixel(cur, x, ctype, bpc, pal, &r, &g, &bl);
                            out[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3);
                        }
                        cb(row, (int)w, out, user);
                    } else {
                        /* Accumulate R/G/B for downscaling */
                        uint32_t limit = out_w * (uint32_t)scale;
                        for (uint32_t x = 0; x < limit && x < w; x++) {
                            uint8_t r, g, bl;
                            get_pixel(cur, x, ctype, bpc, pal, &r, &g, &bl);
                            uint32_t ox = x / (uint32_t)scale;
                            acc[ox * 3 + 0] += r;
                            acc[ox * 3 + 1] += g;
                            acc[ox * 3 + 2] += bl;
                        }

                        /* Emit averaged row every 'scale' input rows */
                        if ((row % scale) == scale - 1) {
                            int div = scale * scale;
                            for (uint32_t ox = 0; ox < out_w; ox++) {
                                uint8_t r  = (uint8_t)(acc[ox * 3 + 0] / div);
                                uint8_t g  = (uint8_t)(acc[ox * 3 + 1] / div);
                                uint8_t bl = (uint8_t)(acc[ox * 3 + 2] / div);
                                out[ox] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3);
                            }
                            cb(out_row, (int)out_w, out, user);
                            out_row++;
                            memset(acc, 0, out_w * 3 * sizeof(uint16_t));
                        }
                    }

                    /* Swap cur/prev */
                    uint8_t *tmp = prev; prev = cur; cur = tmp;
                    memset(cur, 0, stride);
                    row++;
                    sl_pos = 0;
                }
            }
        }

        /* Advance to next IDAT chunk if needed */
        if (in_remain == 0 && ci < nidat - 1) {
            ci++;
            in_ptr = idat[ci].data;
            in_remain = idat[ci].len;
        }

        if (st == TINFL_STATUS_DONE) break;
        if (st < 0) {
            free(cur); free(prev); free(out); free(dict); free(acc);
            return -1;
        }
    }

    free(cur); free(prev); free(out); free(dict); free(acc);
    return 0;
}
