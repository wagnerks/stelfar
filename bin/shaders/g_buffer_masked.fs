#version 450 core
#include "sfe.glsl"
#include "sfe_material.glsl"
layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec2 TexCoords;
layout(location = 2) in mat3 TBN;
layout(location = 5) in vec2 TexCoords1;
layout(location = 6) flat in uint vMaterial;
layout(location = 7) in vec4 vClip;
layout(location = 8) in vec4 vPrevClip;
layout(location = 12) flat in float vPiece;

layout(location = 0) out vec4 gMaterial;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;
layout(location = 3) out vec4 gVelocity;

layout(SFE_SET(1, 1)) uniform sampler2D texture_diffuse1;
layout(SFE_SET(1, 2)) uniform sampler2D normalMap;
layout(SFE_SET(1, 3)) uniform sampler2D texture_specular1;
layout(SFE_SET(1, 4)) uniform sampler2D texture_roughness;
layout(SFE_SET(1, 5)) uniform sampler2D texture_metallic;
layout(SFE_SET(1, 6)) uniform sampler2D texture_subsurface;
layout(SFE_SET(1, 11)) uniform sampler2D texture_occlusion;

#define pc sfeMaterials[vMaterial]

vec2 uvOf(uint bit) {
    return ((uint(pc.uvSel + 0.5) & bit) != 0u) ? TexCoords1 : TexCoords;
}

void main() {
    if (sfeNearFaded(gl_FragCoord)) {
        discard;
    }
    uint maps = uint(pc.mapMask + 0.5);
    vec2 uvDiff = uvOf(1u);
    vec2 uvNrm = uvOf(2u);
    vec2 uvSpec = uvOf(4u);
    vec2 uvRough = uvOf(8u);
    vec2 uvMetal = uvOf(16u);
    vec2 uvSss = uvOf(32u);
    vec2 uvAo = uvOf(64u);

    float cutoff = pc.alphaCutoff > 0.0 ? pc.alphaCutoff : 0.4;
    // Filtered, not level 0: reading the mask at the top level made the coverage of a distant character
    // depend on which single texel the jitter happened to land on. The level is the hardware's, so the
    // anisotropic filter still applies and the two backends pick it the same way.
    float diffLod = textureQueryLod(texture_diffuse1, uvDiff).y;
    float mask = texture(texture_diffuse1, uvDiff).a;
    if ((maps & 8u) != 0u && mask > 0.99) {
        mask = max(texture(texture_specular1, uvSpec).r, texture(texture_roughness, uvRough).g);
    }
    uint uvBits = uint(pc.uvSel + 0.5);
    if ((uvBits & 1u) == 0u && (uvBits & 6u) != 0u) {
        mask = min(mask, texture(texture_diffuse1, TexCoords1).a);
    }
    float cutoutFade = sfeCutoutFade(texture_diffuse1, diffLod);
    if (cutoff > 1.0) {
        float cov = mix(clamp(cutoff - 1.0, 0.05, 1.0), 1.0, cutoutFade);
        mask = min(mask, cov);
        float d = sfeCutoutDither(texture_diffuse1, uvDiff, diffLod);
        if (mask < d) {
            discard;
        }
    } else if (mask < cutoff * (1.0 - cutoutFade)) {
        discard;
    }
    vec4 diff = texture(texture_diffuse1, uvDiff);
    diff.rgb = sfeMaskedAlbedo(texture_diffuse1, uvDiff, diff);

    gNormal.a = gl_FragCoord.z / gl_FragCoord.w;

    vec3 geomN = normalize(TBN[2]);
    float sss = 0.0;
    gNormal.xyz = geomN;

    vec3 linAlb = pow(diff.rgb, vec3(2.2)) * pc.tint;
    // A plant going bare (sfe_gbuffer_surface.glsl).
    if (vPiece < pc.bare) {
        if (!sfeBareKeeps(linAlb)) {
            discard;
        }
        linAlb = mix(linAlb, vec3(sfeLuma(linAlb)), 0.7);
    }
    gAlbedoSpec.rgb = linAlb;

    float roughness = pc.matRoughness;
    if ((maps & 1u) != 0u) {
        roughness = texture(texture_roughness, uvRough).g;
    } else if ((maps & 4u) != 0u) {
        roughness = 1.0 - texture(texture_specular1, uvSpec).r;
    }
    roughness = max(roughness, 0.78);
    float metallic = 0.0;

    float specF = pc.specFactor;
    if ((maps & 1u) != 0u && (maps & 4u) != 0u) {
        specF *= texture(texture_specular1, uvSpec).r;
    }
    specF = min(specF, 0.18);

    gAlbedoSpec.a = clamp(sfeFilteredRoughness(geomN, roughness, 1.0), 0.04, 1.0);
    gMaterial = sfeGBufferMaterial(clamp(metallic, 0.0, 1.0), clamp(specF, 0.0, 1.0), clamp(pc.clearcoat, 0.0, 1.0), clamp(pc.clearcoatRough, 0.04, 1.0));
    gVelocity = sfeGBufferVelocity(vClip, vPrevClip, 1.0 - texture(texture_occlusion, uvAo).r);
}
