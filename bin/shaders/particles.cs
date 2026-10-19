#version 450 core
#include "sfe.glsl"
#include "sfe_particle.glsl"
// Every particle a step on (ParticlePass): those let go this frame copied in where the oldest were, the rest aged,
// pulled by gravity, carried by the wind, and bounced off what the picture shows where they get to -- its depth and
// its normals, so anything drawn is ground to them, and nothing out of the picture is. A chip that comes to rest on
// something flat enough stays there, still, while it lives. A drop that sticks striking a catcher's shape (a vehicle,
// Particles::shape) is caught there, out of the picture or in it: gone, and told back (SfeCaught).
layout(local_size_x = 64) in;

layout(SFE_SET(0, 1), std430) buffer SfeParticles {
    SfeParticle particles[];
};
layout(SFE_SET(0, 2), std140) uniform SfeSpawned {
    SfeParticle spawned[512]; // Particles::kMaxSpawn
};
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
layout(SFE_SET(0, 6)) uniform sampler2D sceneDepth; // the G-buffer's
layout(SFE_SET(0, 7)) uniform sampler2D gNormal;
// The catchers (Particles::GpuCatcher): the world into each one's frame, its low corner and cells' metres (w), its
// cells across and where its shape starts in `shapes` (w); a bit a cell, x fastest, then z, then y.
struct SfeCatcher {
    mat4 toLocal;
    vec4 low;
    uvec4 cells;
};
layout(SFE_SET(0, 3), std430) readonly buffer SfeShapes {
    uint shapes[];
};
layout(SFE_SET(0, 4), std140) uniform SfeCatchers {
    SfeCatcher catchers[8]; // Particles::kCatchers
};
// The drops caught, counting up for good, the newest kMaxCaught of them round the ring: two each, xyz where in the
// catcher's frame it struck and w its litres, then x which catcher.
layout(SFE_SET(0, 8), std430) buffer SfeCaught {
    uint caughtCount;
    uint caughtPad0;
    uint caughtPad1;
    uint caughtPad2;
    vec4 caught[];
};

SFE_PUSH_BLOCK Push {
    float dt;
    uint start;      // where the first of those let go this frame goes
    uint count;      // how many were let go
    float windSpeed; // metres a second of a wind of strength 1
    uint catchers;   // how many of `catchers` to try
} pc;

// What a chip is still at, metres a second, on something this flat (its normal's up) or flatter: at rest.
const float kRestSpeed = 0.5;
const float kRestFlat = 0.5;
// What a sliding particle keeps of its speed along a surface it hits.
const float kSlide = 0.8;
// How far behind what the picture shows a particle may be and still be taken for in it, metres, besides its step: any
// further, it is behind it, not in it.
const float kThickness = 0.3;

// Whether going from `from` to `to` strikes catcher `c`'s shape, and where in its frame first: tried every half a
// cell along the way, so a fast drop does not go through a thin wall.
bool sfeStrikes(uint c, vec3 from, vec3 to, out vec3 struck) {
    SfeCatcher k = catchers[c];
    vec3 a = (k.toLocal * vec4(from, 1.0)).xyz;
    vec3 b = (k.toLocal * vec4(to, 1.0)).xyz;
    vec3 high = k.low.xyz + vec3(k.cells.xyz) * k.low.w;
    struck = b;
    if (k.low.w <= 0.0 || any(lessThan(max(a, b), k.low.xyz)) || any(greaterThan(min(a, b), high))) {
        return false;
    }
    int steps = clamp(int(ceil(length(b - a) / (0.5 * k.low.w))), 1, 16);
    for (int s = 1; s <= steps; ++s) {
        vec3 p = mix(a, b, float(s) / float(steps));
        ivec3 cell = ivec3(floor((p - k.low.xyz) / k.low.w));
        if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(k.cells.xyz)))) {
            continue;
        }
        uint index = (uint(cell.y) * k.cells.z + uint(cell.z)) * k.cells.x + uint(cell.x);
        if ((shapes[k.cells.w + index / 32u] & (1u << (index % 32u))) != 0u) {
            struck = p;
            return true;
        }
    }
    return false;
}

void main() {
    uint capacity = uint(particles.length());
    uint i = gl_GlobalInvocationID.x;
    if (i >= capacity) {
        return;
    }
    uint fresh = (i + capacity - pc.start) % capacity;
    if (fresh < pc.count) {
        particles[i] = spawned[fresh];
        return;
    }
    SfeParticle p = particles[i];
    if (p.velLife.w <= 0.0) {
        return;
    }
    p.posAge.w += pc.dt;
    if (p.posAge.w >= p.velLife.w) {
        p.velLife.w = 0.0;
        particles[i] = p;
        return;
    }

    vec3 wind = vec3(matrices.wind.x, 0.0, matrices.wind.z) * matrices.wind.y * pc.windSpeed;
    vec3 v = p.velLife.xyz + (vec3(0.0, -9.81 * p.motion.x, 0.0) + p.motion.y * (wind - p.velLife.xyz)) * pc.dt;
    vec3 at = p.posAge.xyz + v * pc.dt;
    if (sfeParticleIs(p, kSfeParticleDrop) && p.colour.a > 0.0) {
        for (uint c = 0u; c < pc.catchers; ++c) {
            vec3 struck;
            if (sfeStrikes(c, p.posAge.xyz, at, struck)) {
                uint ring = uint(caught.length()) / 2u;
                uint slot = atomicAdd(caughtCount, 1u) % ring;
                float d = sfeParticleSize(p);
                // Its ball's volume in litres, pi / 6 d^3 a thousand times, as much of it as sticks.
                caught[2u * slot] = vec4(struck, p.colour.a * 523.6 * d * d * d);
                caught[2u * slot + 1u] = vec4(float(c), 0.0, 0.0, 0.0);
                p.velLife.w = 0.0;
                particles[i] = p;
                return;
            }
        }
    }
    if (at.y < p.motion.w) {
        p.velLife.w = 0.0;
        particles[i] = p;
        return;
    }

    vec4 clip = matrices.PV * vec4(at, 1.0);
    if (clip.w > 0.05 && dot(v, v) > 0.0) {
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) {
            vec3 surface = sfeScenePosition(sceneDepth, uv, matrices.invProjection, matrices.invView);
            vec3 eye = matrices.invView[3].xyz;
            float radius = sfeParticleSize(p) * 0.5;
            float behind = distance(at, eye) - distance(surface, eye);
            if (surface != vec3(0.0) && behind > -radius && behind < kThickness + length(v) * pc.dt) {
                vec3 n = textureLod(gNormal, uv, 0.0).rgb;
                n = dot(n, n) > 1e-6 ? normalize(n) : vec3(0.0, 1.0, 0.0);
                float into = dot(v, n);
                if (into < 0.0) {
                    v = (v - into * n) * kSlide - into * p.motion.z * n;
                }
                at = p.posAge.xyz + v * pc.dt;
                if (sfeParticleIsPiece(p) && n.y > kRestFlat && length(v) < kRestSpeed) {
                    v = vec3(0.0);
                    at = p.posAge.xyz;
                    p.motion.xy = vec2(0.0);
                }
            }
        }
    }
    p.posAge.xyz = at;
    p.velLife.xyz = v;
    particles[i] = p;
}
