// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
};

vertex VertexOutput main0(uint index [[vertex_id]]) {
    constexpr float2 corners[] = {
        {-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f},
    };
    return {float4(corners[index], 0.0f, 1.0f)};
}
