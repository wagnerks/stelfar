#version 450 core
#include "sfe.glsl"
#include "sfe_sky.glsl"
// What lives in the air round the camera: dust and pollen drifting on the wind, which only show where the sun
// catches them, and swarms of midges and flies dancing over the places AirLifePass picked. All of it moves here,
// from the instance's number and the time: nothing is simulated or stored. Drawn after TAA, which would average a
// speck a pixel wide that moves every frame away to nothing.
layout(location = 0) in vec2 aPos;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vLight; // added, premultiplied
layout(location = 2) out float vCover; // how much of what is behind it the speck hides
layout(location = 3) out vec2 vShape;  // a round speck at (0, 0); a butterfly's wings, as open as y, at x = 1

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
layout(SFE_SET(0, 8)) uniform sampler2DArrayShadow shadowMap;
layout(SFE_SET(0, 11), std140) uniform LightSpaceMatrices {
    mat4 lightSpaceMatrices[6];
};
layout(SFE_SET(0, 15), std140) uniform AirLifeBlock {
    SFE_SKY_PARAMS
    vec4 life;       // x what is drawn: 0 motes, 1 insects; y seconds; z metres along a side of the motes' cube; w amount
    vec4 view;       // xy the render size, z shadow cascades, w the sun's intensity setting
    vec4 swarms[8];  // xyz where a swarm dances, w 0 for none, 1 midges over the water, 2 flies
} air;

uint hashU(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float rand01(uint seed) {
    return float(hashU(seed) >> 8u) / 16777216.0;
}

#include "sfe_sun_visible.glsl"
float sunVisible(vec3 world) {
    return sfeSunVisible(world, int(air.view.z + 0.5));
}

// Scattered towards the eye out of the sun's light: strongly forwards, so specks glow looking into the sun.
float phase(vec3 fromEye) {
    const float g = 0.65;
    float cosTheta = dot(fromEye, normalize(air.toSun.xyz));
    return (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosTheta, 1.5) * 0.08;
}

void hide() {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    vLight = vec3(0.0);
    vCover = 0.0;
    vShape = vec2(0.0);
}

void main() {
    uint id = uint(SFE_INSTANCE_ID);
    float seconds = air.life.y;
    vec3 camera = air.cameraPos.xyz;
    vec3 world;
    vShape = vec2(0.0);
    float radius;
    float fade;
    float cover;
    float glow;

    if (air.life.x < 0.5) {
        // A mote: somewhere in a cube that wraps round the camera, drifting with the wind and wandering off it.
        float box = air.life.z;
        uint s = id * 3u;
        vec3 seed = vec3(rand01(s), rand01(s + 1u), rand01(s + 2u));
        vec3 wander = vec3(sin(seconds * (0.21 + 0.2 * seed.x) + seed.y * 40.0),
                           sin(seconds * (0.17 + 0.15 * seed.z) + seed.x * 40.0) * 0.6,
                           cos(seconds * (0.19 + 0.2 * seed.y) + seed.z * 40.0)) * 0.8;
        vec3 drift = vec3(air.wind.x, 0.03, air.wind.z) * (0.15 + 0.6 * air.wind.y) * seconds;
        vec3 rel = mod(seed * box + wander + drift - camera + 0.5 * box, box) - 0.5 * box;
        world = camera + rel;
        radius = mix(0.0015, 0.004, rand01(s + 7u));
        // Fading in and out at the cube's walls, where it wraps, and away right in front of the eye.
        float edge = max(abs(rel.x), max(abs(rel.y), abs(rel.z))) / (0.5 * box);
        fade = (1.0 - smoothstep(0.6, 1.0, edge)) * smoothstep(0.25, 0.8, length(rel));
        cover = 0.0;
        glow = 1.0;
    }
    else {
        uint swarm = id % 8u;
        vec4 at = air.swarms[swarm];
        uint index = id / 8u;
        int kind = int(at.w + 0.5);
        // How many of a swarm's places each kind fills: a cloud of midges, a few butterflies.
        uint many = kind == 3 ? 6u : kind == 4 ? 22u : 40u;
        if (kind == 0 || index >= many) {
            hide();
            return;
        }
        uint s = id * 5u + 101u;
        vec3 seed = vec3(rand01(s), rand01(s + 1u), rand01(s + 2u));
        if (kind <= 2) {
            bool midges = kind == 1;
            // Midges bob up and down in a tight column; flies loop wide and quick.
            float reach = midges ? 0.45 : 1.3;
            float speed = midges ? 2.6 : 3.4;
            vec3 w = vec3(1.0 + seed.x, 1.3 + seed.y, 0.9 + seed.z) * speed;
            vec3 offset = vec3(sin(seconds * w.x + seed.y * 30.0),
                               sin(seconds * w.y * (midges ? 1.6 : 0.7) + seed.z * 30.0) * (midges ? 1.4 : 0.4),
                               cos(seconds * w.z + seed.x * 30.0)) * reach;
            // A jitter on top, the quick darts no smooth loop makes.
            offset += vec3(sin(seconds * 23.0 + seed.x * 90.0), sin(seconds * 19.0 + seed.y * 90.0), sin(seconds * 21.0 + seed.z * 90.0)) * reach * 0.12;
            world = at.xyz + offset;
            radius = midges ? 0.007 : 0.012;
            fade = 1.0 - smoothstep(14.0, 22.0, distance(world, camera));
            cover = midges ? 0.55 : 0.85;
            glow = midges ? 0.6 : 0.25;
        }
        else if (kind == 3) {
            // A butterfly: wandering over the flowers, rising and dropping as it goes, its wings beating.
            float speed = 0.5 + 0.35 * seed.x;
            vec3 offset = vec3(sin(seconds * speed + seed.y * 20.0) * (2.2 + 2.0 * seed.z),
                               0.35 + 0.35 * sin(seconds * (1.6 + seed.y) + seed.x * 20.0) + 0.25 * seed.y,
                               cos(seconds * speed * 0.8 + seed.z * 20.0) * (2.2 + 2.0 * seed.x));
            world = at.xyz + offset;
            radius = 0.05 + 0.02 * seed.z;
            fade = 1.0 - smoothstep(18.0, 26.0, distance(world, camera));
            cover = 0.95;
            glow = 0.5;
            // The beat closes the wings and opens them again; the sprite is as wide as they are open.
            vShape = vec2(1.0, 0.25 + 0.75 * abs(sin(seconds * 7.0 + seed.x * 12.0)));
        }
        else {
            // A firefly: drifting low over the grass, its light coming and going.
            float speed = 0.35 + 0.3 * seed.y;
            vec3 offset = vec3(sin(seconds * speed + seed.x * 25.0) * (3.0 + 3.0 * seed.z),
                               0.15 * sin(seconds * 0.9 + seed.y * 25.0) + 0.5 * seed.y,
                               cos(seconds * speed * 0.9 + seed.z * 25.0) * (3.0 + 3.0 * seed.x));
            world = at.xyz + offset;
            radius = 0.03;
            float pulse = pow(max(sin(seconds * (1.1 + 0.5 * seed.x) + seed.z * 30.0), 0.0), 5.0);
            fade = (1.0 - smoothstep(20.0, 30.0, distance(world, camera))) * pulse;
            cover = 0.0;
            glow = 60.0; // its own light, not the sun's
        }
    }

    vec3 toSpeck = world - camera;
    float distanceToEye = length(toSpeck);
    vec4 clip = matrices.PVNoJitter * vec4(world, 1.0);
    if (fade <= 0.0 || clip.w <= 0.05) {
        hide();
        return;
    }
    // At least a pixel across, and a speck smaller than that thinned by how much smaller: by its width rather than its
    // area, as the eye sees a glint or a gnat much smaller than a pixel still.
    float pixels = radius * matrices.projection[1][1] * air.view.y * 0.5 / clip.w;
    float grown = max(pixels, 1.0);
    float spread = pixels / grown;
    vec2 corner = aPos * grown * 2.0 / air.view.xy * clip.w;
    gl_Position = vec4(clip.xy + corner, clip.zw);

    float amount = air.life.w * fade * spread;
    if (glow > 10.0) {
        // A firefly makes its own light, a warm yellow-green.
        vLight = vec3(0.9, 1.0, 0.35) * (glow * 0.06) * amount;
    }
    else if (vShape.x > 0.5) {
        // A butterfly is a lit thing, not a speck of scattering: its wings take the sun and the sky like any surface,
        // pale cream against the meadow.
        float sun = sunVisible(world);
        vec3 sunLight = air.sunColor.rgb * air.view.w;
        vec3 wings = vec3(1.0, 0.93, 0.7);
        vLight = wings * (sunLight * sun * 0.5 + air.sunColor.rgb * 0.05 * air.cameraPos.w) * amount;
    }
    else {
        float sun = sunVisible(world);
        vec3 sunLight = air.sunColor.rgb * air.view.w;
        vLight = sunLight * sun * phase(toSpeck / max(distanceToEye, 1e-3)) * glow * amount * (air.life.x < 0.5 ? 6.0 : 1.0);
    }
    vCover = cover * amount;
    vUv = aPos;
}
