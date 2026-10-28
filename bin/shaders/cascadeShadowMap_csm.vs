#version 450 core
#include "sfe.glsl"
#include "sfe_skin.glsl"
#include "sfe_scene.glsl"
#include "sfe_material_table.glsl"
#include "sfe_wind.glsl"
// Cascade casters only: the cascade matrix applied here, which is what let the cascade geometry shader go away.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoords;
layout(location = 5) in ivec4 aBoneIds;
layout(location = 6) in vec4 aWeights;
layout(location = 7) in vec2 aTexCoords1;

layout(location = 1) out vec2 TexCoords;
layout(location = 2) out vec2 TexCoords1;
layout(location = 3) flat out uint vMaterial;
layout(location = 4) flat out float vPiece; // see g_buffer.vs
layout(location = 5) out vec2 vAcross;      // metres across the light, see cascadeShadowMap.fs

layout(SFE_SET(0, 1), std430) readonly buffer modelMatrices {
    mat4 model[];
};

const int MAX_BONE_INFLUENCE = 4;

// Every armature's bones back to back; an instance's start at its boneOffset.
layout(SFE_SET(0, 2), std430) readonly buffer bonesMatrices {
    mat4 bones[];
};

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(SFE_SET(0, 11), std140) uniform LightSpaceMatrices {
    mat4 lightSpaceMatrices[6];
};

SFE_PUSH_BLOCK PushConstants {
    int cascadeLayer;
    float leafLight; // cascadeShadowMap.fs
} params;

void main() {
    SfeInstance instance = sfeInstances[SFE_INSTANCE_ID];
    uint index = instance.entity;
    vMaterial = instance.material;

    SfeSkin skin = sfeSkinNone();
    for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
        int boneIdx = aBoneIds[i];
        if (boneIdx < 0) {
            continue;
        }
        sfeSkinAdd(skin, bones[instance.boneOffset + uint(boneIdx)], aWeights[i]);
    }
    mat4 BoneTransform = sfeSkinMatrix(skin);

    vec4 newPos = BoneTransform * vec4(aPos, 1.0);
    newPos /= newPos.w;

    int layer = clamp(params.cascadeLayer, 0, 5);
    vec4 worldPos = model[index] * newPos;
    // Swayed like the surface itself, or the shadow of a tree would stand still under its moving crown.
    worldPos.xyz += sfeWindSway(worldPos.xyz, model[index][3].xyz, instance.material, matrices.wind);
    gl_Position = lightSpaceMatrices[layer] * worldPos;
    // Pancaked: a caster between the cascade and the sun, past its near plane, is pressed onto that plane instead of
    // clipped away. At a low sun a tree tens of metres up-sun throws its shadow across the whole cascade.
    gl_Position.z = max(gl_Position.z, -gl_Position.w);
    // Where it stands across the light, in metres: the same for every leaf along one ray, in every cascade.
    mat4 m = lightSpaceMatrices[layer];
    vAcross = vec2(dot(worldPos.xyz, normalize(vec3(m[0][0], m[1][0], m[2][0]))),
                   dot(worldPos.xyz, normalize(vec3(m[0][1], m[1][1], m[2][1]))));
    TexCoords = aTexCoords;
    TexCoords1 = aTexCoords1;
    vPiece = sfePieceKey(uint(SFE_MESH_VERTEX_ID), model[index][3].xyz);
}
