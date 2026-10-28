#version 450 core
#include "sfe.glsl"
#include "sfe_material_table.glsl"
// Cut-out casters past the first cascade, drawn solid (CascadedShadowPass): only the holes a tree's or a plant's leaves
// let the sun through, as the cut-out shader makes them, without its texture fetch; and none of a piece of a material
// dissolved past its share (SfeMaterial::dissolve), all of it, without its texture to tell what of it the application
// keeps. Drawn whole, dissolved trees shaded the ground past the cascade round the body as if in full leaf, a sunny
// patch round the feet in the dark.
layout(location = 3) flat in uint vMaterial;
layout(location = 4) flat in float vPiece;
layout(location = 5) in vec2 vAcross;

SFE_PUSH_BLOCK PushConstants {
    int cascadeLayer;
    float leafLight;
} params;

void main() {
    if (vPiece < sfeMaterials[vMaterial].dissolve || sfeLeafHole(sfeMaterials[vMaterial].mapMask, vAcross, params.leafLight)) {
        discard;
    }
}
