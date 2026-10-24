// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct Arguments {
    constant float4 *color [[id(0)]];
};

fragment float4 main0(constant Arguments &arguments [[buffer(0)]]) {
    return *arguments.color;
}
