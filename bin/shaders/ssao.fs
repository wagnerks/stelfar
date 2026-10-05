#version 450 core
#include "sfe.glsl"
layout(location = 0) out float FragColor;
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 1)) uniform sampler2D gNormal;
layout(SFE_SET(0, 2)) uniform sampler2D gMaterial;

layout(SFE_SET(0, 3), std140) uniform SSAOParams {
    float power;      // the visibility raised to it
    float grassFloor; // the least a leaf or a blade keeps
    float radius;    // metres round a point searched for what covers it
    float thickness; // metres what stands in front of a point is taken to go back from its face
    float frameIndex;
} params;

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

// Ground-truth ambient occlusion (Jimenez et al. 2016, as XeGTAO has it): round each point a few slices through the
// view, in each the highest thing on either side within `radius` metres, and how much of the cosine-weighted
// hemisphere over the surface those two horizons leave open. The radius is in metres and only projected to the
// screen, so the same spot is shaded the same however far or at what angle it is seen; what is far off takes up few
// pixels and is searched in as many steps as what is near. Each texel of a 4x4 block turns its slices and offsets its
// steps by its own sixteenth (as XeGTAO's spatial noise), the same every frame, and ssao_blur.fs averages exactly one
// of each back together: 32 directions a block, nothing left that changes from frame to frame to flicker (a pattern
// turned each frame and left to TAA trembled, its clamp holding the frames apart).
const int SLICES = 2;
const int STEPS = 8; // a side of a slice: 6 left a near thing between two steps, in bands
const float PI = 3.14159265;
const float HALF_PI = 1.57079633;
const float GRASS_HEIGHT = 0.4; // metres

float fastAcos(float x) {
    float a = abs(x);
    float r = (-0.156583 * a + HALF_PI) * sqrt(max(1.0 - a, 0.0));
    return x >= 0.0 ? r : PI - r;
}

vec3 viewAt(vec2 uv, out bool sky) {
    return sfeSceneViewPosition(sceneDepth, uv, matrices.invProjection, sky);
}

// A leaf or a blade of grass.
float foliageAt(vec2 uv) {
    return sfeIsFoliage(textureLod(gMaterial, uv, 0.0)) ? 1.0 : 0.0;
}

// The cosine (from the eye) of the direction to a sample `delta` away, drawn down to `low` (the surface's own plane)
// the further it is: nothing past `radius` covers the point. What stands nearer the eye than `thickness` in front of
// the point counts as further off by as much again, as a thin thing (a table's edge, a stick, a blade) does not reach
// back to what is seen behind it: no dark rim round it on the ground behind.
float horizonCos(vec3 delta, vec3 V, float low, vec3 lift) {
    delta -= lift;
    float d = length(delta);
    float front = dot(delta, V);
    float dEff = d + max(front - params.thickness, 0.0);
    float w = clamp((params.radius - dEff) / (0.6 * params.radius), 0.0, 1.0);
    return mix(low, front / max(d, 1e-5), w);
}

void main() {
    // Every texel of this half-size target stands for the full-size pixel at twice its place: its depth, nothing
    // blended, as the blur and the lighting's upsampling take it again.
    vec2 fullSize = vec2(textureSize(sceneDepth, 0));
    vec2 texel = 1.0 / fullSize;
    ivec2 full = ivec2(TexCoords * floor(fullSize * 0.5)) * 2;
    vec2 uv = (vec2(full) + 0.5) * texel;
    bool sky;
    precise vec3 p = viewAt(uv, sky);
    if (sky) {
        FragColor = 1.0;
        return;
    }
    // The surface's own plane from the depth around it, the nearer neighbour on each axis so an edge does not bend it.
    bool skyN;
    vec3 px0 = viewAt(uv - vec2(texel.x, 0.0), skyN) - p;
    vec3 px1 = viewAt(uv + vec2(texel.x, 0.0), skyN) - p;
    vec3 py0 = viewAt(uv - vec2(0.0, texel.y), skyN) - p;
    vec3 py1 = viewAt(uv + vec2(0.0, texel.y), skyN) - p;
    vec3 dx = abs(px0.z) < abs(px1.z) ? -px0 : px1;
    vec3 dy = abs(py0.z) < abs(py1.z) ? -py0 : py1;
    vec3 plane = cross(dx, dy);
    float plane2 = dot(plane, plane);
    // Degenerate only for the angle between the two steps, not their size: a millimetre a pixel near the eye made the
    // cross product smaller than any fixed bound, and the ground there turned to face the eye in a hard line.
    plane = plane2 > 1e-8 * dot(dx, dx) * dot(dy, dy) ? plane * inversesqrt(plane2) : vec3(0.0, 0.0, 1.0);
    if (dot(plane, p) > 0.0) {
        plane = -plane;
    }
    // The geometry's plane, not the G-buffer's normal: that carries the normal map's bumps, and tilted off the ground
    // half the flat ground round it stood over it and went grey. A blade or a leaf (by its material: a card's own
    // normal is sideways) is shaded as the ground under it, by what stands over it.
    const vec3 up = normalize(mat3(matrices.view) * vec3(0.0, 1.0, 0.0));
    vec3 n = foliageAt(uv) > 0.0 ? up : plane;

    vec3 V = normalize(-p);
    float radiusPx = params.radius * 0.5 * abs(matrices.projection[1][1]) * fullSize.y / max(-p.z, 1e-3);
    if (radiusPx < 1.0) {
        FragColor = 1.0;
        return;
    }
    // The first step at least a pixel and a half out, or it finds the point itself.
    float minS = 1.5 / radiusPx;
    vec2 signs = vec2(matrices.projection[0][0] < 0.0 ? -1.0 : 1.0, matrices.projection[1][1] < 0.0 ? -1.0 : 1.0);
    // The texel's place in its 4x4 block, in an order that spreads neighbours apart (Bayer): its slices' turn, and its
    // steps' offset from the golden ratio of the same, so no two texels of a block search alike.
    const float BAYER[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 cell = ivec2(gl_FragCoord.xy) & 3;
    float index = BAYER[cell.y * 4 + cell.x];
    float noiseSlice = (index + 0.5) / 16.0;
    float noiseStep = fract(0.5 + index * 0.618034);

    float visibility = 0.0;
    for (int slice = 0; slice < SLICES; ++slice) {
        vec2 omega = sfeCosSin((float(slice) + noiseSlice) * (PI / float(SLICES)));
        // The slice's direction in view space: a step across the screen goes the projection's way along x and y.
        vec3 dir = vec3(omega * signs, 0.0);
        vec3 D = normalize(dir - dot(dir, V) * V);
        vec3 axis = cross(D, V);
        vec3 projN = n - axis * dot(n, axis);
        float lenN = length(projN);
        float cosN = clamp(dot(projN, V) / max(lenN, 1e-5), 0.0, 1.0);
        float nAng = (dot(projN, D) < 0.0 ? -1.0 : 1.0) * fastAcos(cosN);
        // Angles from the eye towards D (+) and away (-); the surface's plane is a quarter turn off its normal.
        float lowPlus = cos(nAng + HALF_PI);
        float lowMinus = cos(nAng - HALF_PI);
        float hcPlus = lowPlus;
        float hcMinus = lowMinus;
        vec2 stepUv = omega * radiusPx * texel;
        float jitter = fract(noiseStep + float(slice) / float(SLICES));
        for (int i = 0; i < STEPS; ++i) {
            // Closer together near the point, where what covers it counts most.
            float s = (float(i) + jitter) / float(STEPS);
            s = s * s + minS;
            bool skyS;
            // A blade found round a point (a blade's or the ground's) counts as that much lower: a tuft does not grey the
            // bare ground round it, nor the blades of its own tuft one another, while a table, a bench or the van over the
            // grass covers both alike. Only the blades are lowered: the whole scene lowered round a blade took most of a
            // table's shade off the grass under it, and blades drawn as such only near the eye left a hard line there.
            // The material read only where a sample would raise the horizon (the lift only lowers it).
            vec2 uvP = uv + stepUv * s;
            vec3 sp = viewAt(uvP, skyS);
            if (!skyS) {
                float h = horizonCos(sp - p, V, lowPlus, vec3(0.0));
                if (h > hcPlus && foliageAt(uvP) > 0.0) {
                    h = horizonCos(sp - p, V, lowPlus, up * GRASS_HEIGHT);
                }
                hcPlus = max(hcPlus, h);
            }
            vec2 uvM = uv - stepUv * s;
            vec3 sm = viewAt(uvM, skyS);
            if (!skyS) {
                float h = horizonCos(sm - p, V, lowMinus, vec3(0.0));
                if (h > hcMinus && foliageAt(uvM) > 0.0) {
                    h = horizonCos(sm - p, V, lowMinus, up * GRASS_HEIGHT);
                }
                hcMinus = max(hcMinus, h);
            }
        }
        float hPlus = nAng + min(fastAcos(hcPlus) - nAng, HALF_PI);
        float hMinus = nAng + max(-fastAcos(hcMinus) - nAng, -HALF_PI);
        float sinN = sin(nAng);
        float arcPlus = cosN + 2.0 * hPlus * sinN - cos(2.0 * hPlus - nAng);
        float arcMinus = cosN + 2.0 * hMinus * sinN - cos(2.0 * hMinus - nAng);
        visibility += lenN * 0.25 * (arcPlus + arcMinus);
    }
    visibility = clamp(visibility / float(SLICES), 0.0, 1.0);
    // Leaves and blades are never more than a little grey: they are thin, light comes through them.
    FragColor = max(pow(visibility, params.power), params.grassFloor * foliageAt(uv));
}
