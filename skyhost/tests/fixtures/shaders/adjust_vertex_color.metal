// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct FragmentInput {
    float4 color [[user(locn0)]];
};

struct ColorAdjustment {
    float4 bias;
    float4 scale;
};

struct Arguments {
    constant ColorAdjustment *adjustment [[id(0)]];
};

fragment float4 main0(FragmentInput input [[stage_in]],
                     constant Arguments &arguments [[buffer(0)]]) {
    return input.color * arguments.adjustment->scale + arguments.adjustment->bias;
}
