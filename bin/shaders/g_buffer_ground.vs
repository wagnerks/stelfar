#version 450 core
#include "sfe.glsl"
// The ground drawn dense near the camera (sfe_ground_patch.glsl), into the G-buffer with the terrain's own fragment
// shader and textures: what g_buffer.vs gives it for the terrain's mesh.
invariant gl_Position;
layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec2 TexCoords;
layout(location = 2) out mat3 TBN;
layout(location = 5) out vec2 TexCoords1;
layout(location = 6) flat out uint vMaterial;
layout(location = 7) out vec4 vClip;
layout(location = 8) out vec4 vPrevClip;

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

#include "sfe_ground_patch.glsl"

void main() {
    SfeGroundVertex v = sfeGroundPatchVertex(SFE_VERTEX_ID, SFE_INSTANCE_ID);
    precise vec4 worldPos = vec4(v.world, 1.0);
    FragPos = v.world;
    TexCoords = v.local;
    // y: how much of the marks' depth the shape here took, which the fragment shader no longer looks into.
    TexCoords1 = vec2(v.height, v.take);
    // The terrain's shader reads no material.
    vMaterial = 0u;
    vec3 N = normalize(v.normal);
    vec3 T = normalize(v.tangent - N * dot(N, v.tangent));
    TBN = mat3(T, cross(N, T), N);
    // The ground stays where it is.
    vClip = matrices.PVNoJitter * worldPos;
    vPrevClip = matrices.prevPVNoJitter * worldPos;
    precise vec4 clipPos = matrices.PV * worldPos;
    gl_Position = clipPos;
}
