#ifndef SFE_GPU_MESHES_GLSL
#define SFE_GPU_MESHES_GLSL
// Every mesh's ranges in the shared buffers and its bounds, as GpuScene uploads them: mirrors SFE::GpuMesh
// (MeshVaoRegistry.h) field for field.
struct GpuMesh {
    uint firstIndex;
    uint indexCount;
    uint firstVertex;
    uint vertexCount;
    vec3 boundsMin;
    uint skinned;
    vec3 boundsMax;
    uint lodIndex;
    uint lodCount;
    float lodErrors[6]; // levels 1 on, a share of the mesh's extent
    uint _pad0;
};
layout(SFE_SET(0, 26), std430) readonly buffer Meshes {
    GpuMesh meshes[];
};

// A skinned mesh's bounds without its pose: animation can move vertices past the bind pose, so they get this much of
// their largest half-extent added on every axis.
const float kSkinnedInflate = 0.5;
vec3 sfeSkinnedInflated(vec3 extents) {
    return extents + vec3(max(extents.x, max(extents.y, extents.z)) * kSkinnedInflate);
}
#endif
