#version 450 core
// Text written on a model (ModelText): each glyph a quad of its signed distance field (SdfFont), the paint where the
// field reads more than pc.alphaCutoff. At 0.5 that is the glyph's own outline, sharp however near it is seen; worn,
// the cut rises in patches of noise laid over the text in metres, so the strokes thin and break where it is high.
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
layout(location = 4) out vec4 gEmissive;

layout(SFE_SET(1, 1)) uniform sampler2D texture_diffuse1;
layout(SFE_SET(1, 12)) uniform sampler2D texture_emissive;

#define pc sfeMaterials[vMaterial]

// The worn patches' size, metres, and how far into a stroke the worst of them eat (field units: 0.5 is the outline,
// 1 the spread inside it).
const float kWearScale = 0.025;
const float kWearDepth = 0.45;

void main() {
    if (sfeNearFaded(gl_FragCoord)) {
        discard;
    }
    float field = texture(texture_diffuse1, TexCoords).r;
    float wear = clamp((pc.alphaCutoff - 0.5) * 2.0, 0.0, 1.0);
    float cut = 0.5;
    if (wear > 0.0) {
        float patches = sfeFbm(TexCoords1 / kWearScale, 4);
        cut += kWearDepth * smoothstep(1.0 - wear, 1.0 - wear * 0.4, patches);
    }
    if (field < cut) {
        discard;
    }

    gNormal.a = gl_FragCoord.z / gl_FragCoord.w;
    vec3 geomN = normalize(TBN[2]);
    gNormal.xyz = geomN;
    gAlbedoSpec.rgb = pc.tint;
    gAlbedoSpec.a = clamp(sfeFilteredRoughness(geomN, pc.matRoughness, 1.0), 0.04, 1.0);
    gMaterial = sfeGBufferMaterial(0.0, clamp(pc.specFactor, 0.0, 1.0), 0.0, 0.04);
    gVelocity = sfeGBufferVelocity(vClip, vPrevClip, 0.0);
    gEmissive = sfeGBufferEmissive(vec3(pc.emissiveR, pc.emissiveG, pc.emissiveB), texture_emissive, TexCoords);
}
