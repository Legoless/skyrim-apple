#include "nvn_bc.h"

#include <limits.h>
#include <string.h>

#define BCDEC_BC4BC5_PRECISE
#define BCDECDEF static inline
#define BCDEC_IMPLEMENTATION
#include "third_party/bcdec/bcdec.h"

_Static_assert(CHAR_BIT == 8 && CHAR_MIN < 0, "bcdec requires signed 8-bit char on Apple platforms");
_Static_assert(sizeof(unsigned int) == 4 && sizeof(float) == 4, "bcdec requires 32-bit int/float");

int sky_bc_format_info(uint32_t format, sky_bc_format_info_t *out) {
    if (!out) return 0;
    sky_bc_format_info_t info;
    switch (format) {
        case 130: info = (sky_bc_format_info_t){70, 4, 8}; break;   /* BC1 RGBA -> RGBA8 */
        case 131: info = (sky_bc_format_info_t){71, 4, 8}; break;   /* BC1 sRGB */
        case 132: case 134: case 152:
            info = (sky_bc_format_info_t){70, 4, 16}; break;       /* BC2/3/7 */
        case 133: case 135: case 153:
            info = (sky_bc_format_info_t){71, 4, 16}; break;       /* BC2/3/7 sRGB */
        case 140: case 141:
            info = (sky_bc_format_info_t){25, 2, 8}; break;       /* BC4 -> R16Float */
        case 142: case 143:
            info = (sky_bc_format_info_t){65, 4, 16}; break;      /* BC5 -> RG16Float */
        case 150: case 151:
            info = (sky_bc_format_info_t){115, 8, 16}; break;     /* BC6H -> RGBA16Float */
        default: return 0;
    }
    *out = info;
    return 1;
}

/* Round IEEE binary32 to binary16, ties to even. BC4/5 values are normalized;
 * keeping their float interpolation until this step avoids an 8-bit truncation. */
static uint16_t float_to_half(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    uint32_t sign = (bits >> 16) & 0x8000u;
    uint32_t mantissa = bits & 0x7fffffu;
    uint32_t exponent = (bits >> 23) & 0xffu;
    if (exponent == 0xffu) return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x0200u : 0));
    int half_exponent = (int)exponent - 127 + 15;
    if (half_exponent >= 31) return (uint16_t)(sign | 0x7c00u);
    uint32_t rounded, remainder, halfway;
    if (half_exponent <= 0) {
        if (half_exponent < -10) return (uint16_t)sign;
        mantissa |= 0x800000u;
        unsigned shift = (unsigned)(14 - half_exponent);
        rounded = mantissa >> shift;
        remainder = mantissa & ((1u << shift) - 1u);
        halfway = 1u << (shift - 1u);
    } else {
        rounded = ((uint32_t)half_exponent << 10) | (mantissa >> 13);
        remainder = mantissa & 0x1fffu;
        halfway = 0x1000u;
    }
    if (remainder > halfway || (remainder == halfway && (rounded & 1u))) rounded++;
    return (uint16_t)(sign | rounded);
}

typedef struct decoded_block {
    union {
        uint32_t rgba[16];
        float channels[32];
        uint16_t rgb_half[48];
    } values;
    uint16_t expanded_half[64];
} decoded_block_t;

static const void *decode_block(uint32_t format, const void *src, decoded_block_t *block) {
    switch (format) {
        case 130: case 131:
            bcdec_bc1(src, block->values.rgba, 16);
            return block->values.rgba;
        case 132: case 133:
            bcdec_bc2(src, block->values.rgba, 16);
            return block->values.rgba;
        case 134: case 135:
            bcdec_bc3(src, block->values.rgba, 16);
            return block->values.rgba;
        case 152: case 153:
            bcdec_bc7(src, block->values.rgba, 16);
            return block->values.rgba;
        case 140: case 141:
            bcdec_bc4_float(src, block->values.channels, 4, format == 141);
            for (unsigned i = 0; i < 16; i++) block->expanded_half[i] = float_to_half(block->values.channels[i]);
            return block->expanded_half;
        case 142: case 143:
            bcdec_bc5_float(src, block->values.channels, 8, format == 143);
            for (unsigned i = 0; i < 32; i++) block->expanded_half[i] = float_to_half(block->values.channels[i]);
            return block->expanded_half;
        case 150: case 151:
            /* bcdec's half/float pitch counts components, not bytes. */
            bcdec_bc6h_half(src, block->values.rgb_half, 12, format == 150);
            for (unsigned i = 0; i < 16; i++) {
                for (unsigned c = 0; c < 3; c++) block->expanded_half[4 * i + c] = block->values.rgb_half[3 * i + c];
                block->expanded_half[4 * i + 3] = 0x3c00; /* alpha = 1 */
            }
            return block->expanded_half;
        default: return NULL;
    }
}

static int multiply_size(size_t a, size_t b, size_t *out) {
    if (b && a > SIZE_MAX / b) return 0;
    *out = a * b;
    return 1;
}

static int row_span(size_t rows, size_t bytes, size_t pitch, size_t *out) {
    if (!rows || !bytes || pitch < bytes) return 0;
    if (rows - 1 > (SIZE_MAX - bytes) / pitch) return 0;
    *out = (rows - 1) * pitch + bytes;
    return 1;
}

int sky_bc_decode_region(uint32_t format, uint32_t width, uint32_t height,
                         const void *src, size_t src_size, size_t src_row_pitch,
                         void *dst, size_t dst_size, size_t dst_row_pitch) {
    sky_bc_format_info_t info;
    if (!sky_bc_format_info(format, &info) || !width || !height || !src || !dst) return 0;
    size_t columns = width / 4u + (width % 4u != 0);
    size_t rows = height / 4u + (height % 4u != 0);
    size_t source_row, output_row, source_span, output_span;
    if (!multiply_size(columns, info.block_bytes, &source_row) ||
        !multiply_size(width, info.bytes_per_pixel, &output_row) ||
        !row_span(rows, source_row, src_row_pitch, &source_span) ||
        !row_span(height, output_row, dst_row_pitch, &output_span) ||
        source_span > src_size || output_span > dst_size) return 0;
    uintptr_t source_address = (uintptr_t)src, output_address = (uintptr_t)dst;
    if (source_span > UINTPTR_MAX - source_address || output_span > UINTPTR_MAX - output_address) return 0;
    if (source_address < output_address + output_span && output_address < source_address + source_span) return 0;

    decoded_block_t block;
    for (size_t by = 0; by < rows; by++) {
        size_t y = by * 4u, copy_height = height - y;
        if (copy_height > 4) copy_height = 4;
        for (size_t bx = 0; bx < columns; bx++) {
            size_t x = bx * 4u, copy_width = width - x;
            if (copy_width > 4) copy_width = 4;
            const unsigned char *input = (const unsigned char *)src + by * src_row_pitch + bx * info.block_bytes;
            const unsigned char *pixels = decode_block(format, input, &block);
            if (!pixels) return 0;
            for (size_t row = 0; row < copy_height; row++) {
                unsigned char *output = (unsigned char *)dst + (y + row) * dst_row_pitch + x * info.bytes_per_pixel;
                memcpy(output, pixels + row * 4u * info.bytes_per_pixel, copy_width * info.bytes_per_pixel);
            }
        }
    }
    return 1;
}
