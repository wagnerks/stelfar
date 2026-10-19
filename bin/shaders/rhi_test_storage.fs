#version 450 core
#include "sfe.glsl"

layout(location = 0) out vec4 fragColor;

// std430, which is what every storage buffer in the engine's own shaders uses (g_buffer.vs,
// pointLightMap.vs, animateBones.cs). It matters here more than in the uniform tests: under
// std140 an array of float has a stride of 16 bytes, so the same C++ array would be read every
// fourth element and the values below would come out wrong rather than merely differently packed.
// An unsized array of plain float rather than vec4 is the case that can tell the two apart.
layout(SFE_SET(0, 0), std430) readonly buffer TestStorageBlock {
    float values[];
} block;

layout(location = 0) in vec2 vUv;

void main() {
    // vUv, not gl_FragCoord: the two backends put gl_FragCoord's origin in different corners, and
    // this test is about the buffer, not about that. vUv is a straight function of clip position,
    // whose orientation the convention tests have already pinned. At the pixel centres of a 4x4
    // target vUv is 0.125/0.375/0.625/0.875, so col and row land in the middle of a cell and no
    // rounding decides which element is read.
    int col = int(vUv.x * 4.0);
    int row = int(vUv.y * 4.0);
    int idx = row * 4 + col;

    // Two reads, at mirrored indices, so red and green disagree on every pixel: an SSBO that
    // handed back one element for every index, or a backend that swizzled the channels, cannot
    // produce this pair by accident.
    fragColor = vec4(block.values[idx], block.values[15 - idx], 0.0, 1.0);
}
