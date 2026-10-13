#version 450 core
#include "sfe.glsl"
layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D sceneHdr;
layout(SFE_SET(0, 1)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 2)) uniform sampler2D gMaterial;
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
// How much the surface scatters, as the G-buffer wrote it: a normal shortened by up to half.
layout(SFE_SET(0, 3)) uniform sampler2D gNormal;

layout(SFE_SET(0, 15), std140) uniform PushConstants {
    float strength;
    float radius;
    vec2 resolution;
} params;

const float kOff[7] = float[](-3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0);
const vec3 kW[7] = vec3[](
    vec3(0.020, 0.002, 0.000),
    vec3(0.070, 0.018, 0.004),
    vec3(0.170, 0.110, 0.045),
    vec3(0.400, 0.620, 0.780),
    vec3(0.170, 0.110, 0.045),
    vec3(0.070, 0.018, 0.004),
    vec3(0.020, 0.002, 0.000)
);

float scattersAt(vec2 uv) {
    return sfeScatters(texture(gNormal, uv).xyz, texture(gMaterial, uv).r);
}

// The light that goes in under the skin comes out again round where it went in, the red furthest: the picture spread
// over the skin by params.radius metres a tap, so a shadow's edge on it is as soft seen close as from afar. In pixels
// a tap it was a few millimetres close up, and the edge sharp. Taken only from what scatters too (not the shirt by
// the neck) and lies on the same surface (not the cheek behind an ear). The sun's shadow on it is spread as far
// (sfe_sun_shadow.glsl): this spreads the colour, which keeps most of its blue and green where it is.
void main() {
    vec3 center = texture(sceneHdr, TexCoords).rgb;
    // The mask first: most of a frame does not scatter, and the position costs a matrix product.
    float mask = scattersAt(TexCoords);
    if (mask < 0.01 || params.strength < 0.001) {
        FragColor = vec4(center, 1.0);
        return;
    }
    vec3 world = sfeScenePosition(sceneDepth, TexCoords, matrices.invProjection, matrices.invView);
    if (dot(world, world) < 1e-6) {
        FragColor = vec4(center, 1.0);
        return;
    }

    // A tap's metres across the screen here, in UV.
    float depth = max(abs((matrices.view * vec4(world, 1.0)).z), 1e-3);
    vec2 texel = params.radius * 0.5 * abs(vec2(matrices.projection[0][0], matrices.projection[1][1])) / depth;
    float reach = 4.0 * params.radius;

    vec3 acc = vec3(0.0);
    vec3 wsum = vec3(0.0);
    for (int i = 0; i < 7; ++i) {
        vec2 uvH = TexCoords + vec2(kOff[i], 0.0) * texel;
        vec2 uvV = TexCoords + vec2(0.0, kOff[i]) * texel;
        vec3 pH = sfeScenePosition(sceneDepth, uvH, matrices.invProjection, matrices.invView);
        vec3 pV = sfeScenePosition(sceneDepth, uvV, matrices.invProjection, matrices.invView);
        float aH = exp(-dot(pH - world, pH - world) / (reach * reach)) * scattersAt(uvH);
        float aV = exp(-dot(pV - world, pV - world) / (reach * reach)) * scattersAt(uvV);
        vec3 wH = kW[i] * aH;
        vec3 wV = kW[i] * aV;
        acc += texture(sceneHdr, uvH).rgb * wH + texture(sceneHdr, uvV).rgb * wV;
        wsum += wH + wV;
    }
    vec3 blurred = acc / max(wsum, vec3(1e-4));
    vec3 outc = mix(center, blurred, clamp(mask * params.strength, 0.0, 1.0));
    FragColor = vec4(outc, 1.0);
}
