#version 450 core
#include "sfe.glsl"
#include "sfe_material.glsl"
// A heightfield painted with tiling layers of a texture array, in their own colours: the first layer where the ground
// is level, the second, if the array has more than one, where it is steep, the edge between them set by the layers'
// heights. Drawn over the terrain's mesh (g_buffer.vs) and over the ground drawn dense near the camera
// (g_buffer_ground.vs), both of which put the terrain's own x/z in metres into TexCoords. FragPos, TexCoords1 and
// vMaterial are declared to match the two vertex shaders and are not read. An application stands in for this file with
// one of the same name in its content's shaders/.
layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec2 TexCoords;
layout(location = 2) in mat3 TBN;
layout(location = 5) in vec2 TexCoords1;
layout(location = 6) flat in uint vMaterial;
layout(location = 7) in vec4 vClip;
layout(location = 8) in vec4 vPrevClip;

layout(location = 0) out vec4 gMaterial;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;
layout(location = 3) out vec4 gVelocity;
layout(location = 4) out vec4 gEmissive;

// A layer per material, in the material's DIFFUSE and NORMALS slots: rgb its colour (sRGB) and a its height; rg its
// normal, b its roughness, a its occlusion.
layout(SFE_SET(1, 1)) uniform sampler2DArray groundAlbedo;
layout(SFE_SET(1, 2)) uniform sampler2DArray groundNormal;

// Metres one tile covers.
const float kTile = 3.0;
// How much of the layers' normal maps is kept: a little less than all, so that their relief stays a texture of the
// ground rather than standing out of it.
const float kNormalStrength = 0.9;

struct GroundSample {
    vec3 colour;  // linear
    vec3 normal;  // tangent space, z up
    float roughness;
    float occlusion;
    float height;
};

// Layer `layer` at `p`, twice: the second sample turned and scaled against the first and faded in by a slow noise,
// `variation`, so the tiles do not line up into a grid across the ground. Gradients are given: only some layers are
// sampled, and the implicit ones are undefined inside that branch.
GroundSample sampleGround(float layer, vec2 p, vec2 dpdx, vec2 dpdy, float variation) {
    const mat2 turn = mat2(0.6, 0.8, -0.8, 0.6);
    const float scale2 = 0.71;
    vec2 uv = p / kTile;
    vec2 uv2 = turn * uv * scale2 + vec2(0.37, 0.81);
    vec2 dx = dpdx / kTile;
    vec2 dy = dpdy / kTile;
    vec2 dx2 = turn * dx * scale2;
    vec2 dy2 = turn * dy * scale2;
    vec3 at = vec3(uv, layer);
    vec3 at2 = vec3(uv2, layer);
    // Mostly one of the two is all there is: only the fade between them reads both.
    vec4 a1 = vec4(0.0);
    vec4 n1 = vec4(0.5, 0.5, 1.0, 1.0);
    vec4 a2 = vec4(0.0);
    vec4 n2 = vec4(0.5, 0.5, 1.0, 1.0);
    if (variation < 0.999) {
        a1 = textureGrad(groundAlbedo, at, dx, dy);
        n1 = textureGrad(groundNormal, at, dx, dy);
    }
    if (variation > 0.001) {
        a2 = textureGrad(groundAlbedo, at2, dx2, dy2);
        n2 = textureGrad(groundNormal, at2, dx2, dy2);
    }
    vec4 a = mix(a1, a2, variation);
    // The second sample's normal is in the turned frame: turn it back.
    vec2 xy = mix(n1.rg * 2.0 - 1.0, transpose(turn) * (n2.rg * 2.0 - 1.0), variation) * kNormalStrength;

    GroundSample s;
    s.colour = pow(a.rgb, vec3(2.2));
    s.normal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
    s.roughness = mix(n1.b, n2.b, variation);
    s.occlusion = mix(n1.a, n2.a, variation);
    s.height = a.a;
    return s;
}

void main() {
    // No near fade (sfeNearFaded): the ground never comes that near the camera, and a discard would cost it the early
    // depth test, so that it would be shaded under all else drawn before it too.
    vec2 p = TexCoords;
    vec2 dpdx = dFdx(p);
    vec2 dpdy = dFdy(p);
    vec3 N = normalize(TBN[2]);
    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - N * N.x);
    vec3 B = normalize(vec3(0.0, 0.0, 1.0) - N * N.z);
    float variation = smoothstep(0.42, 0.58, sfeFbm(p * 0.045 + 7.0, 2));

    // How much of each layer, before their heights decide the edge between them: the steep one where the ground slopes.
    const int kLayers = 2;
    float steep = textureSize(groundAlbedo, 0).z > 1 ? smoothstep(0.33, 0.6, 1.0 - N.y) : 0.0;
    float weight[kLayers] = float[](1.0 - steep, steep);
    // A layer outweighed by the other by more than its height can make up (0.3) and the blend reaches (0.6) gets
    // nothing of the blend below, its weight there nought: it is not sampled at all.
    float least = max(0.01, max(weight[0], weight[1]) - 0.9);
    GroundSample samples[kLayers];
    float blend[kLayers];
    float highest = 0.0;
    for (int i = 0; i < kLayers; ++i) {
        blend[i] = 0.0;
        if (weight[i] < least) {
            continue;
        }
        samples[i] = sampleGround(float(i), p, dpdx, dpdy, variation);
        // What stands higher in a layer's height map shows through the other first.
        blend[i] = weight[i] + samples[i].height * 0.3;
        highest = max(highest, blend[i]);
    }
    vec3 albedo = vec3(0.0);
    vec3 detailNormal = vec3(0.0);
    float roughness = 0.0;
    float occlusion = 0.0;
    float total = 0.0;
    for (int i = 0; i < kLayers; ++i) {
        if (weight[i] < least) {
            continue;
        }
        float w = max(blend[i] - highest + 0.6, 0.0) * weight[i];
        GroundSample s = samples[i];
        albedo += s.colour * w;
        detailNormal += s.normal * w;
        roughness += s.roughness * w;
        occlusion += s.occlusion * w;
        total += w;
    }
    total = max(total, 1e-4);
    albedo /= total;
    roughness /= total;
    occlusion /= total;
    detailNormal = normalize(detailNormal / total);
    albedo *= mix(1.0, occlusion, 0.35);

    // The layers' normal on the ground's own frame: u runs along +x and v along +z, and the maps are OpenGL ones,
    // green towards the top of the image, where v is smallest.
    N = normalize(T * detailNormal.x - B * detailNormal.y + N * detailNormal.z);

    gNormal.xyz = N;
    gNormal.a = gl_FragCoord.z / gl_FragCoord.w;
    gAlbedoSpec.rgb = albedo;
    gAlbedoSpec.a = clamp(sfeFilteredRoughness(N, roughness, 1.0), 0.04, 1.0);
    // Dry ground: the specular factor 0.08, the ground's class (kSfeGround), and no occlusion of its own in the
    // velocity target's b.
    gMaterial = sfeGBufferMaterial(0.0, 0.08, kSfeGround, 0.04);
    gVelocity = sfeGBufferVelocity(vClip, vPrevClip, 0.0);
    gEmissive = vec4(0.0);
}
