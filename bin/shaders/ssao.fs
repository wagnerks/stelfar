#version 450 core
#include "sfe.glsl"
layout(location = 0) out float FragColor;
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 1)) uniform sampler2D gNormal;

layout(SFE_SET(0, 3), std140) uniform SSAOParams {
    float intensity;
    float bias;
    // View-space radii of the two rings: the near one for creases and contacts, the far one for the shade a big
    // thing (a tent, the van, a rock) lays on the ground under and around it.
    float nearRadius;
    float farRadius;
    float frameIndex;
} params;

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

const int SAMPLE_COUNT = 8;

vec2 vogel(int i, float phi) {
    float r = sqrt(float(i) + 0.5) / sqrt(float(SAMPLE_COUNT));
    float theta = float(i) * 2.399963 + phi;
    return sfeCosSin(theta) * r;
}

// The share of a disc of view-space radius `radius` round p that stands over p's tangent plane. The radius is
// projected so the disc shrinks with distance, and up close is no wider than `maxUv` of the screen, or the taps would
// scatter over the whole view. Only what lies within 2.4 radii counts, so a far wall or the sky behind an edge does not.
float ring(vec3 p, vec3 n, float radius, float maxUv, float phi) {
    float invZ = 1.0 / max(-p.z, 0.05);
    vec2 uvRad = min(0.5 * radius * invZ * abs(vec2(matrices.projection[0][0], matrices.projection[1][1])), vec2(maxUv));
    float range = radius * 2.4;
    precise float occlusion = 0.0;
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        vec2 uvOff = vogel(i, phi) * uvRad;
        bool sampleSky;
        precise vec3 sampleView = sfeSceneViewPosition(sceneDepth, TexCoords + uvOff, matrices.invProjection, sampleSky);
        if (sampleSky) {
            continue;
        }
        // Occlusion is "the sample sits above my tangent plane", not "the sample is nearer the camera": a pure depth
        // compare makes any plane seen at an angle occlude itself, in bands radiating from the camera.
        precise vec3 diff = sampleView - p;
        float dist = length(diff);
        vec3 dir = diff / max(dist, 1e-5);
        float rangeCheck = 1.0 - smoothstep(range * 0.35, range, dist);
        occlusion += max(dot(n, dir) - params.bias, 0.0) * rangeCheck;
    }
    return occlusion / float(SAMPLE_COUNT);
}

void main() {
    // Straight into view space: through world space and back was two matrix products a tap for nothing.
    bool sky;
    precise vec3 p = sfeSceneViewPosition(sceneDepth, TexCoords, matrices.invProjection, sky);
    if (sky) {
        FragColor = 1.0;
        return;
    }
    // The surface's own plane from the depth around it, the nearer neighbour on each axis so an edge does not bend it.
    vec2 texel = 1.0 / vec2(textureSize(sceneDepth, 0));
    bool skyN;
    vec3 px0 = sfeSceneViewPosition(sceneDepth, TexCoords - vec2(texel.x, 0.0), matrices.invProjection, skyN) - p;
    vec3 px1 = sfeSceneViewPosition(sceneDepth, TexCoords + vec2(texel.x, 0.0), matrices.invProjection, skyN) - p;
    vec3 py0 = sfeSceneViewPosition(sceneDepth, TexCoords - vec2(0.0, texel.y), matrices.invProjection, skyN) - p;
    vec3 py1 = sfeSceneViewPosition(sceneDepth, TexCoords + vec2(0.0, texel.y), matrices.invProjection, skyN) - p;
    vec3 dx = abs(px0.z) < abs(px1.z) ? -px0 : px1;
    vec3 dy = abs(py0.z) < abs(py1.z) ? -py0 : py1;
    vec3 plane = cross(dx, dy);
    float plane2 = dot(plane, plane);
    plane = plane2 > 1e-12 ? plane * inversesqrt(plane2) : vec3(0.0, 0.0, 1.0);
    // In view space the eye is at the origin, so a front-facing normal points back along p.
    if (dot(plane, p) > 0.0) {
        plane = -plane;
    }
    // The G-buffer's normal carries the normal map's bumps: tilted off the plane, half the flat ground round it stood
    // over its tangent plane, and the whole terrain went grey, so that what a tent or the van lays on it hardly showed.
    // Where the two are near alike (bumps) the plane is taken; where they are far apart (a grass blade's own normal, a
    // thin edge) the G-buffer's, which is steadier there than the depth's.
    vec3 n = plane;
    float onPlane = 1.0;
    vec3 g = texture(gNormal, TexCoords).xyz;
    if (dot(g, g) > 0.25) {
        g = normalize(mat3(matrices.view) * g);
        onPlane = smoothstep(0.5, 0.8, dot(g, plane));
        n = normalize(mix(g, plane, onPlane));
    }
    // A blade (its own normal far off the depth's plane) took the ground round it for cover and went near black out in
    // the open meadow. It is lit as the ground under it is: no near ring, and the far one against the sky overhead, so
    // only what stands over the grass (the van, a table, a tent) darkens it.
    const vec3 up = normalize(mat3(matrices.view) * vec3(0.0, 1.0, 0.0));
    vec3 nFar = normalize(mix(up, n, onPlane));
    // The ground seen low, near the eye: 20% of the screen cut a 3 m disc down to under 1 m, and the shade under a
    // table never reached the grass round it. What faces up may spread wider; the range check still bounds it.
    float farUv = mix(0.2, 0.45, smoothstep(0.7, 0.9, dot(nFar, up)));

    // Temporal IGN: the golden-ratio shift per frame keeps the pattern decorrelated over time,
    // so TAA averages it away instead of leaving it standing in screen space.
    float phi = sfeInterleavedGradientNoise(gl_FragCoord.xy, uint(params.frameIndex)) * 6.2831853;
    // The far ring turned half a step off the near one, so the two never lay their taps along the same spokes.
    float near = onPlane > 0.0 ? ring(p, n, params.nearRadius, 0.08, phi) * onPlane : 0.0;
    float far = ring(p, nFar, params.farRadius, farUv, phi + 1.2);
    FragColor = pow((1.0 - near) * (1.0 - far), params.intensity);
}
