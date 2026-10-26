#version 450 core
#include "sfe.glsl"
#include "sfe_particle.glsl"
#include "sfe_liquid.glsl"
// A drop laid into its liquid's field (ParticleLiquidPass): its own body an ellipsoid, and round it as far as its liquid
// gathers (liquid.y, of its own size) the ball of its field, found for each pixel along the eye's ray through it
// (particle_liquid.fs). In flight a ball, drawn out along its way across the picture as far as it goes while a picture
// is taken, as much as its liquid holds together, so the drops of water let go one after another run together as a
// flow does and lumps of mud stay lumps; come to rest spreading out over what it lies on, so the drops lying by each
// other run together into a puddle. Here only a square facing the eye round all of its ball, where the G-buffer has it
// (jittered as it is); and how it moves across the picture. Every other particle is not drawn here.
layout(location = 0) in vec2 aPos;

layout(SFE_SET(0, 1), std430) readonly buffer SfeParticles {
    SfeParticle particles[];
};
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(location = 0) flat out vec4 vLiquid; // rgb its colour, a how much of the light through it its liquid stops a metre
layout(location = 1) flat out vec4 vShape;  // x how far its ball reaches, of its body; y its field's falloff; z its own
                                            // radius; w how rough its face is
layout(location = 2) flat out vec3 vMiddle; // its middle, in view space
layout(location = 3) flat out vec3 vRow0;   // its body's axes in view space, each over its half-length: what takes a
layout(location = 4) flat out vec3 vRow1;   // point of view space, from its middle, into a ball of radius 1
layout(location = 5) flat out vec3 vRow2;
layout(location = 6) out vec4 vScreen;      // where it is in the picture
layout(location = 7) flat out vec2 vMoving; // how its middle moves across the picture (sfeVelocity)

// How long a picture takes, seconds: a drop of a liquid that gathers from kStrung of its size round it or further is
// drawn over as far as it goes meanwhile, at most kMostDrawnOut times its own length more; one gathering from less, as
// much less as it is less past its own edge.
const float kExposure = 1.0 / 30.0;
const float kMostDrawnOut = 8.0;
const float kStrung = 2.0;
// A drop come to rest spreads out over what it lies on within about so many seconds.
const float kSettling = 0.3;
// The least a ball reaches round a drop, of its body: past it, the field holds none.
const float kLeastReach = 1.05;

void main() {
    SfeParticle p = particles[SFE_INSTANCE_ID];
    float size = sfeParticleSize(p);
    if (p.velLife.w <= 0.0 || !sfeParticleIs(p, kSfeParticleDrop) || size <= 0.0) {
        gl_Position = kSfeParticleHidden;
        return;
    }
    float own = 0.5 * size;
    float reach = max(p.liquid.y, kLeastReach);
    vec3 toEye = normalize(matrices.invView[3].xyz - p.posAge.xyz);
    vec3 across = matrices.invView[0].xyz;
    vec3 along = matrices.invView[1].xyz;
    // Its body's three axes and half-lengths, and the square's half-sides across and along the picture.
    vec3 axis0;
    vec3 axis1;
    vec3 axis2;
    vec3 extent;
    vec2 square;
    if (sfeParticleFlying(p)) {
        float drawnOut = 1.0;
        vec3 seen = p.velLife.xyz - toEye * dot(p.velLife.xyz, toEye);
        if (dot(seen, seen) > 1e-6) {
            along = normalize(seen);
            across = normalize(cross(along, toEye));
            drawnOut += min(length(seen) * kExposure / size, kMostDrawnOut) * clamp((reach - 1.0) / (kStrung - 1.0), 0.0, 1.0);
        }
        axis0 = across;
        axis1 = along;
        axis2 = toEye;
        extent = vec3(own, own * drawnOut, own);
        square = vec2(own, own * drawnOut);
    }
    else {
        // Come to rest on what faces `up` (particles.cs): from the ball it fell as down to as thin as its liquid spreads
        // out (liquid.w, of its size), as much wider as holds as much of it.
        vec3 up = dot(p.lying.xyz, p.lying.xyz) > 1e-6 ? normalize(p.lying.xyz) : vec3(0.0, 1.0, 0.0);
        float settled = 1.0 - exp(-max(p.posAge.w - p.lying.w, 0.0) / kSettling);
        float thick = own * mix(1.0, 2.0 * p.liquid.w, settled);
        float wide = own * sqrt(own / max(thick, 1e-5));
        vec3 level = cross(up, toEye);
        across = dot(level, level) > 1e-6 ? normalize(level) : across;
        along = normalize(cross(toEye, across));
        axis0 = across;
        axis1 = normalize(cross(across, up));
        axis2 = up;
        extent = vec3(wide, wide, thick);
        square = vec2(wide);
    }
    // A little over the ball's reach, for the perspective.
    square *= reach * 1.1;
    vec3 world = p.posAge.xyz + across * aPos.x * square.x + along * aPos.y * square.y;
    vec4 clip = matrices.PV * vec4(world, 1.0);
    if (clip.w <= 0.05) {
        gl_Position = kSfeParticleHidden;
        return;
    }
    gl_Position = clip;
    vScreen = clip;
    // Where its middle was the last frame: back along its way by the frame's time.
    float frame = max(matrices.wind.w - matrices.windBefore.w, 0.0);
    vMoving = sfeVelocity(matrices.PVNoJitter * vec4(p.posAge.xyz, 1.0), matrices.prevPVNoJitter * vec4(p.posAge.xyz - p.velLife.xyz * frame, 1.0));
    vLiquid = vec4(p.colour.rgb, p.liquid.x);
    // The field (1 - r^2)^n of a ball's: n such that it holds kSfeLiquidSurface at the drop's body, so a drop on its
    // own is its own size however far its liquid reaches. Reaching further, it falls
    // off as a bell, so drops near each other run together smoothly; reaching hardly past the drop, it holds up to near
    // its edge and drops away there, so they only join where they touch, in lumps.
    vShape = vec4(reach, log(kSfeLiquidSurface) / log(1.0 - 1.0 / (reach * reach)), own, p.liquid.z);
    vMiddle = (matrices.view * vec4(p.posAge.xyz, 1.0)).xyz;
    mat3 toView = mat3(matrices.view);
    vRow0 = toView * axis0 / extent.x;
    vRow1 = toView * axis1 / extent.y;
    vRow2 = toView * axis2 / extent.z;
}
