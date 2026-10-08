#version 450 core
#include "sfe.glsl"
#include "sfe_material.glsl"
layout(location = 1) in vec2 TexCoords;
layout(location = 2) in vec2 TexCoords1;
layout(location = 3) flat in uint vMaterial;
layout(location = 4) flat in float vPiece;

layout(SFE_SET(1, 4)) uniform sampler2D texture_diffuse1;

float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    SfeMaterial material = sfeMaterials[vMaterial];
    float cutoff = abs(material.shadowCutoff);
    // Read only for what is cut out; the second set of coordinates read again only where it is not the first's copy.
    if (cutoff > 0.0) {
        vec4 t = textureLod(texture_diffuse1, TexCoords, 0.0);
        float a = material.shadowCutoff < 0.0 ? max(t.r, t.g) : t.a;
        a = min(a, TexCoords1 == TexCoords ? t.a : textureLod(texture_diffuse1, TexCoords1, 0.0).a);
        // A leaf of a plant gone that far bare (sfe_gbuffer_surface.glsl) casts no shadow either.
        if (a < cutoff || (vPiece < material.bare && !sfeBareKeeps(pow(t.rgb, vec3(2.2)) * material.tint))) {
            discard;
        }
    }
    float opacity = clamp(material.casterOpacity, 0.0, 1.0);
    if (opacity < 0.999 && ign(gl_FragCoord.xy) > opacity) {
        discard;
    }
}
