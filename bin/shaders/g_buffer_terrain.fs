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
// Each material's middle height, so the soil shows through each as much at the same wear, though the litter lies low.
const float kHeightMid[kLayers] = float[](0.63, 0.51, 0.52, 0.54, 0.51, 0.18, 0.4, 0.45, 0.51, 0.45);
// The painted maps are gentle; lit by a low sun they read as flat without some help.
const float kNormalStrength = 0.9;
// Hummocks, ruts and dips a metre or two across: the ground's own unevenness between the mesh's vertices.
const float kBumps[kLayers] = float[](0.35, 0.45, 0.3, 0.12, 0.5, 0.4, 0.3, 0.15, 0.35, 0.35);
// Dry soil, linear, a grey umber, and the moss's earth's mean brightness, which the soil's specks and blotches are
// measured against.
const vec3 kSoilColour = vec3(0.17, 0.115, 0.068);
const float kEarthLuma = 0.2;

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
    // The bank by how dirty it and the water by it are: clean, pebbles with sand in its coves; dirty, mud down by the
    // water and bare earth above it.
    vec3 areaTint = sfeAreaTint(areaTintMap, matrices.tintArea, FragPos.xz);
    float dirty = clamp((max(areaTint.r, areaTint.g) - matrices.tintShore.x)
        / max(matrices.tintShore.y - matrices.tintShore.x, 1e-3), 0.0, 1.0);
    float shore = open * bank;
    float cove = open * layers.beach;
    float byWater = 1.0 - smoothstep(0.25, 0.6, height + 0.4 * (fine - 0.5));
    weight[kSand] = cove * (1.0 - dirty);
    weight[kPebbles] = (shore - cove) * (1.0 - dirty);
    weight[kMud] += shore * dirty * byWater;
    weight[kMoss] += shore * dirty * (1.0 - byWater);
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
    // land is foul: the curve the trees go bare along (tintShore: VegetationSystem::wither's), the grey their bark takes
    // (sfe_gbuffer_surface). Its green is the lighting's to tint, as all green is (deferred_shading).
    if (weight[kLitter] >= least) {
        float foul = clamp((areaTint.r - matrices.tintShore.x) / max(matrices.tintShore.y - matrices.tintShore.x, 1e-3),
                           0.0, 1.0);
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
    tint[kMud] *= mix(1.0, 0.55, layers.wet);
    tint[kSand] *= mix(1.0, 0.55, layers.wet);
    tint[kPebbles] *= mix(1.0, 0.55, layers.wet);
    // Under the water the silt is grey-green and the algae an olive slime.
    tint[kMud] *= mix(vec3(1.0), vec3(0.75, 0.85, 0.65), underwater);
    tint[kMoss] *= mix(vec3(1.0), vec3(0.8, 0.85, 0.45), underwater);
    tint[kTrack] *= mix(1.0, 0.6, rut * 0.8 * road);
    // The soil darker in damp spots, where something was spilt or the shade keeps it.
    float damp = soil > 0.01 ? smoothstep(0.6, 0.75, sfeFbm(p * 0.45 + 61.0, 2)) : 0.0;
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

    // Wet near the water: glossier, a little darker. Under it the water does the rest.
    albedo *= mix(1.0, 0.85, layers.wet * (1.0 - underwater));
    roughness = mix(roughness, 0.35, layers.wet * (1.0 - underwater));
    float specFactor = mix(0.08, 0.6, layers.wet);

    gNormal.xyz = N;
    gNormal.a = gl_FragCoord.z / gl_FragCoord.w;
    gAlbedoSpec.rgb = albedo;
    gAlbedoSpec.a = clamp(sfeFilteredRoughness(N, roughness, 1.0), 0.04, 1.0);
    gMaterial = sfeGBufferMaterial(0.0, specFactor, kSfeGround, 0.04);
    gVelocity = sfeGBufferVelocity(vClip, vPrevClip, 0.0);
}
