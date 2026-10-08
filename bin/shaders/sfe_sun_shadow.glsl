#ifndef SFE_SUN_SHADOW_GLSL
#define SFE_SUN_SHADOW_GLSL
// The sun's shadow from its cascades: sun_shadow.fs for what is drawn (deferred_shading.fs reads it), water.fs for the
// lake. The includer says where the three resources are bound, as SFE_SET(set, binding) of each: SFE_SUN_SHADOW_DEPTH
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
    // x light-space depth extent of this cascade, the rest spare
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
float blockerGap(int layer, vec3 proj, vec2 slope, float searchUv, float uvTexel, float worldTexel, float depthRange,
                 float sunWidth, float angle, float ring) {
    const int BLOCKER_SAMPLES = 16;
    float sum = 0.0;
    int count = 0;
    for (int i = 0; i < BLOCKER_SAMPLES; ++i) {
        vec2 offset = vogel_disk_sample(uint(i), BLOCKER_SAMPLES, angle, ring) * searchUv;
        float depth = texture(shadowMapDepth, vec3(proj.xy + offset, float(layer))).r;
        // Measured from the receiver's own plane there (receiverSlope): the slope it lies on is not over it.
        float gap = (proj.z + dot(slope, offset) - depth) * depthRange;
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

// How the receiver's depth in this cascade's map changes across the map, from two steps along its surface (tx, ty,
// world): d(depth)/d(uv) of its plane. Each texel is compared with the surface at the texel's own centre (litAt), so
// neither the filter's width nor the texel's needs a bias -- the bias it took (texels of depth, and as many again along
// the normal) stood every shadow off its caster's foot, centimetres near the eye and decimetres further out.
vec2 receiverSlope(int layer, vec3 tx, vec3 ty, float uvTexel, float worldTexel, float depthRange) {
    mat3 m = mat3(lightSpaceMatrices[layer]);
    vec3 a = 0.5 * (m * tx);
    vec3 b = 0.5 * (m * ty);
    float det = a.x * b.y - a.y * b.x;
    if (abs(det) < 1e-24) {
        return vec2(0.0);
    }
    vec2 slope = vec2(a.z * b.y - b.z * a.y, b.z * a.x - a.z * b.x) / det;
    // Held to a surface ten times steeper than its rise along the light: one turned edge-on has none to speak of.
    float most = 10.0 * worldTexel / uvTexel / depthRange;
    float len = length(slope);
    return len > most ? slope * (most / len) : slope;
}

// The four texels round `uv`, each lit where what it stores is not nearer the sun than the receiver's plane at the
// texel's own centre less `bias`; bilinear between them, as the comparison sampler did.
float litAt(int layer, vec2 uv, vec3 proj, vec2 slope, float bias, float texels) {
    vec2 t = uv * texels - 0.5;
    vec2 base = floor(t);
    vec2 f = t - base;
    vec4 stored = textureGather(shadowMapDepth, vec3((base + 1.0) / texels, float(layer))); // (0,1) (1,1) (1,0) (0,0)
    vec2 c = (base + 0.5) / texels - proj.xy;
    float one = 1.0 / texels;
    vec4 receiver = proj.z - bias + vec4(dot(slope, c + vec2(0.0, one)), dot(slope, c + vec2(one, one)),
                                          dot(slope, c + vec2(one, 0.0)), dot(slope, c));
    vec4 lit = step(receiver, stored);
    return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
}

float TechniqueVogelCascaded(int layer, float bias, vec3 projCoords, vec2 slope) {
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
    float worldTexel = max(cascadedShadow.data[layer].cascade.y, 1e-6);
    float depthRange = max(cascadedShadow.data[layer].cascadeExtra.x, 1e-4);
    float sunWidth = cascadedShadow.data[layer].cascadeExtra.y;
    float widestUv = max(0.5 * cascadedShadow.data[layer].cascadeExtra.z / worldTexel, 1.5) * uvTexel;
    float radius = uvTexel * 0.7;
    float gap = blockerGap(layer, projCoords, slope, widestUv, uvTexel, worldTexel, depthRange, sunWidth, angle, turn.y);
    if (gap > 0.0) {
        radius = clamp(0.5 * gap * sunWidth / worldTexel * uvTexel, uvTexel * 0.7, widestUv);
    }
    float shadow = 0.0;
    int used = 0;
    for (uint i = 0; i < uint(sampleCount); ++i) {
        vec2 offset = vogel_disk_sample(i, sampleCount, angle, turn.y) * radius;
        vec2 uv = projCoords.xy + offset;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            continue;
        }
        float lit = litAt(layer, uv, projCoords, slope, 0.0, 1.0 / uvTexel);
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

float sampleCascade(int layer, vec3 worldPos, vec3 Normal, vec3 tx, vec3 ty) {
    // Compared with the receiver's own plane texel by texel (receiverSlope, litAt), the lookup is left where it is but
    // for a quarter texel along the normal, for a surface that bends within a texel; the residual depth bias, a tenth of
    // the authored texels (cascade.z), is what rounding leaves. A receiver with no steps (a blade, a leaf) has no plane:
    // compared flat, by the authored texels whole.
    float worldTexel = cascadedShadow.data[layer].cascade.y;
    float uvTexel = max(cascadedShadow.data[layer].cascade.x, 1.0 / 4096.0);
    float depthRange = max(cascadedShadow.data[layer].cascadeExtra.x, 1e-4);
    vec3 proj = worldToShadowUv(layer, worldPos + Normal * (0.25 * worldTexel));
    bool plane = dot(tx, tx) > 0.0;
    vec2 slope = plane ? receiverSlope(layer, tx, ty, uvTexel, worldTexel, depthRange) : vec2(0.0);
    float bias = (plane ? 0.1 : 1.0) * cascadedShadow.data[layer].cascade.z * (worldTexel / depthRange + uvTexel * length(slope));
    return TechniqueVogelCascaded(layer, bias, proj, slope);
}

bool shadowUvOk(vec3 proj) {
    return proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 && proj.z >= 0.0 && proj.z <= 1.0;
}

// The first cascade whose own circle holds this fragment, and how far out in that circle it sits: 0 at
// the centre, 1 at the rim. Each cascade is fitted to a circle around its slice of the view -- that is
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

float ShadowCascadedCalculation(vec3 fragPosWorldSpace, vec3 Normal, vec3 tx, vec3 ty)
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
    float shadow = sampleCascade(layer, fragPosWorldSpace, Normal, tx, ty);
    if (layer + 1 >= cc) {
        // Nothing past the last cascade to hand over to. Left alone, the shadow stops wherever its square
        // map ends -- four straight lines across the ground, slanted with the light rather than the view.
        // Faded out over the rim of its circle instead, the same shape every other step now has.
        return shadow * (1.0 - smoothstep(0.8, 1.0, edge));
    }

    return fade ? mix(shadow, sampleCascade(layer + 1, fragPosWorldSpace, Normal, tx, ty), blend) : shadow;
}

#endif
