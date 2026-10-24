// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct Support {
    float4 reserved[13];
    // The host initializes this support-buffer scalar at byte 208 to one.
    float unity;
};

struct CopyRegion {
    uint2 source_origin;
    uint2 extent;
    uint2 destination_origin;
};

struct Arguments {
    constant Support *support [[id(0)]];
    constant CopyRegion *region [[id(1)]];
    texture2d<float> source [[id(2)]];
    sampler source_sampler [[id(3)]];
    texture2d<float, access::write> destination [[id(4)]];
};

kernel void main0(constant Arguments &arguments [[buffer(0)]],
                  uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= arguments.region->extent)) return;
    float2 size(arguments.source.get_width(), arguments.source.get_height());
    float2 uv = (float2(pixel + arguments.region->source_origin) + 0.5f) / size;
    float4 color = arguments.source.sample(arguments.source_sampler, uv);
    arguments.destination.write(color * arguments.support->unity,
                                pixel + arguments.region->destination_origin);
}
