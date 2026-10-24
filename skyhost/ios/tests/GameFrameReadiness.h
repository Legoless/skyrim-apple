#ifndef SKY_GAME_FRAME_READINESS_H
#define SKY_GAME_FRAME_READINESS_H

#include <CoreGraphics/CoreGraphics.h>
#include <TargetConditionals.h>
#include <stdint.h>

/* Test-only blank-frame guard, not a game/menu classifier. Sample the centre
 * of the aspect-fit 16:9 game viewport, excluding letterboxing and the native
 * controls at the screen edges. Screenshots still require visual review. */
static inline int sky_smoke_frame_has_scene_content(CGImageRef image) {
    if (!image) return 0;
    const size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
    if (width <= height || height < 2) return 0;
    CGFloat scale = (CGFloat)width / 16.0;
    if ((CGFloat)height / 9.0 < scale) scale = (CGFloat)height / 9.0;
    CGRect viewport = CGRectMake(((CGFloat)width - scale * 16.0) * .5,
                                 ((CGFloat)height - scale * 9.0) * .5,
                                 scale * 16.0, scale * 9.0);
    CGRect centre = CGRectInset(viewport, viewport.size.width * .2, viewport.size.height * .2);
    CGImageRef cropped = CGImageCreateWithImageInRect(image, CGRectIntegral(centre));
    if (!cropped) return 0;
    enum { SAMPLE_WIDTH = 64, SAMPLE_HEIGHT = 36, SAMPLE_COUNT = SAMPLE_WIDTH * SAMPLE_HEIGHT };
    uint8_t pixels[SAMPLE_COUNT * 4] = {0};
    CGColorSpaceRef colour = CGColorSpaceCreateDeviceRGB();
    if (!colour) { CGImageRelease(cropped); return 0; }
    CGContextRef context = CGBitmapContextCreate(pixels, SAMPLE_WIDTH, SAMPLE_HEIGHT, 8,
        SAMPLE_WIDTH * 4, colour, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(colour);
    if (!context) { CGImageRelease(cropped); return 0; }
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextSetInterpolationQuality(context, kCGInterpolationLow);
    CGContextDrawImage(context, CGRectMake(0, 0, SAMPLE_WIDTH, SAMPLE_HEIGHT), cropped);
    CGContextRelease(context);
    CGImageRelease(cropped);

    uint64_t sums[3] = {0}, squares[3] = {0}, visible = 0;
    for (size_t i = 0; i < SAMPLE_COUNT; i++) {
        int nonblack = 0;
        for (size_t channel = 0; channel < 3; channel++) {
            const uint64_t value = pixels[i * 4 + channel];
            sums[channel] += value;
            squares[channel] += value * value;
            if (value > 12) nonblack = 1;
        }
        visible += nonblack;
    }
    uint64_t variation = 0;
    for (size_t channel = 0; channel < 3; channel++)
        variation += SAMPLE_COUNT * squares[channel] - sums[channel] * sums[channel];
    /* At least 15% nonblack samples and nontrivial RGB variance. This rejects
     * the observed black loading/fade frames and uniform white startup frame,
     * without measuring frame rate or requiring a fixed startup delay. */
    return visible * 20 >= SAMPLE_COUNT * 3 &&
        variation >= (uint64_t)SAMPLE_COUNT * SAMPLE_COUNT * 108;
}

#if defined(__OBJC__) && TARGET_OS_IPHONE
#import <UIKit/UIKit.h>

static inline UIImage *sky_smoke_normalized_image(UIImage *image) {
    if (!image || image.imageOrientation == UIImageOrientationUp) return image;
    UIGraphicsBeginImageContextWithOptions(image.size, YES, image.scale);
    [image drawInRect:CGRectMake(0, 0, image.size.width, image.size.height)];
    UIImage *normalized = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return normalized;
}

static inline int sky_smoke_image_has_scene_content(UIImage *image) {
    return sky_smoke_frame_has_scene_content(sky_smoke_normalized_image(image).CGImage);
}
#endif

#endif
