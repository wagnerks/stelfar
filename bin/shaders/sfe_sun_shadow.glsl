#ifndef SFE_SUN_SHADOW_GLSL
#define SFE_SUN_SHADOW_GLSL
// The sun's shadow from its cascades: sun_shadow.fs for what is drawn (deferred_shading.fs reads it), water.fs for the
// water. The includer says where the three resources are bound, as SFE_SET(set, binding) of each: SFE_SUN_SHADOW_DEPTH
// (the cascades' depth), SFE_SUN_SHADOW_BLOCK (LightingPass's CascadedShadowBlock), SFE_SUN_SHADOW_MATRICES (the
// cascades' matrices).
// It may say too how the filter's disk is turned at this pixel, as SFE_SUN_SHADOW_TURN: x the turn, 0..1; y where
// in its ring each sample sits, 0..1. By default a noise that stands still, the rings' middles: water.fs. sun_shadow.fs
// gives each pixel of a 4x4 block a sixteenth of both (sfeBlockNoise), for its blur to put the sixteen back together.
#ifndef SFE_SUN_SHADOW_TURN
#define SFE_SUN_SHADOW_TURN vec2(sfeInterleavedGradientNoise(gl_FragCoord.xy, 0u), 0.5)
#endif

struct CascadData{
    // x 1/resolution, y world size of one texel, z depth bias, w samples
    vec4 cascade;
    // x light-space depth extent of this cascade, y the sun's width (the penumbra's metres a metre from the caster),
    // z the widest penumbra, metres, w how far light spreads under the skin, metres
    vec4 cascadeExtra;
};

layout(SFE_SUN_SHADOW_DEPTH) uniform sampler2DArray shadowMapDepth;

layout(SFE_SUN_SHADOW_BLOCK, std140) uniform cascadedShadowBlock {
    vec4 direction_intensity; // xyz direction, w intensity
    vec4 color_and_misc; 
    CascadData data[6];
    vec4 cascadeSplits;
    vec4 cascadeSplits2;
} cascadedShadow;

layout(SFE_SUN_SHADOW_MATRICES, std140) uniform LightSpaceMatrices {
    mat4 lightSpaceMatrices[6];
};

vec2 vogel_disk_sample(uint sample_index, int sample_count, float angle, float ring) {
  const float golden_angle = 2.399963f; // radians
  float r                  = sqrt(sample_index + ring) / sqrt(sample_count);
  float theta              = sample_index * golden_angle + angle;
  float sine, cosine;

  sine = sin(theta);
  cosine = cos(theta);
  return vec2(cosine, sine) * r;
}

vec2 vogel_disk_sample(uint sample_index, int sample_count, float angle) {
  return vogel_disk_sample(sample_index, sample_count, angle, 0.5);
}

// The mean gap, metres along the light, between this point and what stands between it and the sun, -1 where nothing
// does. Of what lies within `searchUv` of it only what is far enough over it for its penumbra to reach this far
// sideways: round the foot of a trunk the trunk's top, a stride off across the map, made the shadow there as soft as
// its tip's.
float blockerGap(int layer, vec3 proj, float searchUv, float uvTexel, float worldTexel, float depthRange,
                 float sunWidth, float angle, float ring) {
    const int BLOCKER_SAMPLES = 16;
    float sum = 0.0;
    int count = 0;
    for (int i = 0; i < BLOCKER_SAMPLES; ++i) {
        vec2 offset = vogel_disk_sample(uint(i), BLOCKER_SAMPLES, angle, ring) * searchUv;
        float depth = texture(shadowMapDepth, vec3(proj.xy + offset, float(layer))).r;
        float gap = (proj.z - depth) * depthRange;
        // Metres aside, a texel and a half spared: what stands on the point itself always counts.
        float aside = length(offset) / uvTexel * worldTexel;
        if (gap > 0.5 * worldTexel && aside <= 0.5 * gap * sunWidth + 1.5 * worldTexel) {
            sum += gap;
            ++count;
        }
    }
    if (count == 0) {
        return -1.0;
    }
    return sum / float(count);
}

// The four texels round `uv`, each lit where what it stores is not nearer the sun than `depth`; bilinear between them,
// as the comparison sampler did.
float litAt(int layer, vec2 uv, float depth, float texels) {
    vec2 t = uv * texels - 0.5;
    vec2 base = floor(t);
    vec2 f = t - base;
    vec4 stored = textureGather(shadowMapDepth, vec3((base + 1.0) / texels, float(layer))); // (0,1) (1,1) (1,0) (0,0)
    vec4 lit = step(vec4(depth), stored);
    return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
}

// The filter's least radius, texels: an edge sharper than the map can draw it showed the map's texels as stairs along
// it -- under a chin, over a shoulder.
const float kLeastTexels = 1.5;

// `leastUv`: the filter's least radius (sampleCascade).
float TechniqueVogelCascaded(int layer, float bias, vec3 projCoords, float leastUv) {
    if (projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 0.0;
    }

    vec2 turn = SFE_SUN_SHADOW_TURN;
    float angle = turn.x * 6.28318530718;
    float uvTexel = max(cascadedShadow.data[layer].cascade.x, 1.0 / 4096.0);
    projCoords.z -= bias;

    const int sampleCount = clamp(int(cascadedShadow.data[layer].cascade.w + 0.5), 4, 16);

    // Contact hardening: sharp where the shadow leaves what casts it, wider the further it falls from it, as the sun's
    // width (cascadeExtra.y, the penumbra's metres a metre from the caster) spreads it, up to the widest penumbra
    // (cascadeExtra.z, metres). It was held to ten texels: a few centimetres near the eye, so near shadows were all
    // sharp and the far ones soft only by their cascade's coarser texels.
    // Never sharper than leastUv.
    float worldTexel = max(cascadedShadow.data[layer].cascade.y, 1e-6);
    float depthRange = max(cascadedShadow.data[layer].cascadeExtra.x, 1e-4);
    float sunWidth = cascadedShadow.data[layer].cascadeExtra.y;
    float widestUv = max(0.5 * cascadedShadow.data[layer].cascadeExtra.z / worldTexel * uvTexel, leastUv);
    float radius = leastUv;
    float gap = blockerGap(layer, projCoords, widestUv, uvTexel, worldTexel, depthRange, sunWidth, angle, turn.y);
    if (gap > 0.0) {
        radius = clamp(0.5 * gap * sunWidth / worldTexel * uvTexel, leastUv, widestUv);
    }
    float shadow = 0.0;
    int used = 0;
    for (uint i = 0; i < uint(sampleCount); ++i) {
        vec2 offset = vogel_disk_sample(i, sampleCount, angle, turn.y) * radius;
        vec2 uv = projCoords.xy + offset;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            continue;
        }
        float lit = litAt(layer, uv, projCoords.z, 1.0 / uvTexel);
        shadow += 1.0 - lit;
        used += 1;
    }
    if (used == 0) {
        return 0.0;
    }
    return shadow / float(used);
}

vec3 worldToShadowUv(int layer, vec3 worldPos) {
    vec4 light = lightSpaceMatrices[layer] * vec4(worldPos, 1.0);
    vec3 proj = light.xyz / light.w;
    proj.xy = proj.xy * 0.5 + 0.5;
    proj.z = proj.z * 0.5 + 0.5;
    return proj;
}

// The most a surface edge-on to the sun is looked up off itself along its normal, metres (sampleCascade).
const float kTurnedReachMost = 0.02;

// `card`: a blade or a leaf, drawn into the map from both sides.
float sampleCascade(int layer, vec3 worldPos, vec3 Normal, bool card, float scatter) {
    // Compared flat, with the point's own depth: of a closed shape the map keeps only the sides turned from the sun
    // (CascadedShadowPass culls those facing it), so a side being lit is never in it to shadow itself -- but for a
    // coarse mesh's triangle just turned from the sun while its smoothed normal still faces it, which the casters' slope
    // bias (GraphicsSettings::shadowSlopeBias) pushes off past the filter. The lookup is
    // left where it is but for a quarter texel along the normal, for a surface that bends within a texel; the residual
    // depth bias, a tenth of the authored texels (cascade.z), is what rounding leaves. A card is in the map itself: by
    // the authored texels whole. A plane drawn from each triangle's depth, as this compared with before, was that
    // triangle's alone: over the filter on a finger or a cheek it passed behind the far side, and the shadow there came
    // in facets, triangle by triangle.
    // A round thing turning from the sun (a head, an arm, a bottle) meets its own far side in the map within the
    // filter's reach of its outline, and shadowed itself in a band along the edge the light leaves. Looked up off
    // itself the more it is turned, by the filter's radius and the texel it reads round each sample, out past its own
    // outline. Never more than two centimetres, which nothing's shadow shows standing off its foot; a far cascade's
    // texels are decimetres.
    // Skin (`scatter`) is lit by the light that went in under it a few millimetres round, shadow and all: its filter
    // never narrower than that (cascadeExtra.w). A shadow's edge on it is as soft, and its texture as sharp as it was.
    // A side turned from the sun is what the map keeps (it draws the sides turned away), so compared with itself it came
    // out lit or shadowed by rounding, triangle by triangle: blotches in the light that wraps on past the sun's edge
    // (deferred_shading.fs). Its push away from the sun along the normal is taken back, and it is looked up as far
    // again towards the sun as it is turned away, clear of itself: shadowed only by what stands between it and the sun.
    float worldTexel = cascadedShadow.data[layer].cascade.y;
    float uvTexel = max(cascadedShadow.data[layer].cascade.x, 1.0 / 4096.0);
    float depthRange = max(cascadedShadow.data[layer].cascadeExtra.x, 1e-4);
    vec3 sunward = -normalize(cascadedShadow.direction_intensity.xyz);
    float facing = -dot(Normal, sunward);
    float turned = sqrt(max(1.0 - facing * facing, 0.0));
    float least = max(kLeastTexels * worldTexel, scatter * cascadedShadow.data[layer].cascadeExtra.w);
    float reach = min(least + worldTexel, kTurnedReachMost);
    float off = 0.25 * worldTexel + reach * turned;
    vec3 at = worldPos + Normal * off;
    at += sunward * max(facing, 0.0) * (off + reach);
    vec3 proj = worldToShadowUv(layer, at);
    float bias = (card ? 1.0 : 0.1) * cascadedShadow.data[layer].cascade.z * worldTexel / depthRange;
    return TechniqueVogelCascaded(layer, bias, proj, least / worldTexel * uvTexel);
}

bool shadowUvOk(vec3 proj) {
    return proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 && proj.z >= 0.0 && proj.z <= 1.0;
}

// The first cascade whose own circle holds this fragment, and how far out in that circle it sits: 0 at
// the centre, 1 at the rim. Each cascade is fitted to a circle around its slice of the view (the first round the
// player's body when there is one, RenderData::shadowFocus) -- that is
// what holds the fit still while the camera turns -- and the square map only circumscribes it, so the
// circle is the region the cascade was built for. Choosing by distance along the view direction instead,
// as this did, cuts the ground into slabs across the view, and the step between cascades then runs as a
// straight band over the floor. A straight line is the one shape the eye finds in a scene that has none.
int selectCascade(vec3 worldPos, int cc, out float edge) {
    edge = 1.0;
    for (int i = 0; i < cc; ++i) {
        vec3 proj = worldToShadowUv(i, worldPos);
        if (proj.z < 0.0 || proj.z > 1.0) {
            continue;
        }
        float r = length(proj.xy * 2.0 - 1.0);
        if (r <= 1.0) {
            edge = r;
            return i;
        }
    }
    // Past the last circle. The rim distance carries on past 1 here, so the caller can fade the shadow
    // out instead of letting it stop at the edge of the square map.
    vec3 last = worldToShadowUv(cc - 1, worldPos);
    edge = length(last.xy * 2.0 - 1.0);
    return cc - 1;
}

// `card`: a blade or a leaf (sampleCascade). `scatter`: how much the surface scatters light under it, 0 to 1
// (sfeScatters).
float ShadowCascadedCalculation(vec3 fragPosWorldSpace, vec3 Normal, bool card, float scatter)
{
    int cc = int(cascadedShadow.color_and_misc.w + 0.5);
    if (cc < 1) {
        cc = 4;
    }

    float edge = 1.0;
    int layer = selectCascade(fragPosWorldSpace, cc, edge);
    // Past the last circle the shadow has faded out all the way (below): nothing to look up there.
    if (layer + 1 >= cc && edge >= 1.0) {
        return 0.0;
    }
    // Faded over the outer fifth of the circle, so the change of resolution arrives as a ring that grows
    // out of the ground rather than an edge.
    float blend = smoothstep(0.8, 1.0, edge);
    // Only cross-fade into the next cascade when the fragment actually lands inside it.
    // TechniqueVogelCascaded answers "lit" for out-of-range coordinates, so blending towards a
    // cascade that does not cover this fragment faded real shadows out at every boundary.
    bool fade = layer + 1 < cc && blend > 0.0 && shadowUvOk(worldToShadowUv(layer + 1, fragPosWorldSpace));
    // Both cascades looked up and mixed. One of the two picked a pixel by a noise turned each frame, for TAA to average,
    // left the ring flickering between a sharp shadow and a soft one; and it went where the view went, so a standing
    // player's breath swept it over the ground.
    float shadow = sampleCascade(layer, fragPosWorldSpace, Normal, card, scatter);
    if (layer + 1 >= cc) {
        // Nothing past the last cascade to hand over to. Left alone, the shadow stops wherever its square
        // map ends -- four straight lines across the ground, slanted with the light rather than the view.
        // Faded out over the rim of its circle instead, the same shape every other step now has.
        return shadow * (1.0 - smoothstep(0.8, 1.0, edge));
    }

    return fade ? mix(shadow, sampleCascade(layer + 1, fragPosWorldSpace, Normal, card, scatter), blend) : shadow;
}

#endif
