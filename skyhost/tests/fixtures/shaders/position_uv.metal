// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct VertexInput {
    float4 position [[attribute(0)]];
    float4 uv [[attribute(1)]];
};

struct VertexOutput {
    float4 position [[position]];
    float2 uv [[user(locn1)]];
};

vertex VertexOutput main0(VertexInput input [[stage_in]]) {
    return {float4(input.position.xyz, 1.0f), input.uv.xy};
}
