// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct VertexInput {
    float4 position [[attribute(0)]];
    float4 color [[attribute(1)]];
    float4 mask [[attribute(2)]];
};

struct VertexOutput {
    float4 position [[position]];
    float4 color [[user(locn0)]];
};

struct Transform {
    float4 x;
    float4 y;
};

struct Arguments {
    constant Transform *transform [[id(0)]];
};

vertex VertexOutput main0(VertexInput input [[stage_in]],
                         constant Arguments &arguments [[buffer(0)]]) {
    float2 transformed(dot(input.position, arguments.transform->x),
                       dot(input.position, arguments.transform->y));
    return {float4(transformed, 0.0f, 1.0f), input.color * input.mask};
}
