#version 450 core
#include "sfe.glsl"
#include "sfe_skin.glsl"
#include "sfe_scene.glsl"
#include "sfe_material_table.glsl"
#include "sfe_wind.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec3 aTangents;
layout(location = 4) in vec3 aBiTangents;
layout(location = 5) in ivec4 aBoneIds;
layout(location = 6) in vec4 aWeights;
layout(location = 7) in vec2 aTexCoords1;

// invariant and `precise` on the skinning chain: otherwise the OpenGL driver and the SPIR-V path may
// fuse multiply-adds differently, and the same skinned vertex lands a few ULPs apart per backend,
// which moves triangle edges by a pixel.
invariant gl_Position;
layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec2 TexCoords;
layout(location = 2) out mat3 TBN;
layout(location = 5) out vec2 TexCoords1;
layout(location = 6) flat out uint vMaterial;
// Unjittered clip positions, this frame and the last: their difference is the pixel's motion.
layout(location = 7) out vec4 vClip;
layout(location = 8) out vec4 vPrevClip;
// Where on the model this is, in world metres from its origin and before the wind: what a triplanar texture is laid
// by, so it neither slides as the model moves nor swims as it sways.
layout(location = 9) out vec3 vSurface;
// When this triangle of a cut-out surface is gone as the plant goes bare (sfePieceKey).
layout(location = 12) flat out float vPiece;
// Where this is on the model at rest, in its own metres: what stays put on it as it moves, such as mud thrown on it.
layout(location = 14) out vec3 vRest;
// The model's normal there at rest, and the entity's look: for a piece of litter how worn and dirty it is (sfeLooked).
layout(location = 15) out vec3 vRestNormal;
layout(location = 13) flat out uvec4 vLook;
// The rest of the look, silted and burnt: its second half.
layout(location = 16) flat out uvec4 vLookMore;

// Each entity's look by its index, as the game puts it (FrameId::PieceLooks), two uvec4 each: 0s for all it does not,
// and for any past its end.
layout(SFE_SET(0, 37), std430) readonly buffer SfeLooks {
    uvec4 sfeLooks[];
};

layout(SFE_SET(0, 1), std430) readonly buffer modelMatrices {
    mat4 model[];
};

const int MAX_BONE_INFLUENCE = 4;

// Every armature's bones back to back; an instance's start at its boneOffset.
layout(SFE_SET(0, 2), std430) readonly buffer bonesMatrices {
    mat4 bones[];
};

// The same two, as they were last frame.
layout(SFE_SET(0, 8), std430) readonly buffer prevModelMatrices {
    mat4 prevModel[];
};
layout(SFE_SET(0, 9), std430) readonly buffer prevBonesMatrices {
    mat4 prevBones[];
};

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

#include "sfe_marks.glsl"

void main() {
    SfeInstance instance = sfeInstances[SFE_INSTANCE_ID];
    uint index = instance.entity;
    vMaterial = instance.material;

    SfeSkin skin = sfeSkinNone();
    SfeSkin prevSkin = sfeSkinNone();
    for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
        int boneIdx = aBoneIds[i];
        if (boneIdx < 0) {
            continue;
        }
        sfeSkinAdd(skin, bones[instance.boneOffset + uint(boneIdx)], aWeights[i]);
        sfeSkinAdd(prevSkin, prevBones[instance.boneOffset + uint(boneIdx)], aWeights[i]);
    }
    precise mat4 BoneTransform = sfeSkinMatrix(skin);
    mat4 prevBoneTransform = sfeSkinMatrix(prevSkin);

    precise mat3 normalMatrix = mat3(model[index]) * mat3(BoneTransform);
    precise vec4 newPos = BoneTransform * vec4(aPos.xyz, 1.0);
    newPos /= newPos.w;
    precise vec4 worldPos = model[index] * newPos;
    vSurface = worldPos.xyz - model[index][3].xyz;
    vRest = aPos;
    vRestNormal = aNormal;
    bool looked = 2u * index + 1u < uint(sfeLooks.length());
    vLook = looked ? sfeLooks[2u * index] : uvec4(0u);
    vLookMore = looked ? sfeLooks[2u * index + 1u] : uvec4(0u);
    vPiece = sfePieceKey(uint(SFE_VERTEX_ID), model[index][3].xyz);
    worldPos.xyz += sfeWindSway(worldPos.xyz, model[index][3].xyz, instance.material, matrices.wind);
    worldPos.xyz = sfeGroundUnderPatch(worldPos.xyz, instance.material);

    FragPos = worldPos.xyz;
    TexCoords = aTexCoords;
    TexCoords1 = aTexCoords1;

    vec3 N = normalize(normalMatrix * aNormal);
    vec3 T = normalMatrix * aTangents;
    vec3 Bsrc = normalMatrix * aBiTangents;
    T = T - N * dot(N, T);
    if (dot(T, T) > 1e-8) {
        T = normalize(T);
        vec3 B = cross(N, T);
        if (dot(Bsrc, B) < 0.0) {
            B = -B;
        }
        TBN[0] = T;
        TBN[1] = B;
    } else {
        TBN[0] = vec3(0.0);
        TBN[1] = vec3(0.0);
    }
    TBN[2] = N;

    // Where this vertex was last frame, through last frame's pose and transform.
    vec4 prevPos = prevBoneTransform * vec4(aPos.xyz, 1.0);
    prevPos /= prevPos.w;
    vClip = matrices.PVNoJitter * worldPos;
    vec4 prevWorld = prevModel[index] * prevPos;
    prevWorld.xyz += sfeWindSway(prevWorld.xyz, prevModel[index][3].xyz, instance.material, matrices.windBefore);
    prevWorld.xyz = sfeGroundUnderPatch(prevWorld.xyz, instance.material);
    vPrevClip = matrices.prevPVNoJitter * prevWorld;

    precise vec4 clipPos = matrices.PV * worldPos;
    gl_Position = clipPos;
}
