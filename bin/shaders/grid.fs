#version 450 core
#include "sfe.glsl"
layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec2 TexCoords;
layout(location = 2) in vec3 Normal;
layout(location = 3) in vec3 ViewPos;
layout(location = 4) in vec4 vClip;
layout(location = 5) in vec4 vPrevClip;

layout(location = 0) out vec4 gMaterial;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;
layout(location = 3) out vec4 gVelocity;

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

SFE_PUSH_BLOCK PushConstants {
    vec4 cameraPos;
    float farPlane;
    float nearPlane;
} pc;

vec4 grid(vec3 fragPos3D, float scale) {
    vec2 coord = fragPos3D.xz * scale;
    vec2 derivative = fwidth(coord);
    vec2 gridLines = abs(fract(coord - 0.5) - 0.5) / derivative;
    float line = min(gridLines.x, gridLines.y);
    float minimumz = min(derivative.y, 1.0);
    float minimumx = min(derivative.x, 1.0);
    vec4 color = vec4(0.2, 0.2, 0.2, 1.2 - min(line, 1.0));
    if (fragPos3D.x > -0.5 * minimumx && fragPos3D.x < 0.5 * minimumx) {
        color.z = 1.0;
        color.a = 1.0;
    }
    if (fragPos3D.z > -0.5 * minimumz && fragPos3D.z < 0.5 * minimumz) {
        color.x = 1.0;
        color.a = 1.0;
    }
    return color;
}

float computeLinearDepth(vec3 pos) {
    vec4 clipSpacePos = matrices.PV * vec4(pos.xyz, 1.0);
    float clipSpaceDepth = (clipSpacePos.z / clipSpacePos.w) * 2.0 - 1.0;
    float linearDepth = (2.0 * pc.nearPlane * pc.farPlane) / (pc.farPlane + pc.nearPlane - clipSpaceDepth * (pc.farPlane - pc.nearPlane));
    return linearDepth / pc.farPlane;
}

void main() {
    gMaterial = sfeGBufferMaterial(0.0, 1.0, 0.0, 0.04);
    gNormal.xyz = vec3(0.0, 1.0, 0.0);
    gNormal.a = gl_FragCoord.z / gl_FragCoord.w;

    float distanceKoef = min(abs(pc.cameraPos.y), pc.farPlane) / pc.farPlane;

    vec4 color1 = grid(FragPos.xyz, 1.0);
    color1.a *= 1.0 - distanceKoef * 40.0;
    color1.a *= 0.8;

    vec4 color2 = grid(FragPos.xyz, 0.1);
    color2.a *= 1.0 - distanceKoef * 10.0;

    vec4 color3 = grid(FragPos.xyz, 0.01);

    float linearDepth = computeLinearDepth(FragPos.xyz);
    float fading = max(0.0, 0.8 - linearDepth);
    float lines = clamp(max(color1.a, max(color2.a, color3.a)), 0.0, 1.0);
    gAlbedoSpec.rgb = mix(vec3(0.12, 0.12, 0.13), vec3(0.04), lines * 0.7);
    gAlbedoSpec.rgb *= vec3(fading);
    gAlbedoSpec.a = 0.28;
    gVelocity = sfeGBufferVelocity(vClip, vPrevClip, 0.0);
}
