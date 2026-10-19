#version 450 core
#include "sfe.glsl"
#include "sfe_material.glsl"
#include "sfe_terrain.glsl"
// Ground painted from where it is with tiling materials, in their own colours: grass, bare earth and the road, footpaths
// and patches trampled to the soil, roots under the trees, the bank and the lake bed, rock where it is steep. The
// terrain mesh puts world x/z in metres into TexCoords and the height above the water line into TexCoords1.x.
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

// Layer per material, see bin/textures/ground: rgb base colour and a height; rg normal, b roughness, a occlusion.
layout(SFE_SET(1, 1)) uniform sampler2DArray groundAlbedo;
layout(SFE_SET(1, 2)) uniform sampler2DArray groundNormal;
// VegetationPlacementsComponent::forestMap, in the material's SPECULAR slot.
layout(SFE_SET(1, 3)) uniform sampler2D forestMap;
// terrainNoiseTexture, in the ROUGHNESS slot.
layout(SFE_SET(1, 4)) uniform sampler2D terrainNoise;
// terrainRoadFrameTexture, in the METALLIC slot.
layout(SFE_SET(1, 5)) uniform sampler2D roadFrame;
// terrainPathTexture, in the SUBSURFACE slot.
layout(SFE_SET(1, 6)) uniform sampler2D pathMap;
// The area tint map (AreaTintFrame), in the frame set: the bank goes to mud where it or the water by it is dirty.
layout(SFE_SET(0, 22)) uniform sampler2D areaTintMap;

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
#include "sfe_marks.glsl"

const int kGrass = 0;
const int kMoss = 1;
const int kMud = 2;
const int kSand = 3;
const int kRocks = 4;
const int kLitter = 5;
const int kTrack = 6; // the road: wheel marks, laid along it
const int kPebbles = 7; // the lake's bank while it is clean
const int kSoil = 8; // trampled earth: the moss's image again, its earth coloured as dry soil; laid over the rest
const int kGravel = 9; // the stones in it: the pebbles' image, finer, sampled with the soil
const int kLayers = 10;
// The image in the arrays each material is.
const int kImage[kLayers] = int[](0, 1, 2, 3, 4, 5, 6, 7, 1, 7);
// Metres one tile covers.
const float kTile[kLayers] = float[](2.5, 3.5, 3.0, 4.0, 3.0, 2.5, 2.0, 2.0, 2.2, 1.1);
// The materials are painted bright; this brings them down to the light the rest of the scene is lit for.
const float kBrightness = 0.75;
// Metres the height map's full range stands for, for parallax: stones and clumps look raised, not painted on.
const float kRelief[kLayers] = float[](0.05, 0.06, 0.07, 0.05, 0.1, 0.05, 0.05, 0.05, 0.05, 0.04);
// Each material's middle height (its height map's mean), so the soil shows through each as much at the same wear.
const float kHeightMid[kLayers] = float[](0.63, 0.53, 0.53, 0.54, 0.58, 0.34, 0.53, 0.52, 0.53, 0.52);
// The painted maps are gentle; lit by a low sun they read as flat without some help.
const float kNormalStrength = 0.9;
// Hummocks, ruts and dips a metre or two across: the ground's own unevenness between the mesh's vertices.
const float kBumps[kLayers] = float[](0.35, 0.45, 0.3, 0.12, 0.5, 0.4, 0.3, 0.15, 0.35, 0.35);
// Dry soil, linear, a grey umber, and the moss's earth's mean brightness, which the soil's specks and blotches are
// measured against.
const vec3 kSoilColour = vec3(0.17, 0.115, 0.068);
const float kEarthLuma = 0.2;
// Wet mud, linear: a dark chocolate, greyed, its wet the sheen's to show rather than a darker colour's; the mud image's
// tone round which its own light and dark go.
const vec3 kWetMud = vec3(0.045, 0.032, 0.022);
// Mud dried out: grey-brown silt, lighter.
const vec3 kDryMud = vec3(0.12, 0.1, 0.078);
const float kMudLuma = 0.2;

// How much of a noise of this frequency survives the pixel footprint: fades detail out before it aliases.
float detail(float frequency, float footprint) {
    return 1.0 - smoothstep(0.2, 0.6, frequency * footprint);
}

// The slope of a noise field along x and z, by forward differences.
vec2 noiseSlope(vec2 p, float frequency, int octaves) {
    const float e = 0.05;
    float c = sfeFbm(p * frequency, octaves);
    float dx = sfeFbm((p + vec2(e, 0.0)) * frequency, octaves) - c;
    float dz = sfeFbm((p + vec2(0.0, e)) * frequency, octaves) - c;
    return vec2(dx, dz) / e;
}

struct GroundSample {
    vec3 colour;  // linear
    vec3 normal;  // tangent space, z up
    float roughness;
    float occlusion;
    float height;
};

// Where the eye's ray meets material `layer`'s relief, `relief` metres deep with the ground's mesh halfway down it:
// marched down from the top in steps, so a stone stands up and hides the ground behind it. `view` is towards the eye,
// in the ground's frame; the gradients are the tile's.
vec2 groundParallax(int layer, vec2 p, vec3 view, float relief, vec2 dx, vec2 dy) {
    float tile = kTile[layer];
    // Metres the ray crosses going down the whole relief; more steps the flatter it comes in.
    vec2 across = view.xy / max(view.z, 0.35) * relief;
    float steps = mix(4.0, 12.0, 1.0 - clamp(view.z, 0.0, 1.0));
    vec2 shift = -across / steps;
    vec2 at = p + across * 0.5;
    float ray = 0.0;
    float under = 1.0 - textureGrad(groundAlbedo, vec3(at / tile, kImage[layer]), dx, dy).a;
    float above = under;  // how far the ray was above the relief a step before
    for (int i = 0; i < 12 && ray < under; ++i) {
        above = under - ray;
        at += shift;
        ray += 1.0 / steps;
        under = 1.0 - textureGrad(groundAlbedo, vec3(at / tile, kImage[layer]), dx, dy).a;
    }
    // Between the last two steps, where the ray went under.
    float below = ray - under;
    return at - shift * (below / max(above + below, 1e-4));
}

// One material at `p`, twice: the second sample turned and scaled against the first and faded in by a slow noise, so
// the tiles do not line up into a grid across a meadow. Gradients are given: only some materials are sampled, and
// the implicit ones are undefined inside that branch. Drawn, it is one sample of each: the painted materials repeat
// nothing a second sample would hide.
GroundSample sampleGround(int layer, vec2 p, vec2 dpdx, vec2 dpdy, float variation, vec3 viewTangent, float parallax, bool drawn) {
    const mat2 turn = mat2(0.6, 0.8, -0.8, 0.6);
    const float scale2 = 0.71;
    float tile = kTile[layer];
    if (parallax > 0.001) {
        p = groundParallax(layer, p, viewTangent, kRelief[layer] * parallax, dpdx / tile, dpdy / tile);
    }
    vec2 uv = p / tile;
    vec2 uv2 = turn * uv * scale2 + vec2(0.37, 0.81);
    vec2 dx = dpdx / tile;
    vec2 dy = dpdy / tile;
    vec2 dx2 = turn * dx * scale2;
    vec2 dy2 = turn * dy * scale2;
    vec3 at = vec3(uv, kImage[layer]);
    vec3 at2 = vec3(uv2, kImage[layer]);
    // Mostly one of the two is all there is: only the fade between them reads both.
    vec4 a1 = vec4(0.0);
    vec4 n1 = vec4(0.5, 0.5, 1.0, 1.0);
    vec4 a2 = vec4(0.0);
    vec4 n2 = vec4(0.5, 0.5, 1.0, 1.0);
    if (drawn) {
        a1 = textureGrad(groundAlbedo, at, dx, dy);
        n1 = textureGrad(groundNormal, at, dx, dy);
        variation = 0.0;
    }
    else if (variation < 0.999) {
        a1 = textureGrad(groundAlbedo, at, dx, dy);
        n1 = textureGrad(groundNormal, at, dx, dy);
    }
    if (!drawn && variation > 0.001) {
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

// The track, laid down the road by its frame (u across it, v along it), so its wheel marks follow the road round its
// bends and run on across a clearing at its end. One sample: a second, turned one would turn the marks off the road.
GroundSample sampleTrack(vec2 p, vec2 dpdx, vec2 dpdy, vec3 viewTangent, float parallax, bool drawn) {
    mat2 frameOf;
    vec2 frame = sfeRoadFrame(roadFrame, p, frameOf);
    GroundSample s = sampleGround(kTrack, frame, frameOf * dpdx, frameOf * dpdy, 0.0,
        vec3(frameOf * viewTangent.xy, viewTangent.z), parallax, drawn);
    // The normal from the road's frame back to the ground's, u along x and v along z.
    vec2 across = normalize(vec2(frameOf[0].x, frameOf[1].x));
    vec2 along = normalize(vec2(frameOf[0].y, frameOf[1].y));
    s.normal.xy = vec2(across.x * s.normal.x - along.x * s.normal.y, along.y * s.normal.y - across.y * s.normal.x);
    return s;
}

// The grain pressed into a mark in a frame turned `angle` from +x, metres: `smear` streaks along it, the way the ground
// was churned, and `tread` blocks pressed in across it in two staggered rows, slanted into chevrons, a tyre's or a
// boot sole's.
float marksGrain(vec2 xz, float angle, float smear, float tread) {
    vec2 along = vec2(cos(angle), sin(angle));
    vec2 q = vec2(dot(xz, vec2(-along.y, along.x)), dot(xz, along));
    float smears = sfeValueNoise(vec2(q.x * 9.0, q.y * 1.5)) - 0.5;
    float lane = fract(q.x / 0.11);
    float bar = fract(q.y / 0.075 + 0.5 * step(0.5, lane) + 0.8 * abs(lane - 0.5));
    float lug = smoothstep(0.2, 0.3, bar) - smoothstep(0.7, 0.8, bar);
    return smears * smear * 0.008 - lug * tread * 0.004;
}

// The marks' own relief at `xz`: x its height, metres, y its lumps alone, -0.5..0.5. `clods` lumps a few centimetres
// across, squeezed up; the grain (marksGrain) in the frame of the way the mark runs, `along`, snapped to sixteenths of
// a half turn and blended between the two nearest, so that it stands still along a mark whose way wavers a little.
vec2 marksRelief(vec2 xz, vec2 along, float clods, float smear, float tread) {
    float lumps = sfeFbm(xz * 14.0 + 7.0, 2) - 0.5;
    const float kStep = 3.14159265 / 16.0;
    along *= along.x < 0.0 ? -1.0 : 1.0;
    float k = atan(along.y, along.x) / kStep;
    float k0 = floor(k);
    float grain = mix(marksGrain(xz, k0 * kStep, smear, tread), marksGrain(xz, (k0 + 1.0) * kStep, smear, tread), k - k0);
    return vec2(lumps * clods * 0.025 + grain, lumps);
}

void main() {
    // No near fade (sfeNearFaded): the ground never comes that near the camera, and a discard would cost it the early
    // depth test, so that it would be shaded under the trees and all else drawn before it too.
    vec2 p = TexCoords;
    vec2 dpdx = dFdx(p);
    vec2 dpdy = dFdy(p);
    float height = TexCoords1.x;
    vec3 N = normalize(TBN[2]);
    float slope = 1.0 - N.y;

    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - N * N.x);
    vec3 B = normalize(vec3(0.0, 0.0, 1.0) - N * N.z);
    // The camera, from the rigid view matrix.
    vec3 camera = -transpose(mat3(matrices.view)) * matrices.view[3].xyz;
    vec3 toEye = normalize(camera - FragPos);
    vec3 viewTangent = vec3(dot(toEye, T), dot(toEye, B), dot(toEye, N));
    float footprint = length(fwidth(p));
    // The drawn look shades the ground by its painted relief and its parallax alone: no hummocks, no ruts in the normal.
    // Those were much of what this shader cost.
    bool drawn = matrices.style.x > 0.5;
    // Parallax only up close, where a height map's centimetres still cover a few pixels.
    float parallax = 1.0 - smoothstep(0.006, 0.02, footprint);
    // The marks in the ground (sfe_marks.glsl). Up close, a rut is seen into: the ground in it is where the eye's ray
    // meets its floor, marched down to it, so its far side shows and its near side hides what is under it.
    vec2 marksCover = sfeMarksCover(FragPos.xz);
    vec2 marksAt = FragPos.xz;
    vec2 reachX = vec2(matrices.marksLook.y, 0.0);
    vec2 reachZ = vec2(0.0, matrices.marksLook.y);
    bool marked = false;
    if (marksCover.x + marksCover.y > 0.001) {
        float depth = sfeMarks(marksAt, dpdx, dpdy, marksCover).x;
        marked = depth > 1e-4 || sfeMarks(marksAt, dpdx + reachX, dpdy + reachZ, marksCover).x > 1e-4;
        float close = 1.0 - smoothstep(0.015, 0.03, footprint);
        // Only as much of it as the ground drawn dense near the camera has not pressed into its own shape: TexCoords1.y
        // of it there (g_buffer_ground.vs), none on the terrain's own mesh.
        float rest = 1.0 - TexCoords1.y;
        if (depth * rest > 0.002 && close > 0.001) {
            // `down` under the mesh, the ray is `along * down` further on.
            vec2 along = -toEye.xz / max(toEye.y, 0.2);
            const int kSteps = 8;
            float stride = clamp(2.0 * depth * rest, 0.02, matrices.marksSize.z) / float(kSteps);
            float down = 0.0;
            float under = depth * rest;
            float above = under; // how far over the floor the ray was a step before
            for (int i = 0; i < kSteps && down < under; ++i) {
                above = under - down;
                down += stride;
                under = sfeMarks(FragPos.xz + along * down, dpdx, dpdy, marksCover).x * rest;
            }
            // Between the last two steps, where the ray went under.
            float below = max(down - under, 0.0);
            down -= stride * below / max(above + below, 1e-5);
            marksAt += along * down * close;
            p += along * down * close;
        }
    }

    vec4 noise = sfeTerrainNoise(terrainNoise, p);
    SfeTerrainLayers layers = sfeTerrainLayers(p, height, slope, sfeForest(forestMap, p), noise);
    float broad = layers.broad;
    float patches = layers.patches;
    float fine = sfeFbm(p * 0.35, 2);
    float variation = smoothstep(0.42, 0.58, noise.b);
    // A lighter and a darker tone of every colour, in soft blotches of a few metres.
    float blotchNoise = sfeFbm(p * 0.28 + 5.0, 2);
    float blotch = smoothstep(0.35, 0.65, blotchNoise);

    // How much of each material, before their heights decide the edges between them.
    float weight[kLayers];
    float underwater = layers.underwater;
    float steep = layers.rock * (1.0 - underwater);
    float open = (1.0 - steep) * (1.0 - underwater);
    float bank = clamp(layers.sand + layers.mud, 0.0, 1.0);
    float land = open * (1.0 - bank);
    float stony = smoothstep(0.55, 0.8, patches);
    weight[kGrass] = land * (1.0 - layers.earth) * (1.0 - layers.litter);
    weight[kLitter] = land * (1.0 - layers.earth) * layers.litter;
    weight[kMoss] = land * layers.earth;
    // The lake bottom: silt, with mats of algae over it in patches, and stones where it is stony.
    // (Its noise worked out only below -0.5, where the depth lets any of it through.)
    float algae = height < -0.5 ? smoothstep(0.4, 0.62, sfeFbm(p * 0.09 + 41.0, 3)) * (1.0 - smoothstep(-1.2, -0.5, height)) : 0.0;
    weight[kMud] = underwater * (1.0 - stony) * (1.0 - algae);
    weight[kMoss] += underwater * (1.0 - stony) * algae;
    weight[kRocks] = steep + underwater * stony;
    weight[kTrack] = 0.0;
    weight[kSoil] = 0.0;
    weight[kGravel] = 0.0;
    // The bank as it is clean: a little silt at the water's very edge, stones with sand in its coves above that, and
    // grass at its top (sfeShoreByWater). Dirty, it is mud (below).
    float shore = open * bank;
    float byWater = sfeShoreByWater(height, fine);
    float edge = 1.0 - smoothstep(-0.05, 0.1, height + 0.15 * (fine - 0.5));
    float clean = shore * (1.0 - edge);
    float sandy = smoothstep(0.4, 0.65, noise.g + 0.3 * (fine - 0.5));
    weight[kMud] += shore * edge;
    weight[kSand] = clean * byWater * sandy;
    weight[kPebbles] = clean * byWater * (1.0 - sandy);
    weight[kGrass] += clean * (1.0 - byWater);
    // The road: packed dirt, two wheel ruts, and a strip of earth with a little green down the middle between them.
    float roadAbs = abs(layers.roadDistance);
    float road = layers.road * (1.0 - underwater);
    float roadStrip = 1.0 - smoothstep(0.15, 0.7, roadAbs + 0.25 * (fine - 0.5));
    // Squared by hand: pow() of a negative base is undefined, and came out NaN on the inner side of each rut.
    float rutOffset = (roadAbs - 0.8) / 0.28;
    float rut = exp(-rutOffset * rutOffset);
    for (int i = 0; i < kLayers; ++i) {
        weight[i] *= 1.0 - road;
    }
    // A country track: a little green down the strip between the wheels, wheel marks either side of it.
    weight[kMoss] += road * roadStrip;
    weight[kTrack] += road * (1.0 - roadStrip);
    // Where the bank is dirty, by it and the water by it (sfeShoreDirty), all the band the litter lies in (sfeShoreBand)
    // is mud, whatever it is clean, and nothing grows on it out of the water (grass.vs, grass_clump.vs); as much of it
    // as it is cleaned turns back to what it is clean.
    vec3 areaTint = sfeAreaTint(areaTintMap, matrices.tintArea, FragPos.xz);
    float muddy = open * sfeShoreBand(height, fine, matrices.tintShore.z) * sfeShoreDirty(areaTint, matrices.tintShore);
    for (int i = 0; i < kLayers; ++i) {
        weight[i] *= 1.0 - muddy;
    }
    weight[kMud] += muddy;
    // How foul the land is, along the curve the trees go bare along (tintShore: VegetationSystem::wither's). The grass's
    // ground goes to mud as it is, in patches that spread and join, as the grass on it thins (sfeGrassLeft).
    float foul = clamp((areaTint.r - matrices.tintShore.x) / max(matrices.tintShore.y - matrices.tintShore.x, 1e-3), 0.0, 1.0);
    float grassMud = weight[kGrass] * smoothstep(0.0, 0.7, foul * (0.7 + 0.6 * fine));
    weight[kGrass] -= grassMud;
    weight[kMud] += grassMud;
    // Footpaths and patches trampled: the soil shows between the grass the more it is walked on, and in bald spots that
    // spread and join until, where it is worn most, it is all bare earth. A clearing camped in is walked thin in places.
    // `soil` is how much of it shows, laid over the rest where that is lowest first: between the blades.
    vec4 trodden = sfeTerrainPath(pathMap, p);
    float wear = trodden.z * open;
    float camp = (1.0 - smoothstep(-3.0, 1.0, trodden.y)) * land;
    // Towards a bald spot the grass thins out over half a metre or more, broken into tufts and gaps, the soil showing
    // between its blades a little way out from it.
    float soil = camp * 0.2 * smoothstep(0.5, 0.75, patches);
    if (wear > 0.01) {
        float bare = sfeTerrainBare(wear, trodden.w, p);
        float bald = smoothstep(-0.2, 0.12, bare);
        bald = clamp(bald + 1.2 * bald * (1.0 - bald) * (sfeFbm(p * 1.6 + 47.0, 2) - 0.5) * 2.0, 0.0, 1.0);
        float thin = max(0.25 * smoothstep(0.1, 0.6, wear), 0.5 * smoothstep(-0.6, -0.15, bare));
        soil = max(soil, bald + (1.0 - bald) * thin);
    }
    // The marks: how deep, how churned up, how deep round about (over the reach their rims and mud spread over), and
    // which way they run. Grass in them is only laid flat (below); churned up over many passes, its soil is bared.
    float marksDepth = 0.0;
    float marksChurn = 0.0;
    float marksAround = 0.0;
    float churnAround = 0.0;
    float pressed = 0.0;
    vec2 marksAlong = vec2(1.0, 0.0);
    if (marked) {
        vec2 marks = sfeMarks(marksAt, dpdx, dpdy, marksCover);
        marksDepth = marks.x;
        marksChurn = marks.y;
        vec2 around = sfeMarks(marksAt, dpdx + reachX, dpdy + reachZ, marksCover);
        marksAround = around.x;
        churnAround = around.y;
        pressed = smoothstep(0.0, matrices.marksSize.w, marksDepth);
        float bend;
        marksAlong = sfeMarksAlong(marksAt, matrices.marksLook.y, marksCover, bend);
        soil = max(soil, smoothstep(matrices.marksChurn.x, matrices.marksChurn.y, marksChurn) * land);
    }


    // A layer outweighed by the heaviest by more than its height can make up (0.3) and the blend reaches (0.6) gets
    // nothing of the blend below, its weight there nought: it is not sampled at all.
    float heaviest = 0.0;
    for (int i = 0; i < kLayers; ++i) {
        heaviest = max(heaviest, weight[i]);
    }
    float least = max(0.01, heaviest - 0.9);
    GroundSample samples[kLayers];
    float blend[kLayers];
    float highest = 0.0;
    for (int i = 0; i < kLayers; ++i) {
        blend[i] = 0.0;
        if (weight[i] < least) {
            continue;
        }
        samples[i] = i == kTrack ? sampleTrack(p, dpdx, dpdy, viewTangent, parallax, drawn)
                                 : sampleGround(i, p, dpdx, dpdy, variation, viewTangent, parallax, drawn);
        // Stones stand out of the mud, grass tufts out of the sand.
        blend[i] = weight[i] + samples[i].height * 0.3;
        highest = max(highest, blend[i]);
    }
    // The forest floor's roots and earth, whatever in it is not a plant's colour, grey with the trees' bark where the
    // land is foul (above), the grey their bark takes (sfe_gbuffer_surface).
    if (weight[kLitter] >= least) {
        float dead = foul * foul * (3.0 - 2.0 * foul) * (1.0 - sfePlantColour(samples[kLitter].colour));
        samples[kLitter].colour = mix(samples[kLitter].colour, vec3(sfeLuma(samples[kLitter].colour)), dead * 0.7);
        // Round a clearing its fallen leaves dull and brown, darker: no bright orange by a camp.
        float byClearing = 1.0 - smoothstep(3.0, 8.0, trodden.y);
        vec3 brown = sfeLuma(samples[kLitter].colour) * vec3(1.3, 0.95, 0.6);
        samples[kLitter].colour = mix(samples[kLitter].colour, brown, 0.6 * byClearing) * mix(1.0, 0.8, byClearing);
    }
    // Over each material's own colour: a lighter and a darker tone in blotches, the bank darkening as it gets wet, and
    // the road darker down its ruts. Worked out after the sampling, so as not to be held through it.
    vec3 tint[kLayers];
    for (int i = 0; i < kLayers; ++i) {
        tint[i] = vec3(mix(0.9, 1.08, blotch) * kBrightness);
    }
    // Mud: its image's light and dark in the mud's own colours, not the painted orange: grey-brown silt where it has
    // dried, a dark chocolate and glossy where it is wet, by the water and under it. Mud is wet earth: wet over the
    // bank, drying up it to its half metre over the water (as the marks' law takes the bank, Soundscape's kBank), its
    // cracks closed up where it is wet, its face smooth and sheened.
    float mudWet = 0.0;
    if (weight[kMud] >= least) {
        // Where the land is foul, damp through, a brown, not dried to grey.
        mudWet = max(max(layers.wet, 1.0 - smoothstep(0.25, 0.6, height)), 0.5 * foul);
        float wetMud = max(mudWet, underwater);
        float tone = clamp(sfeLuma(samples[kMud].colour) / kMudLuma, 0.6, 1.5);
        samples[kMud].colour = mix(kDryMud, kWetMud, wetMud) * mix(tone, 1.0, 0.5 * wetMud) / kBrightness;
        samples[kMud].roughness = mix(samples[kMud].roughness, 0.15, mudWet * (1.0 - underwater));
        samples[kMud].normal = normalize(mix(samples[kMud].normal, vec3(0.0, 0.0, 1.0), 0.7 * wetMud));
        mudWet *= weight[kMud] * (1.0 - underwater);
    }
    tint[kSand] *= mix(1.0, 0.55, layers.wet);
    tint[kPebbles] *= mix(1.0, 0.55, layers.wet);
    // Under the water the silt is grey-green and the algae an olive slime.
    tint[kMud] *= mix(vec3(1.0), vec3(0.75, 0.85, 0.65), underwater);
    tint[kMoss] *= mix(vec3(1.0), vec3(0.8, 0.85, 0.45), underwater);
    tint[kTrack] *= mix(1.0, 0.6, rut * 0.8 * road);
    // The soil darker in damp spots, where something was spilt or the shade keeps it.
    float damp = soil > 0.01 ? smoothstep(0.6, 0.75, sfeFbm(p * 0.45 + 61.0, 2)) : 0.0;
    // Churned up, it is damp all through.
    damp = max(damp, clamp(marksChurn * matrices.marksLook.z, 0.0, 1.0));
    tint[kSoil] *= mix(1.0, 0.7, damp);
    vec3 albedo = vec3(0.0);
    vec3 detailNormal = vec3(0.0);
    float roughness = 0.0;
    float occlusion = 0.0;
    float bumpiness = 0.0;
    float relief = 0.0;
    float total = 0.0;
    for (int i = 0; i < kLayers; ++i) {
        if (weight[i] < least) {
            continue;
        }
        float w = max(blend[i] - highest + 0.6, 0.0) * weight[i];
        GroundSample s = samples[i];
        albedo += s.colour * tint[i] * w;
        detailNormal += s.normal * w;
        roughness += s.roughness * w;
        occlusion += s.occlusion * w;
        bumpiness += kBumps[i] * w;
        relief += (s.height - kHeightMid[i]) * w;
        total += w;
    }
    total = max(total, 1e-4);
    albedo /= total;
    roughness /= total;
    occlusion /= total;
    bumpiness /= total;
    relief /= total;
    detailNormal = normalize(detailNormal / total);
    // Trampled grass dulled to olive and straw the more it is walked on, a camped-in meadow a little so; where it is
    // walked on, laid flat, its blades' relief pressed down and a shade lighter, so a path reads as a line from afar.
    float trampled = max(wear, camp * 0.25);
    if (trampled > 0.01) {
        vec3 straw = sfeLuma(albedo) * vec3(1.05, 0.95, 0.62);
        float flattened = smoothstep(0.1, 0.35, wear);
        albedo = mix(albedo, straw, 0.6 * smoothstep(0.0, 0.45, trampled)) * mix(1.0, 1.15, flattened)
            * mix(1.0, 0.85, smoothstep(0.45, 0.9, wear));
        detailNormal = normalize(mix(detailNormal, vec3(0.0, 0.0, 1.0), 0.4 * flattened));
    }
    // Pressed down, the ground is packed smooth: its own relief and hummocks pressed out, so the mark's walls show.
    detailNormal = normalize(mix(detailNormal, vec3(0.0, 0.0, 1.0), pressed));
    bumpiness *= 1.0 - pressed;
    // Dry soil over it all where it shows: the moss's earth, its specks, blotches and rootlets, in the colour of dust;
    // its clover trodden dark, only at the grass's edge, and where it is worn away crumbs in its place, the gravel's
    // grain; here and there a stone half buried in it, the tops of the gravel's biggest, in the soil's own colours. It
    // shows through the ground over it where that is lowest, so between the grass's blades first and over them last,
    // in specks and streaks.
    if (soil >= 0.01) {
        GroundSample s = sampleGround(kSoil, p, dpdx, dpdy, variation, viewTangent, parallax, drawn);
        GroundSample g = sampleGround(kGravel, p, dpdx, dpdy, variation, viewTangent, parallax, drawn);
        float plant = sfePlantColour(s.colour);
        float leaf = plant * (1.0 - smoothstep(0.2, 0.45, wear)) * (1.0 - smoothstep(0.3, 0.7, soil));
        float crumbs = mix(0.7, 1.2, smoothstep(0.08, 0.35, sfeLuma(g.colour)));
        vec3 earth = kSoilColour * mix(pow(sfeLuma(s.colour) / kEarthLuma, 1.6), crumbs, plant);
        s.colour = mix(earth, s.colour * vec3(0.45, 0.5, 0.35), leaf);
        s.normal = normalize(mix(s.normal, mix(vec3(0.0, 0.0, 1.0), g.normal, 0.5), plant - leaf));
        s.occlusion = mix(s.occlusion, mix(1.0, g.occlusion, 0.5), plant - leaf);
        float stone = smoothstep(0.0, 0.05, g.height - mix(0.9, 0.74, sfeFbm(p * 0.6 + 31.0, 2))) * (1.0 - leaf);
        float shade = clamp(sfeLuma(g.colour) / 0.3, 0.0, 1.0);
        s.colour = mix(s.colour, mix(kSoilColour, vec3(sfeLuma(kSoilColour)), 0.3) * mix(0.6, 1.3, shade), stone);
        s.normal = normalize(mix(s.normal, g.normal, stone));
        s.roughness = mix(mix(s.roughness, 0.75, damp), g.roughness, stone);
        s.occlusion = mix(s.occlusion, g.occlusion, stone);
        float show = smoothstep(-0.1, 0.1, 0.8 * soil - 0.38 - relief);
        albedo = mix(albedo, s.colour * tint[kSoil], show);
        detailNormal = normalize(mix(detailNormal, s.normal, show));
        roughness = mix(roughness, s.roughness, show);
        occlusion = mix(occlusion, s.occlusion, show);
        bumpiness = mix(bumpiness, kBumps[kSoil], show);
    }

    // How the marks look, by the ground they are in. Grass is only laid flat: a strip of blades along the mark, a shade
    // lighter and sheened. Wet ground pressed (the bank, by the water) turns to mud, wet and heavy: dark chocolate,
    // glossy in patches, darker and wetter on the floor, its rims squeezed up in lumps; churned up, wet mud too, as
    // wet as kWet. Bare earth packs down a shade darker and smoother, its rims crumbled. Pressed into the floor of what is not grass, the tread of a
    // tyre or a boot's sole; churned up, the ground smeared along the way it went.
    float grassy = 0.0;
    float mud = 0.0;
    float soak = 0.0; // how wet the ground is under the marks: all of it down by the water
    float glossy = 0.0; // how much of a wet film's gloss the marks have: only where the ground is wet
    float film = 0.0; // how glossy the wet mud is: the water standing on it in patches and down on the floor, less between
    float laidSheen = 0.0;
    float markFloor = 0.0;
    float belowSides = 0.0;
    if (marked) {
        belowSides = marksDepth - marksAround;
        // Churned up, the mud is thrown out over the edge of the mark in a ragged fringe; any mark's edge is ragged,
        // over the few millimetres the depth round about it (over the rims' reach) eases out in, whole inside it.
        float splash = sfeFbm(marksAt * 9.0 + 3.0, 2) - 0.5;
        // The bank: what lies less than half a metre over the water line, as the marks' own law takes it (Soundscape's
        // kBank), mud under whatever grows on it.
        float bankMud = max(bank, 1.0 - smoothstep(0.4, 0.5, height));
        grassy = clamp(weight[kGrass] + weight[kLitter], 0.0, 1.0) * (1.0 - smoothstep(0.3, 0.7, soil)) * (1.0 - bankMud);
        float churned = smoothstep(matrices.marksChurn.x, matrices.marksChurn.y,
            max(marksChurn, 1.6 * churnAround + 0.9 * splash));
        // Down to the water line and under it, the marks are mud all the same: the water is drawn over them there.
        // Worked by what slips over it anywhere, the water in the ground comes up to its face: wet mud by the time its
        // soil is bared (marksChurn.x), as wet as kWet has it, glossy and standing in puddles as by the water.
        soak = max(max(bankMud, underwater),
            matrices.marksLook.z * smoothstep(0.0, matrices.marksChurn.x, max(marksChurn, churnAround)));
        // Wet, its edge soaks out into the ground round it, soft, rather than crumbling off ragged.
        float inMark = clamp((max(marksDepth, marksAround) - 0.0015) / mix(0.004, 0.012, soak)
            + mix(0.8, 0.3, soak) * splash, 0.0, 1.0);
        inMark *= inMark * (3.0 - 2.0 * inMark);
        mud = inMark * max(soak, churned);
        glossy = mud * soak;
        markFloor = smoothstep(0.0, 0.003, belowSides) * smoothstep(0.003, 0.01, marksDepth);
        vec2 across = vec2(-marksAlong.y, marksAlong.x);
        float laid = pressed * grassy * (1.0 - churned);
        if (laid > 0.001) {
            // Laid flat, the blades turn their faces up to the sky: a strip a good deal lighter and yellower than the
            // grass standing round it, sheened, streaked along the way it was laid.
            float streak = sfeValueNoise(vec2(dot(marksAt, across) * 45.0, dot(marksAt, marksAlong) * 4.0))
                * detail(45.0, footprint);
            vec3 blades = mix(albedo, vec3(sfeLuma(albedo)), 0.15) * vec3(1.18, 1.16, 1.02) * mix(0.88, 1.12, streak);
            albedo = mix(albedo, blades, laid);
            roughness = mix(roughness, 0.55, laid);
            laidSheen = laid;
            // Tilted across it by the blades' streaks; the tangent frame's v runs down -z.
            vec2 tilt = across * (streak - 0.5) * 0.3;
            detailNormal = normalize(mix(detailNormal, vec3(tilt.x, -tilt.y, 1.0), laid));
        }
        float earth = (1.0 - grassy) * inMark;
        float rim = smoothstep(0.0, 0.01, matrices.marksLook.x * max(marksAround - marksDepth, 0.0)) * (1.0 - grassy);
        // Bare earth packs down a shade darker.
        albedo *= 1.0 - 0.12 * pressed * earth * (1.0 - mud);
        float near = detail(14.0, footprint);
        float clods = max(rim, churned) * (1.0 - grassy);
        float smear = churned * (1.0 - grassy);
        // A wheel that spun wiped its own tread out.
        float tread = markFloor * (1.0 - grassy) * (1.0 - 0.8 * churned) * detail(40.0, footprint);
        float lump = 0.0;
        if (near > 0.001 && clods + smear + tread > 0.001) {
            float e = max(footprint, 0.004);
            vec2 c = marksRelief(marksAt, marksAlong, clods, smear, tread);
            vec2 cx = marksRelief(marksAt + vec2(e, 0.0), marksAlong, clods, smear, tread);
            vec2 cz = marksRelief(marksAt + vec2(0.0, e), marksAlong, clods, smear, tread);
            vec2 slope = vec2(cx.x - c.x, cz.x - c.x) / e * near;
            detailNormal = normalize(detailNormal + vec3(-slope.x, slope.y, 0.0));
            lump = c.y;
        }
        float sheen = sfeFbm(marksAt * 4.0 + 17.0, 2);
        vec3 wetMud = kWetMud * mix(0.85, 1.15, blotch) * mix(0.9, 1.1, sheen) * mix(1.0, 0.75, markFloor);
        // Damp earth: the ground's own colour darkened, the way to the mud.
        vec3 dampEarth = albedo * vec3(0.85, 0.8, 0.74);
        vec3 mudColour = mix(dampEarth, wetMud, mix(0.1, 1.0, soak)) * (1.0 + 0.8 * clamp(lump * 2.0, 0.0, 1.0) * rim);
        albedo = mix(albedo, mudColour, mud);
        // Wet all over, but glossy only in patches where water stands on it and down on the floor, duller on the rims'
        // clods: a sheen all over mirrored the sky over the whole of it, and it read as light as dry earth.
        float puddled = max(markFloor, smoothstep(0.45, 0.7, sheen)) * (1.0 - rim);
        film = glossy * mix(0.3, 1.0, puddled);
        roughness = mix(roughness, mix(0.7, mix(0.35, 0.12, puddled) + 0.1 * rim, soak), mud);
        // Crumbs of dry earth on the rims, lighter and rough.
        albedo *= 1.0 + 0.25 * clamp(lump * 2.0, 0.0, 1.0) * rim * (1.0 - mud);
        // Down in a rut, below its sides, less of the light reaches it; not under laid grass.
        albedo *= 1.0 - clamp(belowSides * matrices.marksLook.w, 0.0, 0.6) * (1.0 - grassy);
    }
    // Slow variation over the whole terrain, so a meadow is not one flat tone.
    albedo *= 0.85 + 0.3 * broad;
    albedo *= mix(1.0, occlusion, 0.35);

    // The material's normal on the ground's own frame: u runs along +x and v along +z, and the maps are OpenGL ones,
    // green towards the top of the image, where v is smallest.
    // The hummocks first, on the mesh's normal, faded out where they would only shimmer.
    if (!drawn) {
        vec2 bumps = noiseSlope(p, 0.6, 3) * 0.6;
        float fineBumps = detail(2.4, footprint);
        if (fineBumps > 0.001) {
            bumps += noiseSlope(p, 2.4, 2) * 0.15 * fineBumps;
        }
        N = normalize(N - vec3(bumps.x, 0.0, bumps.y) * bumpiness * detail(0.6, footprint));
    }
    // The ruts are a few centimetres deep, too narrow for the mesh: tilt the normal across the road by their slope,
    // the road's direction from how the distance to it changes.
    if (!drawn && road > 0.01) {
        const float e = 0.5;
        float along = (sfeTerrainNoise(terrainNoise, p + vec2(e, 0.0)).a - noise.a) * kSfeRoadTextureRange / e;
        float across = (sfeTerrainNoise(terrainNoise, p + vec2(0.0, e)).a - noise.a) * kSfeRoadTextureRange / e;
        vec2 towards = vec2(along, across);
        float rutSlope = 0.05 * rut * 2.0 * (roadAbs - 0.8) / (0.28 * 0.28) * sign(layers.roadDistance);
        N = normalize(N - vec3(towards.x, 0.0, towards.y) * rutSlope * road * detail(1.5, footprint));
    }
    N = normalize(T * detailNormal.x - B * detailNormal.y + N * detailNormal.z);
    // The marks' own slopes: down into a rut and over its rims.
    if (marked) {
        float e = max(footprint, 0.025);
        vec2 ex = vec2(e, 0.0);
        vec2 ez = vec2(0.0, e);
        vec2 marksSlope = vec2(
            sfeMarksHeight(marksAt + ex, dpdx, dpdy, marksCover) - sfeMarksHeight(marksAt - ex, dpdx, dpdy, marksCover),
            sfeMarksHeight(marksAt + ez, dpdx, dpdy, marksCover) - sfeMarksHeight(marksAt - ez, dpdx, dpdy, marksCover)) / (2.0 * e);
        N = normalize(N - T * marksSlope.x - B * marksSlope.y);
    }

    // Wet near the water, and where the marks churn the ground up, or press into it by the water and it seeps in:
    // glossier, a little darker; the mud the marks made is already both. Under it the water does the rest.
    float wet = clamp(layers.wet * (1.0 + pressed) + marksChurn * matrices.marksLook.z * (1.0 - grassy), 0.0, 1.0)
        * (1.0 - underwater);
    albedo *= mix(1.0, 0.85, wet * (1.0 - mud));
    roughness = min(roughness, mix(roughness, 0.35, wet));
    // Wet mud on the bank sheens, but less than a film of water: sheened all over, it mirrored the sky and read light.
    float specFactor = mix(0.08, 0.6, max(max(max(layers.wet, wet), mudWet) * (1.0 - mud), film));
    // Down in a mark, below its sides, its walls hide most of the sky: less of its light, and what a wet floor mirrors
    // at a low angle is the wall across, not the sky.
    float sunk = marked ? clamp(belowSides * matrices.marksLook.w, 0.0, 0.85) * (1.0 - grassy) : 0.0;
    // Laid blades sheen in the sky's light.
    specFactor = mix(specFactor, 0.18, laidSheen);
    // Slurry: water standing in a mark in wet ground, its mud stirred into it, filling it from its floor the more the
    // wetter and more churned it is: where the mark is deeper below its sides than it fills to, a level sheet of muddy
    // water, opaque brown and glossy, reflecting, its edge soaked darker into the mud round it. None on dry land. It
    // stands in the floor's low parts (sfeMarksFloor), the more of them the wetter: puddles along a rut, not one sheet.
    if (marked && glossy > 0.05) {
        float soaked = clamp(max(marksChurn, glossy) * soak, 0.0, 1.0) * (1.0 - underwater);
        float level = mix(0.06, 0.012, soaked);
        float lowest = 0.7 - 0.4 * soaked;
        float pool = smoothstep(level - 0.004, level + 0.012, belowSides) * smoothstep(0.15, 0.45, soaked)
            * smoothstep(lowest - 0.05, lowest + 0.05, sfeMarksFloor(marksAt));
        if (pool > 0.0) {
            const vec3 kSlurry = vec3(0.032, 0.024, 0.016); // linear: water with the mud stirred into it, brown
            float edge = 4.0 * pool * (1.0 - pool);
            albedo = mix(albedo * (1.0 - 0.45 * edge), kSlurry * mix(0.85, 1.1, fine), smoothstep(0.35, 0.8, pool));
            // Its face still and smooth: the sky in it nearly a mirror's, only the silt in it dulls it.
            roughness = mix(roughness, 0.08, pool);
            specFactor = mix(specFactor, 0.45, pool);
            N = normalize(mix(N, vec3(0.0, 1.0, 0.0), smoothstep(0.35, 0.8, pool)));
            // Its own level face sees more of the sky than the mud's walls round it.
            sunk *= 1.0 - 0.5 * pool;
        }
    }

    gNormal.xyz = N;
    gNormal.a = gl_FragCoord.z / gl_FragCoord.w;
    gAlbedoSpec.rgb = albedo;
    gAlbedoSpec.a = clamp(sfeFilteredRoughness(N, roughness, 1.0), 0.04, 1.0);
    gMaterial = sfeGBufferMaterial(0.0, specFactor, kSfeGround, 0.04);
    gVelocity = sfeGBufferVelocity(vClip, vPrevClip, sunk);
    gEmissive = vec4(0.0);
}
