#version 450 core
#extension GL_ARB_shader_viewport_layer_array : require
#include "sfe.glsl"
#include "sfe_skin.glsl"
#include "sfe_scene.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoords;
layout(location = 5) in ivec4 aBoneIds;
layout(location = 6) in vec4 aWeights;
layout(location = 7) in vec2 aTexCoords1;

// The caster's mask is read with these in pointLightMap.fs.
layout(location = 1) out vec2 TexCoords;
layout(location = 2) out vec2 TexCoords1;
layout(location = 3) flat out uint vMaterial;
layout(location = 4) flat out float vPiece; // see g_buffer.vs

layout(SFE_SET(0, 1), std430) readonly buffer modelMatrices {
    mat4 model[];
};

const int MAX_BONE_INFLUENCE = 4;

// Every armature's bones back to back; an instance's start at its boneOffset.
layout(SFE_SET(0, 2), std430) readonly buffer bonesMatrices {
    mat4 bones[];
};

layout(SFE_SET(0, 11), std140) uniform LightSpaceMatrices {
    mat4 lightSpaceMatrices[72];
};

// The face drawn, its layer of the cube maps' array (PointLightPass): one draw a face, each culled to that face's own
// frustum, so a caster beside the light goes only to the faces it shows in.
SFE_PUSH_BLOCK Push {
    int face;
} pc;

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

    mat4 world = model[index];
    gl_Position = lightSpaceMatrices[pc.face] * (world * newPos);
    gl_Layer = pc.face;
    TexCoords = aTexCoords;
    TexCoords1 = aTexCoords1;
    vPiece = sfePieceKey(uint(SFE_VERTEX_ID), world[3].xyz);
}
