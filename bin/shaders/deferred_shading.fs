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

layout(SFE_SET(0, 7)) uniform sampler2DArrayShadow PointLightShadowMapArray;
layout(SFE_SET(0, 8)) uniform sampler2DArrayShadow shadowMap;
layout(SFE_SET(0, 9)) uniform sampler2D gVelocity; // b: what a material's occlusion map shuts out (sfeGBufferVelocity)
layout(SFE_SET(0, 23)) uniform sampler2D gEmissive; // the light surfaces give off themselves (sfeGBufferEmissive)
layout(SFE_SET(0, 12)) uniform samplerCube irradianceMap;
layout(SFE_SET(0, 13)) uniform samplerCube prefilterMap;
layout(SFE_SET(0, 14)) uniform sampler2D brdfLUT;
layout(SFE_SET(0, 21)) uniform sampler2D cloudLayer;
layout(SFE_SET(0, 22)) uniform sampler2D areaTintMap;
// The sun's shadow over what is drawn, 0 lit and 1 in shadow (SunShadowPass, sun_shadow.fs): looked up there with its
// disk turned a sixteenth each over a 4x4 block and averaged back over the block, so it leaves no grain.
layout(SFE_SET(0, 10)) uniform sampler2D sunShadowMask;

// A ShelterComponent, as LightingPass::DrawData::Shelter has it.
struct Shelter {
    mat4 toLocal; // world to its own space
    vec4 box;     // x width, y the ridge's height, z length, w the eaves' height
    vec4 sky;     // x the share of the sky's light that comes in
};

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
    // How far the picture is pushed towards a painted one: what is far off in the sky's own tone, the colours fuller.
    float stylise;
    float ssaoSun; // the share of the AO the sun's light takes
    float shelterCount;
    float pad2;
    // The area tint (AreaTintFrame): the map's place, and the foliage's and the air's colour and strength.
    vec4 tintArea;
    vec4 tintFoliage;
    vec4 tintAir; // a the haze per metre where the map is full
    // The ShelterComponents, the first four.
    Shelter shelters[4];
} pc;

// The share of the sky's light that reaches `at` under the shelters (ShelterComponent::inside, kSoft 0.35 m): what is
// inside one takes only its share, what is out in the open all of it.
float skyOpen(vec3 at) {
    float open = 1.0;
    for (int i = 0; i < int(pc.shelterCount + 0.5); ++i) {
        vec3 local = (pc.shelters[i].toLocal * vec4(at, 1.0)).xyz;
        vec4 box = pc.shelters[i].box;
        float halfWide = max(box.x * 0.5, 1e-3);
        float across = abs(local.x);
        float roof = box.w + (box.y - box.w) * (1.0 - across / halfWide);
        float depth = min(min(halfWide - across, box.z * 0.5 - abs(local.z)), roof - local.y);
        open = min(open, mix(1.0, pc.shelters[i].sky.x, smoothstep(0.0, 0.35, depth)));
    }
    return open;
}


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
    int Type; // 0 a point light, 1 a spot
    int Layers; // its shadow's faces from `offset`: 6 a point light's, 1 a spot's, 0 for none
    float cosInner; // a spot's cone along Direction: lit fully inside cosInner, none outside cosOuter
    vec3 Direction;
    float cosOuter;
};

const int MAX_POINT_LIGHTS_SIZE = 12;

layout(SFE_SET(1, 10), std140) uniform pointLightsBlock {
    PointLight pointLight[MAX_POINT_LIGHTS_SIZE];
} plb;




layout(SFE_SET(1, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(SFE_SET(1, 3), std140) uniform PointLightMatrices {
    mat4 pointLightMatrices[72];
};

// The sun's shadow (sfe_sun_shadow.glsl), on these bindings.
#define SFE_SUN_SHADOW_DEPTH SFE_SET(0, 6)
#define SFE_SUN_SHADOW_BLOCK SFE_SET(1, 9)
#define SFE_SUN_SHADOW_MATRICES SFE_SET(1, 11)
#include "sfe_sun_shadow.glsl"

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

// The AO, worked out at half size a texel for every second pixel each way (ssao.fs), back at this pixel from those of
// the (up to) four round it that lie on its own surface: by plain bilinear weights a texel past an edge lent the ground
// behind a thing its shade, or the thing the ground's light, in a rim round it.
float upsampledAO(vec2 uv) {
    vec2 texelFull = 1.0 / vec2(textureSize(sceneDepth, 0));
    ivec2 aoMax = textureSize(ssao, 0) - 1;
    ivec2 f = ivec2(uv / texelFull);
    ivec2 i0 = f / 2;
    ivec2 odd = f - i0 * 2;
    bool sky;
    float z = -sfeSceneViewPosition(sceneDepth, uv, matrices.invProjection, sky).z;
    float tolerance = 0.02 * z + 0.02;
    // Nor a blade's from the ground under it, or the ground's from the blade (ssao_blur.fs).
    bool leaf = sfeIsFoliage(textureLod(gMaterial, uv, 0.0));
    float sum = 0.0;
    float wsum = 0.0;
    float nearest = 1.0;
    float nearestDz = 1e30;
    for (int j = 0; j <= odd.y; ++j) {
        for (int i = 0; i <= odd.x; ++i) {
            ivec2 t = min(i0 + ivec2(i, j), aoMax);
            bool tSky;
            float tz = -sfeSceneViewPosition(sceneDepth, (vec2(t * 2) + 0.5) * texelFull, matrices.invProjection, tSky).z;
            float dz = tSky ? 1e30 : abs(tz - z);
            float ao = texelFetch(ssao, t, 0).r;
            bool tLeaf = sfeIsFoliage(texelFetch(gMaterial, t * 2, 0));
            float w = exp(-(dz / tolerance) * (dz / tolerance)) * (tLeaf == leaf ? 1.0 : 0.02);
            sum += ao * w;
            wsum += w;
            if (dz < nearestDz) {
                nearestDz = dz;
                nearest = ao;
            }
        }
    }
    return wsum > 1e-4 ? sum / wsum : nearest;
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
    const bool glint = abs(clearcoat - kSfeGlint) < 0.5 / 255.0;
    const bool ground = abs(clearcoat - kSfeGround) < 0.5 / 255.0;
    if (glint || ground) {
        clearcoat = 0.0;
    }
    // Leaves and blades let the sun through (see sfeGBufferMaterial).
    const float translucency = clearcoat < 0.01 ? smoothstep(0.1, 1.0, pbr.w) : 0.0;
    // The area tint: whatever is a plant's colour (by its colour: hair is cut out as leaves are, and is not to wither)
    // goes where the map says towards its grey times the tint's colour, which keeps its pattern and can darken it. Not
    // the ground: it turns to mud where it is foul (g_buffer_terrain.fs).
    const vec3 areaTint = sfeAreaTint(areaTintMap, pc.tintArea, FragPos.xz);
    const float green = ground ? 0.0 : (1.0 - metallic) * sfePlantColour(albedo);
    if (green > 0.0 && areaTint.r > 0.0) {
        vec3 tinted = pc.tintFoliage.rgb * dot(albedo, vec3(0.2126, 0.7152, 0.0722));
        albedo = mix(albedo, tinted, clamp(areaTint.r * pc.tintFoliage.a, 0.0, 1.0) * green);
    }
    float ccRough = max(pbr.w, 0.04);
    const float AmbientOcclusion = upsampledAO(TexCoords);
    // The material's own occlusion map: it darkens the sky's light and the reflections, never the sun or the lamps.
    const float materialAO = 1.0 - texture(gVelocity, TexCoords).b;

    const vec3 V = normalize(pc.viewPos - FragPos);
    float luma = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    float darkDiel = (1.0 - metallic) * (1.0 - smoothstep(0.03, 0.22, luma));
    darkDiel *= 1.0 - smoothstep(0.05, 0.25, clearcoat);
    // Wet ground (its specular raised over the dry 0.08, g_buffer_terrain.fs) is filmed with water, as glossy however
    // dark the mud under it.
    if (ground) {
        darkDiel *= 1.0 - smoothstep(0.15, 0.45, specFactor);
    }
    roughness = mix(roughness, max(roughness, 0.62), darkDiel);
    vec3 F0 = mix(vec3(0.04 * specFactor), albedo, metallic);
    float f90 = clamp(50.0 * mix(0.04 * specFactor, 1.0, metallic), 0.0, 1.0);
    F0 *= mix(1.0, 0.35, darkDiel);

    vec3 Lsun = normalize(-cascadedShadow.direction_intensity.xyz);
    const vec3 sunColour = cascadedShadow.color_and_misc.xyz;
    vec3 Lo = cookTorrance(
        Normal, V, Lsun,
        albedo, F0, roughness, metallic,
        sunColour * PI, f90);
    // A glint mirrors the sun whole, over a spot a few pixels across (a sharper one falls between pixels and TAA takes
    // it for a firefly); the glare in bloom_extract.fs turns it into a star.
    vec3 flash = vec3(0.0);
    if (glint) {
        flash = cookTorrance(Normal, V, Lsun, vec3(0.0), vec3(1.0), 0.3, 0.0, sunColour * PI, 1.0);
    }

    // Leaves and blades let the sun through: lit from behind they glow, most where the eye looks towards the sun
    // through them. In shadow like the rest of the direct light.
    if (translucency > 0.0) {
        const float behind = max(dot(-Normal, Lsun), 0.0);
        const float intoSun = pow(max(dot(-V, Lsun), 0.0), 6.0);
        Lo += albedo * sunColour * translucency * (behind * 0.55 + intoSun * 1.1);
    }

    // Everywhere, turned from the sun or not: what wraps on past where the sun leaves a surface (the light under the
    // skin, the sheen) is shadowed there as on the side facing it. Read only where the surface faced the sun, the
    // shadow ended at that edge and the wrapped light came on there at once: a hard, ragged line down a face.
    const float shadow = texelFetch(sunShadowMask, ivec2(TexCoords * vec2(textureSize(sunShadowMask, 0))), 0).r;

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
        // A spot lights its cone alone, fading out over the cone's edge.
        const float cone = light.Type == 1 ? smoothstep(light.cosOuter, light.cosInner, dot(-Lp, light.Direction)) : 1.0;
        if (cone <= 0.0) {
            continue;
        }
        const float lit = cone * (1.0 - PointLightCalculation(light, FragPos, Normal));
        float window = 1.0 - k * k * k * k;
        window *= window;
        const float attenuation = window / (1.0 + light.Linear * distance + light.Quadratic * distance * distance);
        Lpoint += lit * cookTorrance(Normal, V, Lp, albedo, F0, roughness, metallic, light.Color * attenuation, f90);
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
    const float turnedAway = translucency > 0.0 ? 0.0 : 1.0 - smoothstep(-0.35, 0.3, dot(Normal, Lsun));
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
    vec3 ambient = kD * diffuse * ao + specular;
    ambient *= mix(pc.shadowAmbient, 1.0, sunVisible);
    // Under a roof the sky comes in only so far. Tested a little off the surface, out along its normal: a tent's
    // canvas from outside is under the open sky, the same canvas from inside under the roof.
    const float skyIn = skyOpen(FragPos + Normal * 0.25);
    ambient *= skyIn;

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

    // What glows (a lamp's glass) gives off its own light over what falls on it, in no shadow.
    lighting += texture(gEmissive, TexCoords).rgb;

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
