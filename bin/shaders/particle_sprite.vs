#version 450 core
#include "sfe.glsl"
#include "sfe_particle.glsl"
#include "sfe_sky.glsl"
// A mote or a spark over the picture after TAA (ParticleSpritePass), as AirLifePass's specks are. A mote: a soft puff
// facing the eye, lit by the sun where the shadows let it and by the sky, thinning out as it grows. A spark: its own
// light, streaked back along its way and dying down. Chips are not drawn here.
layout(location = 0) in vec2 aPos;

layout(SFE_SET(0, 1), std430) readonly buffer SfeParticles {
    SfeParticle particles[];
};
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
layout(SFE_SET(0, 8)) uniform sampler2DArrayShadow shadowMap;
layout(SFE_SET(0, 11), std140) uniform LightSpaceMatrices {
    mat4 lightSpaceMatrices[6];
};
layout(SFE_SET(0, 15), std140) uniform SpriteBlock {
    SFE_SKY_PARAMS
    vec4 view; // xy the render size, z shadow cascades, w the sun's intensity setting
} sprite;
#include "sfe_sun_visible.glsl"

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vLight;  // added, premultiplied
layout(location = 2) out float vCover; // how much of what is behind it it hides
layout(location = 3) out float vDepth; // its distance along the view, for fading where it meets what is behind
layout(location = 4) out float vSoft;  // over how far it fades there, metres
layout(location = 5) out vec4 vScreen; // where it is in the picture, for what is behind it there

// How far back along its way a spark is streaked: its way over this many seconds.
const float kStreak = 0.025;

void main() {
    SfeParticle p = particles[SFE_INSTANCE_ID];
    bool mote = sfeParticleIs(p, kSfeParticleMote);
    bool spark = sfeParticleIs(p, kSfeParticleSpark);
    float size = sfeParticleSize(p);
    if (p.velLife.w <= 0.0 || !(mote || spark) || size <= 0.0) {
        gl_Position = kSfeParticleHidden;
        return;
    }
    vec3 eye = matrices.invView[3].xyz;
    vec3 at = p.posAge.xyz;
    vec3 toEye = normalize(eye - at);
    vec3 right = matrices.invView[0].xyz;
    vec3 up = matrices.invView[1].xyz;
    vec3 world;
    if (spark && dot(p.velLife.xyz, p.velLife.xyz) > 1e-4) {
        // Along its way and across it, square to the eye: a short bright line.
        vec3 along = p.velLife.xyz * kStreak;
        vec3 side = normalize(cross(along, toEye)) * size * 0.5;
        world = at + side * aPos.x - along * (aPos.y * 0.5 + 0.5) + normalize(along) * size * 0.5 * aPos.y;
    }
    else {
        world = at + (right * aPos.x + up * aPos.y) * size * 0.5;
    }
    vec4 clip = matrices.PVNoJitter * vec4(world, 1.0);
    if (clip.w <= 0.05) {
        gl_Position = kSfeParticleHidden;
        return;
    }
    gl_Position = clip;
    vScreen = clip;

    float spent = sfeParticleSpent(p);
    if (spark) {
        // Flaring and dimming as it burns down.
        float flicker = 0.75 + 0.25 * sin(p.posAge.w * 40.0 + p.shape.w * 60.0);
        vLight = p.colour.rgb * flicker * (1.0 - spent);
        vCover = 0.0;
    }
    else {
        // Coming in quickly, then thinning out as it grows.
        float amount = p.colour.a * smoothstep(0.0, 0.12, spent) * (1.0 - spent) * (1.0 - spent);
        float sun = sfeSunVisible(at, int(sprite.view.z + 0.5));
        vec3 sunLight = sprite.sunColor.rgb * sprite.view.w;
        vLight = p.colour.rgb * (sunLight * sun * 0.5 + sprite.sunColor.rgb * 0.05 * sprite.cameraPos.w) * amount;
        vCover = amount;
    }
    vUv = aPos;
    vDepth = -(matrices.view * vec4(at, 1.0)).z;
    vSoft = max(size * 0.5, 0.02);
}
