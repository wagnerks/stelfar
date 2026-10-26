#version 450 core
#include "sfe.glsl"
// A chip into the G-buffer (ParticlePass): a ragged scrap rather than a square, of its own colour, matte, lit as the
// scene is.
layout(location = 0) in vec2 vUv;
layout(location = 1) flat in vec3 vAlbedo;
layout(location = 2) flat in vec3 vNormal;
layout(location = 3) flat in float vSeed;
layout(location = 4) in vec4 vClip;
layout(location = 5) in vec4 vPrevClip;

layout(location = 0) out vec4 gMaterial;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;
layout(location = 3) out vec4 gVelocity;
layout(location = 4) out vec4 gEmissive;

void main() {
    // Its outline: a few lobes round the middle, each chip's its own.
    float angle = atan(vUv.y, vUv.x);
    float edge = 0.72 + 0.18 * sin(angle * 3.0 + vSeed * 40.0) + 0.1 * sin(angle * 5.0 + vSeed * 90.0);
    if (length(vUv) > edge) {
        discard;
    }
    gMaterial = sfeGBufferMaterial(0.0, 0.5, 0.0, 0.04);
    gNormal = vec4(vNormal, gl_FragCoord.z / gl_FragCoord.w);
    gAlbedoSpec = vec4(vAlbedo, 0.55);
    gVelocity = sfeGBufferVelocity(vClip, vPrevClip, 0.0);
    gEmissive = vec4(0.0);
}
