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

// The light in the haze is marched once for every 4x4 pixels (volumetric.fs) and brought back to each pixel from the
// four texels round it: by their bilinear weights, but only those whose rays end about where this pixel's does. By the
// bilinear weights alone the haze far behind a trunk lay over its edge in a bright rim, and the trunk's clear air over
// the haze beside it.
void main() {
    vec3 scene = texture(sceneHdr, TexCoords).rgb;

    // This pixel's ray, as the march takes one: to what the pixel shows, no further than maxDistance, and that far into
    // the sky.
    vec3 surface = sfeScenePosition(sceneDepth, TexCoords, matrices.invProjection, matrices.invView);
    float rayLen = dot(surface, surface) > 1e-4 ? min(length(surface - params.viewPos), params.maxDistance) : params.maxDistance;

    vec2 texels = vec2(textureSize(volumeLight, 0));
    vec2 t = TexCoords * texels - 0.5;
    vec2 base = floor(t);
    vec2 f = t - base;
    vec2 uv = (base + 1.0) / texels;
    vec4 light = textureGather(volumeLight, uv, 0); // (0,1) (1,1) (1,0) (0,0)
    vec4 through = textureGather(volumeLight, uv, 1);
    vec4 rayLens = textureGather(volumeLight, uv, 2);

    // A tenth of the ray apart is the same surface: the ground far off, seen at a slant, is up to that much further at
    // the next texel up the screen, and haze along a ray that much longer or shorter looks no different. A little of the
    // texel whose ray ends nearest to this one's is all there is where none is that near: a twig thinner than a texel.
    vec4 off = abs(rayLens - rayLen) / (0.1 * rayLen + 0.1);
    vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y)) * exp(-off * off);
    w += 1e-4 * step(off, vec4(min(min(off.x, off.y), min(off.z, off.w))));
    w /= dot(w, vec4(1.0));
    FragColor = vec4(scene * dot(through, w) + params.lightColor * dot(light, w), 1.0);
}
