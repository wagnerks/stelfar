#ifndef SFE_PARTICLE_GLSL
#define SFE_PARTICLE_GLSL
// One particle (Particles::GpuParticle, renderModule/Particles.h): moved in particles.cs, drawn by particle_chip.vs (chips),
// particle_liquid.vs (drops) and particle_sprite.vs.
struct SfeParticle {
    vec4 posAge;  // xyz where, w seconds since it was let go
    vec4 velLife; // xyz how it goes, w how long it lives: 0 for none
    vec4 colour;  // rgb albedo, linear (a chip, a mote) or light (a spark); a how much of what is behind a mote it hides
    vec4 shape;   // x size, metres across; y metres a second it grows; z its look; w a random number of its own
    vec4 motion;  // x the share of gravity on it, y how quickly it takes the wind's speed, z what it keeps of its speed
                  // into a surface, w the height it is gone below
    vec4 liquid;  // a drop's: x how much of the light through it its liquid stops, of it a metre; y how far round it
                  // gathers with others, of its size; z how rough its face is, 0 a mirror; w how thick it spreads
                  // out to lying, of its size
    vec4 lying;   // a drop come to rest (particles.cs): xyz which way what it lies on faces, w its age then; let go to
                  // take its colour from the picture (Burst::colourOf): xyz where, w minus the share of it it is
};

const float kSfeParticleChip = 0.0;
const float kSfeParticleMote = 1.0;
const float kSfeParticleSpark = 2.0;
const float kSfeParticleDrop = 3.0;

bool sfeParticleIs(SfeParticle p, float look) {
    return abs(p.shape.z - look) < 0.5;
}

// A chip or a drop: a piece of something, settling where it lands and shrinking away at its end; a chip drawn into the
// G-buffer, a drop as the liquid it is (particle_liquid.vs).
bool sfeParticleIsPiece(SfeParticle p) {
    return sfeParticleIs(p, kSfeParticleChip) || sfeParticleIs(p, kSfeParticleDrop);
}

// Whether it is on its way, not come to rest (particles.cs stills what rests).
bool sfeParticleFlying(SfeParticle p) {
    return dot(p.velLife.xyz, p.velLife.xyz) > 0.0;
}

// How far through its life it is, 0 let go to 1 gone.
float sfeParticleSpent(SfeParticle p) {
    return clamp(p.posAge.w / max(p.velLife.w, 1e-4), 0.0, 1.0);
}

// How big it is now, metres across: grown so far, and a piece shrinking away over the last fifth of its life.
float sfeParticleSize(SfeParticle p) {
    float size = p.shape.x + p.shape.y * p.posAge.w;
    if (sfeParticleIsPiece(p)) {
        size *= 1.0 - smoothstep(0.8, 1.0, sfeParticleSpent(p));
    }
    return max(size, 0.0);
}

// The vertex of a particle not drawn by this pass: off every side of the picture.
const vec4 kSfeParticleHidden = vec4(2.0, 2.0, 2.0, 1.0);
#endif
