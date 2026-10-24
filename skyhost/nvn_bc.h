#ifndef SKY_NVN_BC_H
#define SKY_NVN_BC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sky_bc_format_info {
    uint32_t decoded_format;
    uint32_t bytes_per_pixel;
    uint32_t block_bytes;
} sky_bc_format_info_t;

/* Logical/decoded format values are MTLPixelFormat values. Unknown formats
 * return zero without changing *out. BC4/5 use precise normalized decoding
 * rounded to binary16 in R16Float/RG16Float, including signed values.
 * BC6H expands RGB binary16 to RGBA16Float with opaque alpha. */
int sky_bc_format_info(uint32_t logical_format, sky_bc_format_info_t *out);

/* Decode linear rows of 4x4 compressed blocks. Pitches and buffer sizes are
 * bytes. Block counts round up; edge pixels are clipped to width/height.
 * All sizes/strides and nonoverlapping spans are checked before any write.
 * Returns one on success, zero on invalid input; performs no allocation.
 * Encoded sRGB bytes remain encoded. RGB_DXT1's opaque-alpha interpretation
 * stays in the caller's existing NVN component mapping. */
int sky_bc_decode_region(uint32_t logical_format, uint32_t width, uint32_t height,
                         const void *src, size_t src_size, size_t src_row_pitch,
                         void *dst, size_t dst_size, size_t dst_row_pitch);

#ifdef __cplusplus
}
#endif

#endif
