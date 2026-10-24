#include "nvn_bc.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* These blocks are authored format fixtures. No game bytes or upstream test
 * images are used. Endpoint/index layouts follow the public BC specifications. */
static void put16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void constant_bc1(unsigned char block[8], uint16_t color) {
    memset(block, 0, 8);
    put16(block, color);
    put16(block + 2, color);
}

static void alpha_indices(unsigned char block[8], unsigned char a, unsigned char b, int reverse) {
    block[0] = a;
    block[1] = b;
    uint64_t indices = 0;
    for (unsigned i = 0; i < 16; i++) indices |= (uint64_t)(reverse ? 7 - i % 8 : i % 8) << (3 * i);
    for (unsigned i = 0; i < 6; i++) block[2 + i] = (unsigned char)(indices >> (8 * i));
}

/* Clang's native binary16 conversion is independent of the wrapper's integer
 * round-to-nearest-even conversion and supplies the normalized-channel oracle. */
static uint16_t half_bits(float value) {
    __fp16 half = (__fp16)value;
    uint16_t bits;
    memcpy(&bits, &half, sizeof bits);
    return bits;
}

static void write_bits(unsigned char block[16], unsigned *position, unsigned value, unsigned count) {
    assert(*position + count <= 128);
    assert(count < 32 && value < (1u << count));
    for (unsigned i = 0; i < count; i++, (*position)++)
        block[*position / 8] |= ((value >> i) & 1u) << (*position % 8);
}

static void test_formats(void) {
    const struct { uint32_t input, output, pixel, block; } cases[] = {
        {130,70,4,8}, {131,71,4,8}, {132,70,4,16}, {133,71,4,16},
        {134,70,4,16}, {135,71,4,16}, {140,25,2,8}, {141,25,2,8},
        {142,65,4,16}, {143,65,4,16}, {150,115,8,16}, {151,115,8,16},
        {152,70,4,16}, {153,71,4,16},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        sky_bc_format_info_t info = {0};
        assert(sky_bc_format_info(cases[i].input, &info));
        assert(info.decoded_format == cases[i].output);
        assert(info.bytes_per_pixel == cases[i].pixel && info.block_bytes == cases[i].block);
    }
    const uint32_t unsupported[] = {0, 70, 129, 136, 139, 144, 149, 154, UINT32_MAX};
    for (unsigned i = 0; i < sizeof unsupported / sizeof unsupported[0]; i++) {
        sky_bc_format_info_t info = {111, 222, 333};
        assert(!sky_bc_format_info(unsupported[i], &info));
        assert(info.decoded_format == 111 && info.bytes_per_pixel == 222 && info.block_bytes == 333);
    }
    assert(!sky_bc_format_info(130, NULL));
}

static void test_bc1(void) {
    unsigned char block[8] = {0x00,0xf8,0xe0,0x07,0xe4,0xe4,0xe4,0xe4};
    const unsigned char colors[4][4] = {{255,0,0,255}, {0,255,0,255}, {170,85,0,255}, {85,170,0,255}};
    unsigned char rgba[64];
    for (uint32_t format = 130; format <= 131; format++) {
        assert(sky_bc_decode_region(format, 4, 4, block, sizeof block, 8, rgba, sizeof rgba, 16));
        for (unsigned pixel = 0; pixel < 16; pixel++) assert(!memcmp(rgba + pixel * 4, colors[pixel % 4], 4));
    }
    memset(block, 0, sizeof block);
    put16(block + 2, 0xffff);
    memset(block + 4, 0xff, 4);
    assert(sky_bc_decode_region(130, 4, 4, block, sizeof block, 8, rgba, sizeof rgba, 16));
    /* Transparent black stays transparent here; NVN RGB_DXT1 uses its existing
     * alpha-one view mapping, without incorrectly selecting four-color RGB. */
    for (unsigned i = 0; i < sizeof rgba; i++) assert(rgba[i] == 0);
}

static void test_bc2_bc3(void) {
    unsigned char block[16], rgba[64];
    constant_bc1(block + 8, 0xf800);
    memset(block + 12, 0xff, 4); /* Equal endpoints/index3 must still use opaque RGB. */
    for (unsigned i = 0; i < 8; i++) block[i] = (unsigned char)(((2 * i + 1) << 4) | (2 * i));
    for (uint32_t format = 132; format <= 133; format++) {
        assert(sky_bc_decode_region(format, 4, 4, block, sizeof block, 16, rgba, sizeof rgba, 16));
        for (unsigned p = 0; p < 16; p++) {
            assert(rgba[p * 4] == 255 && !rgba[p * 4 + 1] && !rgba[p * 4 + 2]);
            assert(rgba[p * 4 + 3] == p * 17);
        }
    }
    const unsigned char alpha[2][8] = {{255,0,218,182,145,109,72,36}, {0,255,51,102,153,204,0,255}};
    for (unsigned mode = 0; mode < 2; mode++) {
        alpha_indices(block, mode ? 0 : 255, mode ? 255 : 0, 0);
        for (uint32_t format = 134; format <= 135; format++) {
            assert(sky_bc_decode_region(format, 4, 4, block, sizeof block, 16, rgba, sizeof rgba, 16));
            for (unsigned p = 0; p < 16; p++) {
                assert(rgba[p * 4] == 255 && !rgba[p * 4 + 1] && !rgba[p * 4 + 2]);
                assert(rgba[p * 4 + 3] == alpha[mode][p % 8]);
            }
        }
    }
}

static void test_bc4_bc5(void) {
    const float unsigned_desc[8] = {1,0,6.f/7,5.f/7,4.f/7,3.f/7,2.f/7,1.f/7};
    const float unsigned_asc[8] = {0,1,1.f/5,2.f/5,3.f/5,4.f/5,0,1};
    const float signed_asc[8] = {-1,1,-3.f/5,-1.f/5,1.f/5,3.f/5,-1,1};
    const float signed_desc[8] = {1,-1,5.f/7,3.f/7,1.f/7,-1.f/7,-3.f/7,-5.f/7};
    unsigned char block[16];
    uint16_t halves[32];
    for (unsigned sign = 0; sign < 2; sign++) {
        alpha_indices(block, sign ? 128 : 255, sign ? 127 : 0, 0);
        const float *red = sign ? signed_asc : unsigned_desc;
        assert(sky_bc_decode_region(140 + sign, 4, 4, block, 8, 8, halves, 32, 8));
        for (unsigned p = 0; p < 16; p++) assert(halves[p] == half_bits(red[p % 8]));

        alpha_indices(block + 8, sign ? 127 : 0, sign ? 128 : 255, 1);
        const float *green = sign ? signed_desc : unsigned_asc;
        assert(sky_bc_decode_region(142 + sign, 4, 4, block, sizeof block, 16, halves, sizeof halves, 16));
        for (unsigned p = 0; p < 16; p++) {
            assert(halves[p * 2] == half_bits(red[p % 8]));
            assert(halves[p * 2 + 1] == half_bits(green[7 - p % 8]));
        }
    }
}

static void bc6_constant(unsigned char block[16], unsigned r, unsigned g, unsigned b) {
    memset(block, 0, 16);
    unsigned position = 0;
    /* BC6H mode11: one subset, two explicit 10-bit RGB endpoints, then indices. */
    write_bits(block, &position, 3, 5);
    for (unsigned endpoint = 0; endpoint < 2; endpoint++) {
        write_bits(block, &position, r, 10);
        write_bits(block, &position, g, 10);
        write_bits(block, &position, b, 10);
    }
    write_bits(block, &position, 0, 3);
    for (unsigned p = 1; p < 16; p++) write_bits(block, &position, 0, 4);
    assert(position == 128);
}

static void test_bc6(void) {
    unsigned char block[16];
    uint16_t rgba[64];
    bc6_constant(block, 495, 0, 1023);
    assert(sky_bc_decode_region(151, 4, 4, block, 16, 16, rgba, sizeof rgba, 32));
    for (unsigned p = 0; p < 16; p++) {
        /* Public BC6H unquantization maps 495 to1.0 and1023 to65504. */
        assert(rgba[p * 4] == 0x3c00 && rgba[p * 4 + 1] == 0);
        assert(rgba[p * 4 + 2] == 0x7bff && rgba[p * 4 + 3] == 0x3c00);
    }
    bc6_constant(block, 512, 511, 0);
    assert(sky_bc_decode_region(150, 4, 4, block, 16, 16, rgba, sizeof rgba, 32));
    for (unsigned p = 0; p < 16; p++) {
        assert(rgba[p * 4] == 0xfbff && rgba[p * 4 + 1] == 0x7bff);
        assert(rgba[p * 4 + 2] == 0 && rgba[p * 4 + 3] == 0x3c00);
    }
    memset(block, 0, sizeof block);
    block[0] = 0x13; /* Reserved BC6H mode: opaque black. */
    assert(sky_bc_decode_region(151, 4, 4, block, 16, 16, rgba, sizeof rgba, 32));
    for (unsigned p = 0; p < 16; p++) {
        assert(!rgba[p * 4] && !rgba[p * 4 + 1] && !rgba[p * 4 + 2]);
        assert(rgba[p * 4 + 3] == 0x3c00);
    }
}

static void bc7_constant(unsigned char block[16]) {
    memset(block, 0, 16);
    unsigned position = 0;
    write_bits(block, &position, 1u << 6, 7); /* Mode6, one subset. */
    const unsigned components[4] = {127, 0, 0, 127};
    for (unsigned c = 0; c < 4; c++)
        for (unsigned endpoint = 0; endpoint < 2; endpoint++) write_bits(block, &position, components[c], 7);
    write_bits(block, &position, 0, 2); /* Both endpoint P-bits zero. */
    write_bits(block, &position, 0, 3);
    for (unsigned p = 1; p < 16; p++) write_bits(block, &position, 0, 4);
    assert(position == 128);
}

static void test_bc7(void) {
    unsigned char block[16], rgba[64];
    bc7_constant(block);
    const unsigned char color[4] = {254, 0, 0, 254};
    for (uint32_t format = 152; format <= 153; format++) {
        assert(sky_bc_decode_region(format, 4, 4, block, 16, 16, rgba, sizeof rgba, 16));
        for (unsigned p = 0; p < 16; p++) assert(!memcmp(rgba + p * 4, color, 4));
    }
    unsigned char tiny[24];
    assert(sky_bc_decode_region(152, 2, 3, block, 16, 16, tiny, sizeof tiny, 8));
    for (unsigned p = 0; p < 6; p++) assert(!memcmp(tiny + p * 4, color, 4));
    memset(block, 0, sizeof block); /* Invalid mode decodes to transparent black. */
    assert(sky_bc_decode_region(152, 4, 4, block, 16, 16, rgba, sizeof rgba, 16));
    for (unsigned i = 0; i < sizeof rgba; i++) assert(!rgba[i]);
}

static void test_regions_and_bounds(void) {
    unsigned char source[40], original[40], destination[116];
    memset(source, 0xaa, sizeof source);
    const uint16_t rgb565[4] = {0xf800, 0x07e0, 0x001f, 0xffff};
    const unsigned char colors[4][4] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,255,255}};
    for (unsigned by = 0; by < 2; by++)
        for (unsigned bx = 0; bx < 2; bx++) constant_bc1(source + 1 + by * 19 + bx * 8, rgb565[by * 2 + bx]);
    memcpy(original, source, sizeof source);
    memset(destination, 0xcd, sizeof destination);
    /* Unaligned input/output, non-aligned pitches, clipped right/bottom blocks. */
    assert(sky_bc_decode_region(130, 5, 5, source + 1, 35, 19, destination + 1, 112, 23));
    for (unsigned i = 0; i < sizeof destination; i++) {
        unsigned relative = i ? i - 1 : 0, y = relative / 23, x_byte = relative % 23;
        if (i && y < 5 && x_byte < 20)
            assert(destination[i] == colors[(y / 4) * 2 + (x_byte / 4) / 4][x_byte % 4]);
        else assert(destination[i] == 0xcd);
    }
    assert(!memcmp(source, original, sizeof source));
    unsigned char tiny[4];
    assert(sky_bc_decode_region(130, 1, 1, source + 1, 8, 8, tiny, sizeof tiny, 4));
    assert(!memcmp(tiny, colors[0], 4));

    memset(destination, 0xcd, sizeof destination);
    assert(!sky_bc_decode_region(130, 5, 5, source + 1, 34, 19, destination + 1, 112, 23));
    assert(!sky_bc_decode_region(130, 5, 5, source + 1, 35, 19, destination + 1, 111, 23));
    assert(!sky_bc_decode_region(130, 5, 5, source + 1, 35, 15, destination + 1, 112, 23));
    assert(!sky_bc_decode_region(130, 5, 5, source + 1, 35, 19, destination + 1, 112, 19));
    assert(!sky_bc_decode_region(130, 0, 5, source, sizeof source, 19, destination, sizeof destination, 23));
    assert(!sky_bc_decode_region(130, 5, 0, source, sizeof source, 19, destination, sizeof destination, 23));
    assert(!sky_bc_decode_region(136, 4, 4, source, sizeof source, 8, destination, sizeof destination, 16));
    assert(!sky_bc_decode_region(130, 4, 4, NULL, 8, 8, destination, sizeof destination, 16));
    assert(!sky_bc_decode_region(130, 4, 4, source, 8, 8, NULL, 64, 16));
    assert(!sky_bc_decode_region(130, 4, 8, source, SIZE_MAX, SIZE_MAX, destination, SIZE_MAX, 16));
    assert(!sky_bc_decode_region(150, UINT32_MAX, UINT32_MAX, source, SIZE_MAX, SIZE_MAX, destination, SIZE_MAX, SIZE_MAX));
    assert(!sky_bc_decode_region(130, 4, 4, destination, 8, 8, destination, 64, 16));
    for (unsigned i = 0; i < sizeof destination; i++) assert(destination[i] == 0xcd);
    assert(!memcmp(source, original, sizeof source));
}

int main(void) {
    test_formats();
    test_bc1();
    test_bc2_bc3();
    test_bc4_bc5();
    test_bc6();
    test_bc7();
    test_regions_and_bounds();
    puts("NVN_BC_CPU_OK formats=14 bc1-alpha bc2-bc3-alpha signed-normalized-bc4-bc5 hdr-bc6 bc7 srgb-bytes unaligned tiny-npot-strides truncation-overflow-overlap-no-write");
    return 0;
}
