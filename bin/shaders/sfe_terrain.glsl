#ifndef SFE_TERRAIN_GLSL
#define SFE_TERRAIN_GLSL
// What covers the ground where: the terrain shader paints by it, and grass grows by it, so the blades stand exactly
// where the ground under them is grass. Include after sfe.glsl.

struct SfeTerrainLayers {
    float broad;      // large-scale variation, 0..1
    float patches;    // mid-scale variation, 0..1
    float underwater; // weights, 0..1, bottom up
    float wet;
    float sand;       // the bank where it is sandy: most of it
    float mud;        // the rest of the bank: patches of wet silt
    float beach;      // the open sandy coves among the sand, where no reed grows
    float rock;
    float earth;      // moss among the grass, and in patches on the forest floor
    float forest;     // how deep in the forest, 0..1, from the forest map
    float litter;     // fallen needles and twigs under the trees
    float roadDistance; // metres from the road's centreline, signed by its side; clearings are not in it
    float road;       // the road's surface
};

// Metres a texel of the forest map covers; VegetationComponent.h has the same.
const float kSfeForestTexelMetres = 2.0;

// Metres a texel of the terrain noise covers; Terrain.h has the same.
const float kSfeTerrainNoiseTexelMetres = 4.0;

// Metres of signed road distance the noise texture's alpha spans; Terrain.h has the same.
const float kSfeRoadTextureRange = 16.0;

// The terrain's slow noise at `p`, baked by terrainNoiseTexture: r broad variation, g sandy coves, b tile variation,
// a the signed distance to the road. Away from the road its two sides meet again, past its ends and where the road is
// no longer looked for, and there the sign turns over between two texels far from it; blended as it is, that would
// draw a thin road between them. Two texels of a true distance are never further apart in it than in place, so where
// they are, the distance is blended without its sign.
vec4 sfeTerrainNoise(sampler2D noiseMap, vec2 p) {
    float side = float(textureSize(noiseMap, 0).x);
    vec2 uv = p / (side * kSfeTerrainNoiseTexelMetres) + 0.5;
    vec4 noise = textureLod(noiseMap, uv, 0.0);
    // The four texels the filter blends: (0, 1), (1, 1), (1, 0), (0, 0).
    vec4 road = (textureGather(noiseMap, uv, 3) - 0.5) * kSfeRoadTextureRange;
    float lowest = min(min(road.x, road.y), min(road.z, road.w));
    float highest = max(max(road.x, road.y), max(road.z, road.w));
    if (lowest < 0.0 && highest > 0.0 && highest - lowest > 1.5 * kSfeTerrainNoiseTexelMetres) {
        vec2 f = fract(uv * side - 0.5);
        road = abs(road);
        noise.a = mix(mix(road.w, road.z, f.x), mix(road.x, road.y, f.x), f.y) / kSfeRoadTextureRange + 0.5;
    }
    return noise;
}

// Where along the road `p` is, from terrainRoadFrameTexture: x metres across it, y metres along it. `jacobian` gets
// how those change along world x (its first column) and z (its second). Filtered here in full precision: the texture
// unit blends texels with a few bits of weight, which would step a texture laid by it a centimetre or two at a time.
vec2 sfeRoadFrame(sampler2D frameMap, vec2 p, out mat2 jacobian) {
    ivec2 side = textureSize(frameMap, 0);
    vec2 texel = p / kSfeTerrainNoiseTexelMetres + vec2(side) * 0.5 - 0.5;
    ivec2 i = ivec2(floor(texel));
    vec2 f = texel - vec2(i);
    ivec2 last = side - 1;
    vec2 a = texelFetch(frameMap, clamp(i, ivec2(0), last), 0).rg;
    vec2 b = texelFetch(frameMap, clamp(i + ivec2(1, 0), ivec2(0), last), 0).rg;
    vec2 c = texelFetch(frameMap, clamp(i + ivec2(0, 1), ivec2(0), last), 0).rg;
    vec2 d = texelFetch(frameMap, clamp(i + ivec2(1, 1), ivec2(0), last), 0).rg;
    jacobian = mat2(mix(b - a, d - c, f.y), mix(c - a, d - b, f.x)) / kSfeTerrainNoiseTexelMetres;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Metres from a footpath past which it no longer matters; Terrain.h has the same.
const float kSfePathFar = 8.0;

// From terrainPathTexture: x metres past the edge of the nearest footpath or patch (below zero on one, ragged), y past
// the nearest clearing's, z how worn the ground is (terrainPathWear), w metres into a path's bare strip (its core);
// kSfePathFar, kSfePathFar, 0, -kSfePathFar outside the box it covers. Its first row says where that is.
vec4 sfeTerrainPath(sampler2D pathMap, vec2 p) {
    vec2 origin = texelFetch(pathMap, ivec2(0, 0), 0).rg;
    float metres = texelFetch(pathMap, ivec2(1, 0), 0).r;
    ivec2 side = textureSize(pathMap, 0) - ivec2(0, 1);
    vec2 texel = (p - origin) / max(metres, 1e-4);
    if (metres <= 0.0 || any(lessThan(texel, vec2(0.0))) || any(greaterThanEqual(texel, vec2(side - 1)))) {
        return vec4(kSfePathFar, kSfePathFar, 0.0, -kSfePathFar);
    }
    ivec2 i = ivec2(texel);
    vec2 f = texel - vec2(i);
    vec4 a = texelFetch(pathMap, i + ivec2(0, 1), 0);
    vec4 b = texelFetch(pathMap, i + ivec2(1, 1), 0);
    vec4 c = texelFetch(pathMap, i + ivec2(0, 2), 0);
    vec4 d = texelFetch(pathMap, i + ivec2(1, 2), 0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// How far into bare earth ground worn `wear` with a path's strip `core` is at `p`: above zero bare, below it grass, the
// two mixing a tenth or so either side. Spots that spread and join as the wear rises, all of it bare at 1, and the
// strip; their edges broken by a finer noise. Terrain.cpp's terrainBareEarth is the same, and the grass clumps grow by
// that.
float sfeTerrainBare(float wear, float core, vec2 p) {
    float reach = 0.47 * wear + 0.2 * smoothstep(0.2, 0.5, wear) + 0.3 * smoothstep(0.8, 1.0, wear);
    return max(reach - sfeFbm(p * 0.5 + 23.0, 3), 1.5 * core) - 0.1 * (sfeFbm(p * 3.0 + 7.0, 2) - 0.5);
}

// How deep in the forest `p` (the terrain's own x/z) is, from VegetationPlacementsComponent::forestMap: centred on
// the terrain, one texel every kSfeForestTexelMetres.
float sfeForest(sampler2D forestMap, vec2 p) {
    float side = float(textureSize(forestMap, 0).x);
    return textureLod(forestMap, p / (side * kSfeForestTexelMetres) + 0.5, 0.0).r;
}

// How dirty the bank is by the area tint map's value there, `tint` (sfeAreaTint): by the larger of the land's and the
// water's, from shore.x (clean) to shore.y (all mud) (AreaTintFrame::shore).
float sfeShoreDirty(vec3 tint, vec4 shore) {
    return clamp((max(tint.r, tint.g) - shore.x) / max(shore.y - shore.x, 1e-3), 0.0, 1.0);
}

// How much of the clean bank `height` above the water is stones and sand by the water rather than grass above them,
// `fine` sfeFbm(p * 0.35, 2) raggedening where the one gives way to the other, 0.25 to 0.6 metres up.
float sfeShoreByWater(float height, float fine) {
    return 1.0 - smoothstep(0.25, 0.6, height + 0.4 * (fine - 0.5));
}

// How much of the ground `height` above the water is in the band along the lake the litter lies in, up to `top`
// metres over it (AreaTintFrame::shore.z), its upper edge ragged by `fine` (sfeFbm(p * 0.35, 2)). Where the bank is
// dirty (sfeShoreDirty), all of it is mud. None above `top` + 0.45.
float sfeShoreBand(float height, float fine, float top) {
    return 1.0 - smoothstep(top, top + 0.3, height + 0.3 * (fine - 0.5));
}

// `p` world x/z in metres, `height` above the water line, `slope` 1 - the normal's y.
// `noise` is sfeTerrainNoise at `p`.
SfeTerrainLayers sfeTerrainLayers(vec2 p, float height, float slope, float forest, vec4 noise) {
    SfeTerrainLayers layers;
    layers.broad = noise.r;
    layers.patches = sfeFbm(p * 0.11 + 13.0, 3);
    float shoreLine = 0.35 + 0.55 * layers.patches;
    layers.underwater = 1.0 - smoothstep(-0.7, -0.15, height);
    layers.wet = 1.0 - smoothstep(-0.15, 0.3, height);
    float bank = 1.0 - smoothstep(shoreLine - 0.6, shoreLine + 0.15, height);
    float sandy = smoothstep(0.3, 0.45, noise.g);
    layers.sand = bank * sandy;
    layers.mud = bank * (1.0 - sandy);
    layers.beach = bank * smoothstep(0.5, 0.72, noise.g);
    layers.rock = smoothstep(0.33, 0.6, slope + 0.12 * (layers.patches - 0.5));
    layers.forest = forest;
    // Under the trees the grass gives way to roots, broken by the odd patch of bare earth; ragged where the forest thins.
    float underTrees = smoothstep(0.2, 0.7, forest + 0.25 * (layers.patches - 0.5)) * (1.0 - bank);
    float mossy = smoothstep(0.58, 0.82, layers.patches) * underTrees;
    layers.earth = max(smoothstep(0.6, 0.88, layers.broad * 0.6 + layers.patches * 0.5), mossy);
    layers.litter = underTrees * (1.0 - mossy);
    layers.roadDistance = (noise.a - 0.5) * kSfeRoadTextureRange;
    // A ragged edge where the verge grass creeps in.
    layers.road = 1.0 - smoothstep(1.2, 2.6 + 0.6 * layers.patches, abs(layers.roadDistance));
    return layers;
}

// The linear albedo of the grass, lush where `broad` is low and dry where it is high; `fine` in 0..1 shifts it a little.
vec3 sfeTerrainGrass(float broad, float fine) {
    const vec3 lush = vec3(0.028, 0.062, 0.016);
    const vec3 dry = vec3(0.070, 0.085, 0.030);
    return mix(lush, dry, smoothstep(0.4, 0.8, broad + 0.25 * (fine - 0.5)));
}

#endif
