#version 450 core
#include "sfe.glsl"
#include "sfe_sky.glsl"

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D gMaterial;
layout(SFE_SET(0, 4)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 1)) uniform sampler2D gNormal;
layout(SFE_SET(0, 2)) uniform sampler2D gAlbedoSpec;
layout(SFE_SET(0, 3)) uniform sampler2D ssao;
layout(SFE_SET(0, 5)) uniform sampler2D gOutlines;

layout(SFE_SET(0, 6)) uniform sampler2DArray shadowMapDepth;
layout(SFE_SET(0, 7)) uniform sampler2DArrayShadow PointLightShadowMapArray;
layout(SFE_SET(0, 8)) uniform sampler2DArrayShadow shadowMap;
layout(SFE_SET(0, 9)) uniform sampler2D gVelocity; // b: what a material's occlusion map shuts out (sfeGBufferVelocity)
layout(SFE_SET(0, 12)) uniform samplerCube irradianceMap;
layout(SFE_SET(0, 13)) uniform samplerCube prefilterMap;
layout(SFE_SET(0, 14)) uniform sampler2D brdfLUT;
layout(SFE_SET(0, 21)) uniform sampler2D cloudLayer;
layout(SFE_SET(0, 22)) uniform sampler2D areaTintMap;

layout(SFE_SET(1, 15), std140) uniform PushConstants {
    vec3 viewPos;
    float shadowIntensity;

    float fogStart;
    float drawDistance;
    int pointLightsSize;
    int cascadeCount;

    vec3 ambientColor;
    float ssaoAmount;

    vec3 viewDir;
    float iblStrength;
    float shadowAmbient;
    float padLook0;
    float padLook1;
    float debugTerm;
    float sheenStrength;
    float rimStrength;
    float taaFrame;  // -1 without TAA
    float cloudCoverage;
    // How far the picture is pushed from what the light really does towards a painted one: the sun's light in steps
    // rather than a gradient, no highlight to speak of, and the colour flattened.
    float stylise;
    float ssaoSun; // the share of the AO the sun's light takes
    float pad1, pad2;
    // The area tint (AreaTintFrame): the map's place, and the foliage's and the air's colour and strength.
    vec4 tintArea;
    vec4 tintFoliage;
    vec4 tintAir; // a the haze per metre where the map is full
    vec4 tintGround;
} pc;


struct PointLight {
    vec3 Position;
    vec2 texelSize;
    float bias;
    int samples;
    float radius;
    int offset; //all point lights store in one 2D array, and one matricec array, so here should be offset for matrix and shadowMap layer

    vec3 Color;
    float Linear;
    float Quadratic;
    int Type; //0 - point, 1 - directional
    int Layers;
};

const int MAX_POINT_LIGHTS_SIZE = 12;

layout(SFE_SET(1, 10), std140) uniform pointLightsBlock {
    PointLight pointLight[MAX_POINT_LIGHTS_SIZE];
} plb;

struct CascadData{
    // x 1/resolution, y world size of one texel, z depth bias, w samples
    vec4 cascade;
    // x light-space depth extent of this cascade, the rest spare
    vec4 cascadeExtra;
};

layout(SFE_SET(1, 9), std140) uniform cascadedShadowBlock {
    vec4 direction_intensity; // xyz direction, w intensity
    vec4 color_and_misc; 
    CascadData data[6];
    vec4 cascadeSplits;
    vec4 cascadeSplits2;
} cascadedShadow;

layout(SFE_SET(1, 11), std140) uniform LightSpaceMatrices {
    mat4 lightSpaceMatrices[6];
};

layout(SFE_SET(1, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(SFE_SET(1, 3), std140) uniform PointLightMatrices {
    mat4 pointLightMatrices[72];
};

vec2 vogel_disk_sample(uint sample_index, int sample_count, float angle) {
  const float golden_angle = 2.399963f; // radians
  float r                  = sqrt(sample_index + 0.5f) / sqrt(sample_count);
  float theta              = sample_index * golden_angle + angle;
  float sine, cosine;

  sine = sin(theta);
  cosine = cos(theta);
  return vec2(cosine, sine) * r;
}

float findAverageBlockerDepth(int layer, vec3 proj, float searchRadius, float angle) {
    const int BLOCKER_SAMPLES = 8;
    float sum = 0.0;
    int count = 0;
    for (int i = 0; i < BLOCKER_SAMPLES; ++i) {
        vec2 offset = vogel_disk_sample(uint(i), BLOCKER_SAMPLES, angle) * searchRadius;
        float depth = texture(shadowMapDepth, vec3(proj.xy + offset, float(layer))).r;
        if (depth < proj.z) {
            sum += depth;
            ++count;
        }
    }
    if (count == 0) {
        return -1.0;
    }
    return sum / float(count);
}

float TechniqueVogelCascaded(int layer, float bias, vec3 projCoords) {
    if (projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 0.0;
    }

    float angle = sfeInterleavedGradientNoise(gl_FragCoord.xy, 0u) * 6.28318530718;
    float uvTexel = max(cascadedShadow.data[layer].cascade.x, 1.0 / 4096.0);
    projCoords.z -= bias;

    const int sampleCount = clamp(int(cascadedShadow.data[layer].cascade.w + 0.5), 4, 16);

    // Contact hardening. A fixed filter radius gives every shadow the same soft edge, at the foot
    // of a wall and a hundred metres down its length alike, which is what makes a shadow read as
    // painted on. The penumbra of a real shadow grows with the gap between caster and receiver, so
    // measure that gap first: findAverageBlockerDepth (written long ago and never called) returns
    // the mean depth of whatever stands between this point and the sun.
    float worldTexel = max(cascadedShadow.data[layer].cascade.y, 1e-6);
    float depthRange = max(cascadedShadow.data[layer].cascadeExtra.x, 1e-4);
    float radius = uvTexel * 1.5;
    float blocker = findAverageBlockerDepth(layer, projCoords, uvTexel * 8.0, angle);
    if (blocker > 0.0) {
        // Depth difference to world units, then a world penumbra from the sun's angular size, then
        // back into UV. SUN_TAN is a touch wider than the real sun so the softening is visible.
        const float SUN_TAN = 0.02;
        float gapWorld = max(projCoords.z - blocker, 0.0) * depthRange;
        float penumbraUV = gapWorld * SUN_TAN * uvTexel / worldTexel;
        radius = clamp(penumbraUV, uvTexel * 1.2, uvTexel * 10.0);
    }
    float shadow = 0.0;
    int used = 0;
    for (uint i = 0; i < uint(sampleCount); ++i) {
        vec2 offset = vogel_disk_sample(i, sampleCount, angle) * radius;
        vec2 uv = projCoords.xy + offset;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            continue;
        }
        float lit = texture(shadowMap, vec4(uv, float(layer), projCoords.z));
        shadow += 1.0 - lit;
        used += 1;
    }
    if (used == 0) {
        return 0.0;
    }
    return shadow / float(used);
}

float TechniqueVogelPoint(PointLight light, int layer, vec3 projCoords) {
    float shadow = 0.0;

    const int sampleCount = clamp(light.samples, 1, 64);
    const float invSamples = 1.0 / float(sampleCount);
    const float angle = sfeInterleavedGradientNoise(gl_FragCoord.xy, 0u) * 6.28318530718;

    for (int i = 0; i < sampleCount; i++) {
        const vec2 offset = vogel_disk_sample(uint(i), sampleCount, angle) * light.texelSize;
        const float lit = texture(PointLightShadowMapArray, vec4(projCoords.xy + offset, layer + light.offset, projCoords.z));
        shadow += 1.0 - lit;
    }

    return shadow * invSamples;
}

vec3 worldToShadowUv(int layer, vec3 worldPos) {
    vec4 light = lightSpaceMatrices[layer] * vec4(worldPos, 1.0);
    vec3 proj = light.xyz / light.w;
    proj.xy = proj.xy * 0.5 + 0.5;
    proj.z = proj.z * 0.5 + 0.5;
    return proj;
}

float sampleCascade(int layer, vec3 worldPos, vec3 Normal) {
    float illumination = -dot(Normal, cascadedShadow.direction_intensity.xyz);
    float nDotL = clamp(illumination, 0.0, 1.0);

    // Normal-offset bias: step the lookup off the surface instead of pushing its depth away from
    // the light. A depth bias slides the shadow along the light ray, which is what detaches it
    // from the caster (peter panning); moving along the normal cannot, because the shift is
    // perpendicular to the ray. The offset grows on grazing surfaces, where one texel covers the
    // most depth. Depth bias stays only as the small residual the offset cannot cover.
    float worldTexel = cascadedShadow.data[layer].cascade.y;
    float slope = 1.0 - nDotL;
    vec3 proj = worldToShadowUv(layer, worldPos + Normal * (worldTexel * (1.0 + 3.0 * slope)));

    // The residual depth bias, measured in texels of this cascade and turned into its depth units here. It was a
    // fixed share of the depth range (plus a UV texel read as depth), and the range is padded far past the slice, so
    // the bias came out at centimetres on the first cascade and decimetres further out: at a low sun every shadow
    // stood that far off the foot that cast it.
    float texelBias = cascadedShadow.data[layer].cascade.z * (1.0 + 2.0 * slope);
    float depthRange = max(cascadedShadow.data[layer].cascadeExtra.x, 1e-4);
    return TechniqueVogelCascaded(layer, texelBias * worldTexel / depthRange, proj);
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

float ShadowCascadedCalculation(vec3 fragPosWorldSpace, vec3 Normal)
{
    int cc = pc.cascadeCount;
    if (cc < 1) {
        cc = int(cascadedShadow.color_and_misc.w + 0.5);
    }
    if (cc < 1) {
        cc = 4;
    }

    float edge = 1.0;
    int layer = selectCascade(fragPosWorldSpace, cc, edge);
    // Faded over the outer fifth of the circle, so the change of resolution arrives as a ring that grows
    // out of the ground rather than an edge.
    float blend = smoothstep(0.8, 1.0, edge);
    // Only cross-fade into the next cascade when the fragment actually lands inside it.
    // TechniqueVogelCascaded answers "lit" for out-of-range coordinates, so blending towards a
    // cascade that does not cover this fragment faded real shadows out at every boundary.
    bool fade = layer + 1 < cc && blend > 0.0 && shadowUvOk(worldToShadowUv(layer + 1, fragPosWorldSpace));
    // Under TAA the ring takes one cascade a pixel, the next one as often as its weight, and the history averages that
    // into the same cross-fade for one filtered lookup instead of two: the filter is most of the lighting's cost.
    if (fade && pc.taaFrame >= 0.0) {
        float pick = sfeInterleavedGradientNoise(gl_FragCoord.xy, uint(pc.taaFrame) + 17u);
        return sampleCascade(pick < blend ? layer + 1 : layer, fragPosWorldSpace, Normal);
    }
    float shadow = sampleCascade(layer, fragPosWorldSpace, Normal);
    if (layer + 1 >= cc) {
        // Nothing past the last cascade to hand over to. Left alone, the shadow stops wherever its square
        // map ends -- four straight lines across the ground, slanted with the light rather than the view.
        // Faded out over the rim of its circle instead, the same shape every other step now has.
        return shadow * (1.0 - smoothstep(0.8, 1.0, edge));
    }

    return fade ? mix(shadow, sampleCascade(layer + 1, fragPosWorldSpace, Normal), blend) : shadow;
}



float PointLightCalculation(PointLight light, vec3 fragPosWorldSpace, vec3 Normal) {//return 0.0 - light, 1.0 - dark
    const vec3 lightDir = normalize(light.Position - fragPosWorldSpace);
    const float nDotL = clamp(dot(Normal, lightDir), 0.0, 1.0);
    // texelSize is the PCF kernel, not 1/mapSize. Cap the old 0.0035 NDC default —
    // that plus a second z+bias sample is what detached contact shadows (peter panning).
    const float mapTexel = 1.0 / 1024.0;
    const float ndcBias = min(max(light.bias, 0.0), mapTexel * 1.5);
    const float zBias = max(ndcBias, mapTexel * 0.6) * mix(2.0, 0.35, nDotL);
    // Looked up from a little off the surface, along its normal, by about the filter's width there (the faces' texels
    // widen with the distance): ground the light grazes shaded itself, darker under the cube's side faces than under
    // the one straight below, with an edge between.
    const float reach = length(light.Position - fragPosWorldSpace);
    const vec3 receiver = fragPosWorldSpace + Normal * reach * (light.texelSize.x + mapTexel) * 2.0 * sqrt(1.0 - nDotL * nDotL);

    int layer = 0;
    vec3 projCoords = vec3(0.0);
    for (; layer < light.Layers; ++layer) {
        vec4 fragPosLightSpace = pointLightMatrices[layer + light.offset] * vec4(receiver, 1.0);
        projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
        projCoords = projCoords * 0.5 + 0.5;
        if (projCoords.x > 0.0 && projCoords.x < 1.0 && projCoords.y > 0.0 && projCoords.y < 1.0 &&
            projCoords.z > 0.0 && projCoords.z < 1.0) {
            break;
        }
    }

    if (layer >= light.Layers) {
        return 0.0;
    }

    projCoords.z -= zBias;
    return TechniqueVogelPoint(light, layer, projCoords);
}

const float PI = 3.14159265;
const float MAX_REFLECTION_LOD = 4.0;

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

// f90: how far the reflectance may rise at grazing angles. 1 for anything with a normal specular level; a surface that
// asks for less (grass, soil) keeps its grazing highlight down too instead of turning into a white sheen against the sun.
vec3 cookTorrance(vec3 N, vec3 V, vec3 L, vec3 albedo, vec3 F0, float roughness, float metallic, vec3 radiance, float f90) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = F0 + (max(vec3(f90), F0) - F0) * pow(clamp(1.0 - max(dot(H, V), 0.0), 0.0, 1.0), 5.0);
    vec3 specular = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 0.001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

vec2 calculateIllumination(vec3 lightDir, vec3 normal){
    const float illumination = -dot(normal, lightDir);
    float illuminationKoef = illumination;
    if (illuminationKoef < 0.0){
        illuminationKoef = 0.0;
    }
    return vec2(illumination, 1.0 - illuminationKoef);
}

void main() {
    const vec3 FragPos = sfeScenePosition(sceneDepth, TexCoords, matrices.invProjection, matrices.invView);
    const vec4 normalSample = texture(gNormal, TexCoords);
    const vec3 nSample = normalSample.rgb;
    // Nothing drawn here (the sky, which SkyPass paints over): black, as the end of the shader would have written it
    // after lighting it and filtering the shadow for nothing. The debug views still go through.
    if (dot(nSample, nSample) < 0.01 && pc.debugTerm < 0.5) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const float nLen = length(nSample);
    const vec3 Normal = nLen > 1e-5 ? nSample / nLen : vec3(0.0, 1.0, 0.0);
    const float sssAmt = clamp((1.0 - nLen) * 2.0, 0.0, 1.0);
    const float Depth = normalSample.a;
    vec4 albedoSample = texture(gAlbedoSpec, TexCoords);
    vec3 albedo = albedoSample.rgb;
    float roughness = clamp(albedoSample.a, 0.04, 1.0);
    vec4 pbr = texture(gMaterial, TexCoords);
    float metallic = pbr.x;
    float specFactor = pbr.y;
    float clearcoat = pbr.z;
    const bool celGlint = abs(clearcoat - kSfeCelGlint) < 0.5 / 255.0;
    const bool glint = celGlint || abs(clearcoat - kSfeGlint) < 0.5 / 255.0;
    const bool cel = celGlint || abs(clearcoat - kSfeCel) < 0.5 / 255.0;
    const bool ground = abs(clearcoat - kSfeGround) < 0.5 / 255.0;
    if (glint || cel || ground) {
        clearcoat = 0.0;
    }
    // Leaves and blades let the sun through (see sfeGBufferMaterial).
    const float translucency = clearcoat < 0.01 && !celGlint ? smoothstep(0.1, 1.0, pbr.w) : 0.0;
    // The area tint: whatever is a plant's colour (by its colour: hair is cut out as leaves are, and is not to wither)
    // goes where the map says towards its grey times the tint's colour, which keeps its pattern and can darken it.
    const vec3 areaTint = sfeAreaTint(areaTintMap, pc.tintArea, FragPos.xz);
    const float green = (1.0 - metallic) * sfePlantColour(albedo);
    if (green > 0.0 && areaTint.r > 0.0) {
        vec3 tinted = pc.tintFoliage.rgb * dot(albedo, vec3(0.2126, 0.7152, 0.0722));
        albedo = mix(albedo, tinted, clamp(areaTint.r * pc.tintFoliage.a, 0.0, 1.0) * green);
    }
    // And the ground goes grimy.
    if (ground && areaTint.r > 0.0) {
        albedo *= mix(vec3(1.0), pc.tintGround.rgb, clamp(areaTint.r * pc.tintGround.a, 0.0, 1.0));
    }
    float ccRough = max(pbr.w, 0.04);
    const float AmbientOcclusion = texture(ssao, TexCoords).r;
    // The material's own occlusion map: it darkens the sky's light and the reflections, never the sun or the lamps.
    const float materialAO = 1.0 - texture(gVelocity, TexCoords).b;

    const vec3 V = normalize(pc.viewPos - FragPos);
    float luma = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    float darkDiel = (1.0 - metallic) * (1.0 - smoothstep(0.03, 0.22, luma));
    darkDiel *= 1.0 - smoothstep(0.05, 0.25, clearcoat);
    roughness = mix(roughness, max(roughness, 0.62), darkDiel);
    vec3 F0 = mix(vec3(0.04 * specFactor), albedo, metallic);
    float f90 = clamp(50.0 * mix(0.04 * specFactor, 1.0, metallic), 0.0, 1.0);
    F0 *= mix(1.0, 0.35, darkDiel);

    vec3 Lsun = normalize(-cascadedShadow.direction_intensity.xyz);
    // The clouds overhead dim the sun, their shadows drifting across the forest with them: under the thickest it is
    // kCloudShade of the way to what sfeCloudShadow says. Kept apart from the shadows things cast and lighter than
    // them, the sky's light left as it is, so a tree's shade still falls inside a cloud's.
    const float kCloudShade = 0.6;
    const vec3 sunColour = cascadedShadow.color_and_misc.xyz * (1.0 - kCloudShade
        * (1.0 - sfeCloudShadow(cloudLayer, FragPos, Lsun, pc.cloudCoverage, sfeCloudDrift(matrices.wind, matrices.wind.w))));
    vec3 Lo = cookTorrance(
        Normal, V, Lsun,
        albedo, F0, roughness, metallic,
        sunColour * PI, f90);
    // A glint mirrors the sun whole, over a spot a few pixels across (a sharper one falls between pixels and TAA takes
    // it for a firefly); the glare in bloom_extract.fs turns it into a star. As strongly as a drawing that glints says
    // (kSfeCelGlint, its last channel), and added to the light once the drawn look has flattened that: it would swell
    // the glint towards the line where the light ends.
    vec3 flash = vec3(0.0);
    if (glint) {
        flash = cookTorrance(Normal, V, Lsun, vec3(0.0), vec3(1.0), 0.3, 0.0, sunColour * PI, 1.0)
            * (celGlint ? pbr.w : 1.0);
    }

    // Leaves and blades let the sun through: lit from behind they glow, most where the eye looks towards the sun
    // through them. In shadow like the rest of the direct light.
    if (translucency > 0.0) {
        const float behind = max(dot(-Normal, Lsun), 0.0);
        const float intoSun = pow(max(dot(-V, Lsun), 0.0), 6.0);
        Lo += albedo * sunColour * translucency * (behind * 0.55 + intoSun * 1.1);
    }

    float nDotLSun = max(dot(Normal, Lsun), 0.0);
    float shadow = 0.0;
    if (nDotLSun > 0.001 || translucency > 0.0) {
        shadow = ShadowCascadedCalculation(FragPos, Normal);
    }

    // A point light (a fire, a lantern) is its own: its shadow darkens only its light, never the sun's or the sky's,
    // and the sun's shadow leaves it alone. It dies away before its radius rather than ending there in a ring.
    vec3 Lpoint = vec3(0.0);
    for (int idx = 0; idx < pc.pointLightsSize; idx++) {
        const PointLight light = plb.pointLight[idx];
        const float distance = length(light.Position - FragPos);
        const float k = distance / light.radius;
        if (k >= 1.0) {
            continue;
        }
        const vec3 Lp = normalize(light.Position - FragPos);
        if (dot(Normal, Lp) <= 0.001) {
            continue;
        }
        const float lit = 1.0 - PointLightCalculation(light, FragPos, Normal);
        float window = 1.0 - k * k * k * k;
        window *= window;
        const float attenuation = window / (1.0 + light.Linear * distance + light.Quadratic * distance * distance);
        Lpoint += lit * cookTorrance(Normal, V, Lp, albedo, F0, roughness, metallic, light.Color * attenuation, f90);
    }

    // Cartoon light: a surface is either in the sun or turned from it, with a soft edge between the two rather than
    // the long falloff real light has -- two tones, the way an animated film paints a round shape. Taken on the light,
    // so a cloud's shadow or a tree's shade still falls where it did.
    // A drawing (kSfeCel) has no soft edge at all: flat in the light, a line, flat in the shade.
    const float litSide = cel ? smoothstep(0.02, 0.08, nDotLSun) : smoothstep(0.0, 0.32, nDotLSun);
    if (pc.stylise > 0.0) {
        const float toon = cel ? litSide : litSide * 0.85 + nDotLSun * 0.15;
        Lo *= mix(1.0, nDotLSun > 0.0001 ? toon / max(nDotLSun, 1e-3) : 1.0, pc.stylise);
    }
    Lo += flash;

    const float shadowFactor = 1.0 - pc.shadowIntensity * shadow;
    Lo *= shadowFactor;
    // Painted: a crease or the ground under a tent is darker in the sun too, by a share of the AO. The lamps keep theirs.
    Lo *= mix(1.0, mix(1.0, AmbientOcclusion, pc.ssaoAmount), pc.ssaoSun);
    Lo += Lpoint;
    // Over a wide range of angles: a normal map's bumps swing the normal across the terminator, and a narrow one cut
    // hard black blotches into a sunlit trunk.
    // A side turned away from the sun is as much out of it as one under a cast shadow: the ambient and sheen below
    // dim alike, or the back of a trunk stays brighter than the ground in its shadow. Not where light comes through.
    const float turnedAway = translucency > 0.0 ? 0.0
        : 1.0 - mix(smoothstep(-0.35, 0.3, dot(Normal, Lsun)), litSide, cel ? pc.stylise : 0.0);
    const float sunVisible = min(shadowFactor, 1.0 - pc.shadowIntensity * turnedAway);

    vec3 F = fresnelSchlickRoughness(max(dot(Normal, V), 0.0), F0, roughness);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 irradiance = texture(irradianceMap, Normal).rgb * pc.iblStrength;
    vec3 diffuse = irradiance * albedo;
    vec3 R = reflect(-V, Normal);
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb * pc.iblStrength;
    vec2 brdf = texture(brdfLUT, vec2(max(dot(Normal, V), 0.0), roughness)).rg;
    vec3 specular = prefilteredColor * (F * brdf.x + brdf.y);
    specular *= mix(1.0, 0.12, darkDiel);
    specular *= 1.0 - clearcoat;
    if (clearcoat > 0.01) {
        vec3 Fc0 = vec3(0.04);
        vec3 sunCol = sunColour * PI;
        vec3 Fc = fresnelSchlickRoughness(max(dot(Normal, V), 0.0), Fc0, ccRough);
        float coatRough = max(ccRough, 0.08);
        Lo += cookTorrance(Normal, V, Lsun, vec3(0.0), Fc0, coatRough, 0.0, sunCol, 1.0) * clearcoat;
        vec3 preCc = textureLod(prefilterMap, R, coatRough * MAX_REFLECTION_LOD).rgb * pc.iblStrength;
        vec2 brdfCc = texture(brdfLUT, vec2(max(dot(Normal, V), 0.0), coatRough)).rg;
        specular += preCc * (Fc * brdfCc.x + brdfCc.y) * clearcoat;
        specular += Fc * clearcoat * 0.28;
    }
    specular *= materialAO;
    float ao = mix(1.0, AmbientOcclusion, pc.ssaoAmount);
    ao = mix(ao, 1.0, clearcoat) * materialAO;
    // A painted surface has no highlight to speak of: the sky lights it flatly and the sun does the shaping.
    vec3 ambient = kD * diffuse * ao + specular * mix(1.0, 0.15, pc.stylise);
    ambient *= mix(pc.shadowAmbient, 1.0, sunVisible);

    // Backend-parity debugging: isolate one term so it can be captured and diffed on its own.
    // 1 shadow factor, 2 ambient/IBL, 3 direct light, 4 albedo, 5 normal, 6 AO (SSAO x material), 7 irradiance,
    // 8 prefiltered reflection, 9 brdf LUT, 15 the material's occlusion map alone.
    if (pc.debugTerm > 0.5) {
        int term = int(pc.debugTerm + 0.5);
        vec3 dbg = vec3(0.0);
        if      (term == 1) dbg = vec3(shadowFactor);
        else if (term == 2) dbg = ambient;
        else if (term == 3) dbg = Lo;
        else if (term == 4) dbg = albedo;
        else if (term == 5) dbg = Normal * 0.5 + 0.5;
        else if (term == 6) dbg = vec3(ao);
        else if (term == 7) dbg = irradiance;
        else if (term == 8) dbg = prefilteredColor;
        else if (term == 9) dbg = vec3(brdf, 0.0);
        else if (term == 15) dbg = vec3(materialAO);
        else if (term == 14) {
            // R = hardware comparison sampler result, G = manual compare on the same texel,
            // B = stored depth. If R and G disagree the comparison sampler is the problem.
            int cc = pc.cascadeCount > 0 ? pc.cascadeCount : int(cascadedShadow.color_and_misc.w + 0.5);
            float pickEdge = 1.0;
            int L = selectCascade(FragPos, cc, pickEdge);
            vec3 sp = worldToShadowUv(L, FragPos);
            float hw = texture(shadowMap, vec4(sp.xy, float(L), sp.z));
            float stored = texture(shadowMapDepth, vec3(sp.xy, float(L))).r;
            float manual = sp.z <= stored ? 1.0 : 0.0;
            dbg = vec3(hw, manual, stored);
        }
        else if (term == 13) {
            // The cross-fade weight itself: black = pure current cascade, white = fully the next
            // one. Shows exactly where the transition band sits on screen.
            int cc = pc.cascadeCount > 0 ? pc.cascadeCount : int(cascadedShadow.color_and_misc.w + 0.5);
            float pickEdge = 1.0;
            int L = selectCascade(FragPos, cc, pickEdge);
            dbg = vec3(L + 1 < cc ? smoothstep(0.8, 1.0, pickEdge) : 0.0);
        }
        else if (term == 12) {
            // Receiver depth minus stored occluder depth, amplified. >0.5 means the receiver sits
            // behind an occluder and should be shadowed; 0.5 means nothing is stored above it.
            int cc = pc.cascadeCount > 0 ? pc.cascadeCount : int(cascadedShadow.color_and_misc.w + 0.5);
            float pickEdge = 1.0;
            int L = selectCascade(FragPos, cc, pickEdge);
            vec3 sp = worldToShadowUv(L, FragPos);
            float blocker = texture(shadowMapDepth, vec3(sp.xy, float(L))).r;
            dbg = vec3((sp.z - blocker) * 100.0 + 0.5);
        }
        else if (term == 11) {
            // Green where the selected cascade's UV+depth are inside [0,1] (a shadow lookup can
            // happen at all), red where they are not and the test silently returns "lit".
            int cc = pc.cascadeCount > 0 ? pc.cascadeCount : int(cascadedShadow.color_and_misc.w + 0.5);
            float pickEdge = 1.0;
            int L = selectCascade(FragPos, cc, pickEdge);
            vec3 sp = worldToShadowUv(L, FragPos);
            dbg = shadowUvOk(sp) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        }
        else if (term == 10) {
            // Chosen cascade, as a step per level: 0 black, then 0.25 / 0.5 / 0.75 / 1.0.
            int cc = pc.cascadeCount > 0 ? pc.cascadeCount : int(cascadedShadow.color_and_misc.w + 0.5);
            float pickEdge = 1.0;
            int L = selectCascade(FragPos, cc, pickEdge);
            dbg = vec3(float(L + 1) / float(max(cc, 1)));
        }
        FragColor = vec4(dbg, 1.0);
        return;
    }

    float sssW = sssAmt;
    vec3 lighting = ambient + Lo;
    if (sssW > 0.05 && metallic < 0.25 && clearcoat < 0.5) {
        float wrap = max(pc.padLook0, 0.0);
        float str = max(pc.padLook1, 0.0);
        float nDotL = dot(Normal, Lsun);
        float trans = max(-nDotL, 0.0);
        float wrapL = clamp((nDotL + wrap) / max(1.0 + wrap, 1e-3), 0.0, 1.0);
        vec3 scatter = albedo * vec3(0.55, 0.22, 0.16);
        vec3 sssL = scatter * sunColour * 3.14159265
            * (trans * 0.9 + wrapL * 0.25) * sssW * str;
        lighting += sssL * mix(shadowFactor, 1.0, trans * 0.5);
    }

    // Sheen: a soft wrapped light and a rim on rough dielectrics, strongest on the roughest, by the sun's colour
    // held to a modest brightness.
    float fabric = (1.0 - metallic) * mix(0.15, 0.85, clamp(albedoSample.a, 0.04, 1.0)) * (1.0 - clearcoat);
    if (fabric >= 0.02 && pc.sheenStrength >= 0.01) {
        vec3 sheenLight = sunColour;
        float peak = max(sheenLight.r, max(sheenLight.g, sheenLight.b));
        sheenLight *= peak > 1.0 ? 1.25 / peak : 1.0;
        float wrap = max(pc.padLook0, 0.0);
        float wrapL = clamp((dot(Normal, Lsun) + wrap) / max(1.0 + wrap, 1e-3), 0.0, 1.0);
        float rim = pow(1.0 - max(dot(Normal, V), 0.0), 3.0) * pc.rimStrength;
        lighting += vec3(luma) * sheenLight * (wrapL * 0.12 + rim * 0.04) * pc.sheenStrength * fabric * sunVisible;
    }

    const float outlines = texture(gOutlines, TexCoords).b;
    vec3 fogColor = pc.ambientColor * 0.15 + vec3(0.08, 0.10, 0.14);
    float distFog = smoothstep(pc.fogStart, pc.drawDistance, Depth);
    float fogFactor = clamp(distFog * 0.45, 0.0, 0.35);
    if (pc.stylise > 0.0) {
        // Depth carries the palette: a drawing of a lake has everything past the near bank in one flat tone of the
        // sky's colour, and only what is close keeps its own. The haze does this already, far more gently. The tone
        // is the sky's own low towards the horizon this way, so at night it is the night's blue and not a grey.
        vec3 horizon = normalize(vec3(-V.x, 0.08, -V.z));
        fogColor = mix(fogColor, texture(irradianceMap, horizon).rgb * pc.iblStrength * 1.6, pc.stylise);
        fogFactor = max(fogFactor, smoothstep(40.0, 320.0, Depth) * 0.58 * pc.stylise);
        // Shade is coloured, not dark: the sky fills it with its own blue, and at night the moon lights everything
        // plainly rather than leaving it black. Most where the sun does not reach, though not so much that the shade
        // is as bright as the sun. The sky is overhead: what faces up takes all of it, a side or an underside half.
        float inShade = 1.0 - sunVisible * litSide;
        const float skyAbove = 0.5 + 0.5 * max(Normal.y, 0.0);
        lighting += albedo * texture(irradianceMap, Normal).rgb * pc.iblStrength * (0.8 + 1.0 * inShade) * skyAbove * pc.stylise * ao;
        // A bright rim where a shape turns away from the eye: what outlines it against what is behind in a cartoon.
        float rim = pow(1.0 - max(dot(Normal, V), 0.0), 3.0);
        lighting += texture(irradianceMap, -V).rgb * pc.iblStrength * rim * 0.9 * pc.stylise * albedo;
    }
    vec3 color = mix(lighting, fogColor, fogFactor);
    // The area tint's air: a haze of its colour in the sky's light low towards the horizon, as thick as the air
    // between the eye and here, taken as the mean of the two ends.
    const float airDensity = (areaTint.b + sfeAreaTint(areaTintMap, pc.tintArea, pc.viewPos.xz).b) * 0.5 * pc.tintAir.a;
    if (airDensity > 0.0) {
        vec3 skyLow = texture(irradianceMap, normalize(vec3(-V.x, 0.08, -V.z))).rgb * pc.iblStrength;
        color = mix(color, pc.tintAir.rgb * skyLow, 1.0 - exp(-airDensity * Depth));
    }
    if (pc.stylise > 0.0) {
        // A cartoon's colours are fuller than the world's.
        float grey = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color = max(mix(vec3(grey), color, 1.0 + 0.15 * pc.stylise), vec3(0.0));
    }
    if (dot(nSample, nSample) < 0.01) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    color += vec3(0.0, 0.78, 0.86) * outlines * 0.8;
    FragColor = vec4(color, 1.0);
}
