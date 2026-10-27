#ifndef SFE_GBUFFER_SURFACE_GLSL
#define SFE_GBUFFER_SURFACE_GLSL
// The G-buffer surface of a mesh with the default material shader. g_buffer.fs defines SFE_GBUFFER_CUTOUT and cuts
// the material's alpha out; g_buffer_solid.fs, for materials with nothing to cut, leaves the discard out entirely:
// its mere presence stops the depth test from rejecting hidden fragments before they are shaded.
#include "sfe.glsl"
#include "sfe_material.glsl"
layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec2 TexCoords;
layout(location = 2) in mat3 TBN;
layout(location = 5) in vec2 TexCoords1;
layout(location = 6) flat in uint vMaterial;
layout(location = 7) in vec4 vClip;
layout(location = 8) in vec4 vPrevClip;
layout(location = 9) in vec3 vSurface;
layout(location = 12) flat in float vPiece;
layout(location = 14) in vec3 vRest;

layout(location = 0) out vec4 gMaterial;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;
layout(location = 3) out vec4 gVelocity;
layout(location = 4) out vec4 gEmissive;

layout(SFE_SET(1, 1)) uniform sampler2D texture_diffuse1;
layout(SFE_SET(1, 2)) uniform sampler2D normalMap;
layout(SFE_SET(1, 3)) uniform sampler2D texture_specular1;
layout(SFE_SET(1, 4)) uniform sampler2D texture_roughness;
layout(SFE_SET(1, 5)) uniform sampler2D texture_metallic;
layout(SFE_SET(1, 6)) uniform sampler2D texture_subsurface;
layout(SFE_SET(1, 7)) uniform sampler2D triplanarAlbedo;
layout(SFE_SET(1, 8)) uniform sampler2D triplanarNormals;
layout(SFE_SET(1, 9)) uniform sampler2D triplanarAlbedo2;
layout(SFE_SET(1, 10)) uniform sampler2D triplanarNormals2;
layout(SFE_SET(1, 11)) uniform sampler2D texture_occlusion;
layout(SFE_SET(1, 12)) uniform sampler2D texture_emissive;

#define pc sfeMaterials[vMaterial]

bool inRect(vec2 uv, vec4 rect) {
    return all(greaterThanEqual(uv, rect.xy)) && all(lessThanEqual(uv, rect.zw));
}

// A texture projected along the model's axes, weighted towards the axis the surface faces, its normals blended as
// whiteout: the albedo and the world normal.
// `rotated` swaps each projection's u and v, and the normal map's x and y with them.
void sfeTriplanar(sampler2D albedo, sampler2D normals, float repeat, bool rotated, inout vec3 colour, inout vec3 Nw) {
    vec3 n = normalize(TBN[2]);
    vec3 w = pow(abs(n), vec3(4.0));
    w /= w.x + w.y + w.z;
    vec3 side = vec3(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0, n.z < 0.0 ? -1.0 : 1.0);
    vec3 p = vSurface * repeat;
    vec2 uvX = vec2(p.z * side.x, p.y);
    vec2 uvY = vec2(p.x * side.y, p.z);
    vec2 uvZ = vec2(-p.x * side.z, p.y);
    if (rotated) {
        uvX = uvX.yx;
        uvY = uvY.yx;
        uvZ = uvZ.yx;
    }
    colour = texture(albedo, uvX).rgb * w.x + texture(albedo, uvY).rgb * w.y + texture(albedo, uvZ).rgb * w.z;
    vec3 tX = texture(normals, uvX).xyz * 2.0 - 1.0;
    vec3 tY = texture(normals, uvY).xyz * 2.0 - 1.0;
    vec3 tZ = texture(normals, uvZ).xyz * 2.0 - 1.0;
    if (rotated) {
        tX.xy = tX.yx;
        tY.xy = tY.yx;
        tZ.xy = tZ.yx;
    }
    tX.x *= side.x;
    tY.x *= side.y;
    tZ.x *= -side.z;
    tX = vec3(tX.xy + n.zy, abs(tX.z) * n.x);
    tY = vec3(tY.xy + n.xz, abs(tY.z) * n.y);
    tZ = vec3(tZ.xy + n.xy, abs(tZ.z) * n.z);
    Nw = normalize(tX.zyx * w.x + tY.xzy * w.y + tZ.xyz * w.z);
}

vec2 uvOf(uint bit) {
    return ((uint(pc.uvSel + 0.5) & bit) != 0u) ? TexCoords1 : TexCoords;
}

// The application's own say over the surface (sfe_user_surface.glsl: sfeUserSurface and sfeUserSurfaceLate) and over
// which texels of a dissolving piece stay (sfe_user_dissolve.glsl), each a file an application stands in for with one
// of the same name in its content's shaders/. Included here, after everything above, which their bodies may read: the
// inputs, the material's textures, its entry pc and uvOf.
#include "sfe_user_surface.glsl"
#include "sfe_user_dissolve.glsl"

void main() {
    if (sfeNearFaded(gl_FragCoord)) {
        discard;
    }
#ifdef SFE_GBUFFER_DEPTH
    // A game's own depth for the surface: a shader that defines SFE_GBUFFER_DEPTH declares sfeSurfaceDepth before
    // including this file and defines it after. It returns the depth to keep for the pixel -- for a surface its vertex
    // shader moved, worked out exactly per pixel rather than between moved vertices.
    gl_FragDepth = sfeSurfaceDepth();
#endif
    uint maps = uint(pc.mapMask + 0.5);
    vec2 uvDiff = uvOf(1u);
    vec2 uvNrm = uvOf(2u);
    vec2 uvSpec = uvOf(4u);
    vec2 uvRough = uvOf(8u);
    vec2 uvMetal = uvOf(16u);
    vec2 uvSss = uvOf(32u);
    vec2 uvAo = uvOf(64u);

    // Filtered, not level 0: reading the mask at the top level made the coverage of a distant character
    // depend on which single texel the jitter happened to land on. The level is the hardware's, so the
    // anisotropic filter still applies and the two backends pick it the same way.
    float diffLod = textureQueryLod(texture_diffuse1, uvDiff).y;
    // Read once: the mask, the albedo and the edge search below all start from it.
    vec4 diffuseSample = texture(texture_diffuse1, uvDiff);
    float mask = diffuseSample.a;
    if ((maps & 8u) != 0u && mask > 0.99) {
        mask = max(texture(texture_specular1, uvSpec).r, texture(texture_roughness, uvRough).g);
    }
    uint uvBits = uint(pc.uvSel + 0.5);
    if ((uvBits & 1u) == 0u && (uvBits & 6u) != 0u) {
        mask = min(mask, texture(texture_diffuse1, TexCoords1).a);
    }
#ifdef SFE_GBUFFER_STYLE_MASK
    // A game's own look over what is cut out: sfeStyleMask, declared before this file is included and defined after,
    // gets the texture and the mask as read, and returns the mask to cut by.
    mask = sfeStyleMask(texture_diffuse1, uvDiff, diffLod, mask);
#endif
#ifdef SFE_GBUFFER_CUTOUT
    float cutoutFade = sfeCutoutFade(texture_diffuse1, diffLod);
    if (pc.alphaCutoff > 1.0) {
        float cov = mix(clamp(pc.alphaCutoff - 1.0, 0.05, 1.0), 1.0, cutoutFade);
        mask = min(mask, cov);
        float d = sfeCutoutDither(texture_diffuse1, uvDiff, diffLod);
        if (mask < d) {
            discard;
        }
    } else if (pc.alphaCutoff > 0.0 && mask < pc.alphaCutoff * (1.0 - cutoutFade)) {
        discard;
    }
#endif
    vec4 diff = diffuseSample;
    if (pc.alphaCutoff > 0.0) {
        diff.rgb = sfeMaskedAlbedo(texture_diffuse1, uvDiff, diffuseSample);
    }

    gNormal.a = gl_FragCoord.z / gl_FragCoord.w;

    vec3 nSample = texture(normalMap, uvNrm).rgb;
    vec3 Nw;
    // 1 where the map says nothing: then only the geometry spreads the normal inside the pixel.
    float normalLen = 1.0;
    if (dot(nSample, nSample) < 0.02) {
        Nw = normalize(TBN[2]);
    } else {
        vec3 nts = nSample * 2.0 - 1.0;
        // Before normalising: the mip handed back an average of several texels, and how much shorter that
        // average is says how much the normals under this pixel disagree.
        normalLen = min(1.0, length(nts));
        Nw = normalize(TBN * nts);
    }
    if (pc.clearcoat > 0.5) {
        Nw = normalize(TBN[2]);
    } else if (pc.clearcoat > 0.01) {
        Nw = normalize(mix(Nw, normalize(TBN[2]), pc.clearcoat));
    }
    // The triplanar textures where the coordinates fall in their rectangles; one pixel reads one of them at most.
    bool triplanar = false;
    if (pc.triplanarRepeat > 0.0 && inRect(TexCoords, pc.triplanarUv)) {
        sfeTriplanar(triplanarAlbedo, triplanarNormals, pc.triplanarRepeat, (uint(pc.triplanarRotated + 0.5) & 1u) != 0u, diff.rgb, Nw);
        triplanar = true;
    } else if (pc.triplanarRepeat2 > 0.0 && inRect(TexCoords, pc.triplanarUv2)) {
        sfeTriplanar(triplanarAlbedo2, triplanarNormals2, pc.triplanarRepeat2, (uint(pc.triplanarRotated + 0.5) & 2u) != 0u, diff.rgb, Nw);
        triplanar = true;
    }

#ifdef SFE_GBUFFER_STYLE
    // A game's own look over the surface: a shader that defines SFE_GBUFFER_STYLE declares sfeStyleSurface before
    // including this file and defines it after. It gets the colour as read, the normal, and the coat written below with
    // its last channel (one of sfe.glsl's markers, kSfeGlint..., and what that channel says there: sfeGBufferMaterial),
    // and turns them into its look before anything below is worked out from them.
    float coat = pc.clearcoat;
    float coatRough = pc.clearcoatRough;
    sfeStyleSurface(texture_diffuse1, uvDiff, diffLod, diff.rgb, Nw, coat, coatRough);
#else
    float coat = pc.clearcoat;
    const float coatRough = pc.clearcoatRough;
#endif
    vec3 linAlb = pow(diff.rgb, vec3(2.2)) * pc.tint;
    float roughness = pc.matRoughness;
    if ((maps & 1u) != 0u) {
        roughness = texture(texture_roughness, uvRough).g;
    } else if ((maps & 4u) != 0u) {
        roughness = 1.0 - texture(texture_specular1, uvSpec).r;
    }
    float metallic = pc.matMetallic;
    if ((maps & 2u) != 0u) {
        metallic = texture(texture_metallic, uvMetal).b;
    }
    // The application's own look over the surface (sfe_user_surface.glsl): the colour, the normal, the roughness and
    // the metalness as the maps give them, changed before anything below is worked out from them.
    sfeUserSurface(linAlb, Nw, roughness, metallic);
#ifdef SFE_GBUFFER_CUTOUT
    // A piece of a cut-out material dissolving (SfeMaterial::bare, the pieces whose vPiece is under it): a texel the
    // application keeps (sfe_user_dissolve.glsl) stays, in the colour it gives it, and the rest are gone.
    if (pc.alphaCutoff > 0.0 && vPiece < pc.bare) {
        if (!sfeUserDissolveKeeps(linAlb)) {
            discard;
        }
        linAlb = sfeUserDissolved(linAlb);
    }
#endif
    const bool cutout = pc.alphaCutoff > 0.0;
    // Cut-out surfaces (leaves, grass cards) scatter nothing under the skin: their subsurface map is not even read.
    float sss = 0.0;
    if (cutout) {
        Nw = normalize(TBN[2]);
    }
    else {
        sss = texture(texture_subsurface, uvSss).r;
        if (sss < 0.001) {
            sss = texture(texture_subsurface, uvSss).a;
        }
        // The one place skin is guessed from colour; lighting and the SSS pass read the result from the normal's
        // length. Nothing that sways in the wind (bits 32 and 64) is skin, however brown its bark.
        if (sss < 0.05 && pc.clearcoat < 0.5 && (maps & 16u) != 0u && (maps & 96u) == 0u && !triplanar) {
            sss = max(sss, sfeSkinSss(linAlb, 0.0));
        }
    }
    gNormal.xyz = Nw * (1.0 - 0.5 * clamp(sss, 0.0, 1.0));

    gAlbedoSpec.rgb = linAlb;

    roughness = mix(roughness, max(roughness, 0.62), clamp(sss, 0.0, 1.0));
    if (pc.clearcoat > 0.5) {
        roughness = 0.48;
    }
    metallic *= 1.0 - clamp(pc.clearcoat, 0.0, 1.0);
    if ((maps & 8u) != 0u || cutout) {
        metallic = 0.0;
    }

    float specF = pc.specFactor;
    if ((maps & 1u) != 0u && (maps & 4u) != 0u) {
        specF *= texture(texture_specular1, uvSpec).r;
    }
    if (cutout) {
        specF = min(specF, 0.18);
    }
    // The application's last word on the roughness, the metalness, the specular factor and the coat
    // (sfe_user_surface.glsl), before a cut-out's roughness floor and the clamps they are written with.
    sfeUserSurfaceLate(roughness, metallic, specF, coat);
    if (cutout) {
        roughness = max(roughness, 0.78);
    }

    gAlbedoSpec.a = clamp(sfeFilteredRoughness(Nw, roughness, normalLen), 0.04, 1.0);
    // Cut-out surfaces are leaves, needles and grass: they let the sun through.
    float lastChannel = cutout && pc.clearcoat < 0.01 ? kSfeTranslucentFoliage : clamp(coatRough, 0.04, 1.0);
    gMaterial = sfeGBufferMaterial(clamp(metallic, 0.0, 1.0), clamp(specF, 0.0, 1.0), clamp(coat, 0.0, 1.0), lastChannel);
    gVelocity = sfeGBufferVelocity(vClip, vPrevClip, 1.0 - texture(texture_occlusion, uvAo).r);
    gEmissive = sfeGBufferEmissive(vec3(pc.emissiveR, pc.emissiveG, pc.emissiveB), texture_emissive, uvOf(128u));
}

#endif
