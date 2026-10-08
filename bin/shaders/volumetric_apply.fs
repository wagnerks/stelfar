#version 450 core
#include "sfe.glsl"
layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D sceneHdr;
layout(SFE_SET(0, 1)) uniform sampler2D volumeLight;
layout(SFE_SET(0, 2)) uniform sampler2D sceneDepth;

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

// The march's own (volumetric.fs).
layout(SFE_SET(0, 15), std140) uniform PushConstants {
    vec3 viewPos;
    float density;
    vec3 lightDir;
    float anisotropy;
    vec3 lightColor;
    float intensity;
    vec2 resolution;
    int cascadeCount;
    float maxDistance;
    float cloudCoverage;
} params;

// The weight of a texel `o` texels past the lower of the two a pixel lies between, `f` of the way to the upper, along one
// axis: sfe_block_blur.glsl's kernel (half weight on the outer ring, so each sixteenth of the 4x4 block counts once)
// round each of the two, blended as bilinear filtering blends them. Every sixteenth still counts once wherever the pixel
// lies, so the block is averaged exactly and the upsampling is smooth.
float blockWeight(int o, float f) {
    float k0 = (o == -2 || o == 2) ? 0.5 : (o > -2 && o < 2 ? 1.0 : 0.0);
    float k1 = (o == -1 || o == 3) ? 0.5 : (o > -1 && o < 3 ? 1.0 : 0.0);
    return mix(k0, k1, f);
}

// The light in the haze is marched once for every 4x4 pixels (volumetric.fs), each texel of a 4x4 block of the march
// starting its ray at its own sixteenth of a step (sfeBlockNoise), and brought back to each pixel by the block blur round
// it: only from texels whose rays end about where this pixel's does. By the filter weights alone the haze far behind a
// trunk lay over its edge in a bright rim, and the trunk's clear air over the haze beside it.
void main() {
    vec3 scene = texture(sceneHdr, TexCoords).rgb;

    // This pixel's ray, as the march takes one: to what the pixel shows, no further than maxDistance, and that far into
    // the sky.
    vec3 surface = sfeScenePosition(sceneDepth, TexCoords, matrices.invProjection, matrices.invView);
    float rayLen = dot(surface, surface) > 1e-4 ? min(length(surface - params.viewPos), params.maxDistance) : params.maxDistance;

    ivec2 size = textureSize(volumeLight, 0);
    vec2 t = TexCoords * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(t));
    vec2 f = t - vec2(base);

    // A tenth of the ray apart is the same surface: the ground far off, seen at a slant, is up to that much further at
    // the next texel up the screen, and haze along a ray that much longer or shorter looks no different. The texel whose
    // ray ends nearest to this one's is all there is where none is that near: a twig thinner than a texel.
    float tolerance = 0.1 * rayLen + 0.1;
    vec2 sum = vec2(0.0);
    float wsum = 0.0;
    vec2 nearest = vec2(0.0, 1.0);
    float nearestOff = 1e30;
    for (int y = -2; y <= 3; ++y) {
        float wy = blockWeight(y, f.y);
        for (int x = -2; x <= 3; ++x) {
            vec3 texel = texelFetch(volumeLight, clamp(base + ivec2(x, y), ivec2(0), size - 1), 0).rgb;
            float off = abs(texel.b - rayLen) / tolerance;
            float w = wy * blockWeight(x, f.x) * exp(-off * off);
            sum += texel.rg * w;
            wsum += w;
            if (off < nearestOff) {
                nearestOff = off;
                nearest = texel.rg;
            }
        }
    }
    vec2 v = wsum > 1e-4 ? sum / wsum : nearest;
    FragColor = vec4(scene * v.y + params.lightColor * v.x, 1.0);
}
