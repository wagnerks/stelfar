#version 450 core
#include "sfe.glsl"
#include "sfe_particle.glsl"
// A chip (ParticlePass): a small piece facing the eye, tumbling as it flies -- its face turned about an axis of its own,
// so the light catches it and leaves it -- and lying still once it rests. A drop too: facing the eye, drawn out along
// its way across the picture as it flies, its face a ball's (particle_chip.fs). Every other particle is not drawn here.
layout(location = 0) in vec2 aPos;

layout(SFE_SET(0, 1), std430) readonly buffer SfeParticles {
    SfeParticle particles[];
};
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(location = 0) out vec2 vUv;
layout(location = 1) flat out vec3 vAlbedo;
layout(location = 2) flat out vec3 vNormal;
layout(location = 3) flat out float vSeed;
layout(location = 4) out vec4 vClip;
layout(location = 5) out vec4 vPrevClip;
layout(location = 6) flat out vec3 vAcross; // a drop's face: across its way, along it, and towards the eye
layout(location = 7) flat out vec3 vAlong;
layout(location = 8) flat out vec3 vToEye;
layout(location = 9) flat out float vDrop;  // 1 a drop, 0 a chip

// Turns a second of a flying chip.
const float kSpin = 2.5;
// How much longer than wide a drop is drawn for each metre a second it goes across the picture, and at most.
const float kDrawnOut = 0.12;
const float kMostDrawnOut = 1.2;
// A drop come to rest: spread wider and as much lower, a splat on what it landed on.
const float kSplatWide = 1.5;
const float kSplatLow = 0.45;

void main() {
    SfeParticle p = particles[SFE_INSTANCE_ID];
    float size = sfeParticleSize(p);
    if (p.velLife.w <= 0.0 || !sfeParticleIsPiece(p) || size <= 0.0) {
        gl_Position = kSfeParticleHidden;
        return;
    }
    vec3 right = matrices.invView[0].xyz;
    vec3 up = matrices.invView[1].xyz;
    vec3 toEye = normalize(matrices.invView[3].xyz - p.posAge.xyz);
    float seed = p.shape.w;
    vDrop = sfeParticleIs(p, kSfeParticleDrop) ? 1.0 : 0.0;
    // A drop: its quad's long side along its way across the picture, as much longer as it goes fast.
    vec3 across = right;
    vec3 along = up;
    float wide = 1.0;
    float drawnOut = 1.0;
    float sunk = 0.0;
    vec3 seen = p.velLife.xyz - toEye * dot(p.velLife.xyz, toEye);
    if (vDrop > 0.5 && dot(seen, seen) > 1e-6) {
        along = normalize(seen);
        across = cross(along, toEye);
        drawnOut += min(length(seen) * kDrawnOut, kMostDrawnOut);
    }
    else if (vDrop > 0.5 && dot(p.velLife.xyz, p.velLife.xyz) == 0.0) {
        wide = kSplatWide;
        drawnOut = kSplatLow;
        sunk = (1.0 - kSplatLow) * 0.5;
    }
    vAcross = across;
    vAlong = along;
    vToEye = toEye;
    // Its face: towards the eye, turned off it about its own axis in the picture's plane, round as it flies.
    float moving = dot(p.velLife.xyz, p.velLife.xyz) > 0.0 ? 1.0 : 0.0;
    float turn = seed * 6.2832 + moving * p.posAge.w * kSpin * 6.2832;
    float axis = seed * 37.0;
    vec3 aside = right * cos(axis) + up * sin(axis);
    vec3 n = normalize(toEye * cos(turn) * 0.7 + aside * sin(turn) + toEye * 0.3);
    vNormal = dot(n, toEye) < 0.0 ? -n : n;

    vec3 world = p.posAge.xyz + (across * aPos.x * wide + along * (aPos.y * drawnOut - sunk)) * size * 0.5;
    gl_Position = matrices.PV * vec4(world, 1.0);
    // Where it was last frame, for the motion: back along its way by the frame's time.
    float frame = max(matrices.wind.w - matrices.windBefore.w, 0.0);
    vClip = matrices.PVNoJitter * vec4(world, 1.0);
    vPrevClip = matrices.prevPVNoJitter * vec4(world - p.velLife.xyz * frame, 1.0);
    vUv = aPos;
    vAlbedo = p.colour.rgb;
    vSeed = seed;
}
