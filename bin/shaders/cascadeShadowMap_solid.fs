#version 450 core
#include "sfe.glsl"
#include "sfe_material_table.glsl"
// Cut-out casters past the first cascade, drawn solid (CascadedShadowPass): only the holes a tree's or a plant's leaves
// let the sun through, as the cut-out shader makes them, without its texture fetch.
layout(location = 3) flat in uint vMaterial;
layout(location = 5) in vec2 vAcross;

SFE_PUSH_BLOCK PushConstants {
    int cascadeLayer;
    float leafLight;
} params;

void main() {
    if (sfeLeafHole(sfeMaterials[vMaterial].mapMask, vAcross, params.leafLight)) {
        discard;
    }
}
