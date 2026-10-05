#version 450 core
#include "sfe.glsl"
#include "sfe_material.glsl"
layout(location = 1) in vec2 TexCoords;
layout(location = 2) in vec2 TexCoords1;
layout(location = 3) flat in uint vMaterial;
layout(location = 4) flat in float vPiece;
layout(location = 5) in vec2 vAcross;

layout(SFE_SET(1, 4)) uniform sampler2D texture_diffuse1;

SFE_PUSH_BLOCK PushConstants {
    int cascadeLayer;
    float leafLight; // the share of the sun a tree's or a plant's leaves let through
} params;

float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    SfeMaterial material = sfeMaterials[vMaterial];
    vec4 t = textureLod(texture_diffuse1, TexCoords, 0.0);
    float cutoff = abs(material.shadowCutoff);
    float a = material.shadowCutoff < 0.0 ? max(t.r, t.g) : t.a;
    a = min(a, textureLod(texture_diffuse1, TexCoords1, 0.0).a);
    // A leaf of a plant gone that far bare (sfe_gbuffer_surface.glsl) casts no shadow either.
    if (cutoff > 0.0 && (a < cutoff || (vPiece < material.bare && !sfeBareKeeps(pow(t.rgb, vec3(2.2)) * material.tint)))) {
        discard;
    }
    if (sfeLeafHole(material.mapMask, vAcross, params.leafLight)) {
        discard;
    }
    float opacity = clamp(material.casterOpacity, 0.0, 1.0);
    if (opacity < 0.999 && ign(gl_FragCoord.xy) > opacity) {
        discard;
    }
}
