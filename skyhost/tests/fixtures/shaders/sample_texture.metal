// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct FragmentInput {
    float2 uv [[user(locn1)]];
};

struct Arguments {
    texture2d<float> image [[id(0)]];
    sampler image_sampler [[id(1)]];
};

fragment float4 main0(FragmentInput input [[stage_in]],
                     constant Arguments &arguments [[buffer(0)]]) {
    return arguments.image.sample(arguments.image_sampler, input.uv);
}
