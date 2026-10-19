#version 450 core
#include "sfe.glsl"
// A mote or a spark (particle_sprite.vs): round and soft-edged, fading out where it meets what is behind it rather than
// cut off along it.
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vLight;
layout(location = 2) in float vCover;
layout(location = 3) in float vDepth;
layout(location = 4) in float vSoft;
layout(location = 5) in vec4 vScreen;

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
layout(SFE_SET(0, 4)) uniform sampler2D sceneDepth;

layout(location = 0) out vec4 FragColor;

void main() {
    float shape = 1.0 - smoothstep(0.25, 1.0, length(vUv));
    bool sky;
    vec2 uv = vScreen.xy / vScreen.w * 0.5 + 0.5;
    vec3 behind = sfeSceneViewPosition(sceneDepth, uv, matrices.invProjection, sky);
    float room = sky ? 1e6 : -behind.z - vDepth;
    shape *= clamp(room / vSoft, 0.0, 1.0);
    FragColor = vec4(vLight * shape, vCover * shape);
}
