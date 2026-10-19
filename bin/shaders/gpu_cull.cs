#version 450 core
#include "sfe.glsl"
// One invocation per batch (all instances of one mesh in one draw group). It walks its own instances
// in order and packs the visible ones at the start of its range, so no two invocations share a write.
layout(local_size_x = 16) in;

layout(SFE_SET(0, 1), std430) readonly buffer modelMatrices {
    mat4 model[];
};

#include "sfe_gpu_meshes.glsl"

struct SceneInstance {
    uint entity;
    uint mesh;
    uint material;
    uint boneOffset;
};
layout(SFE_SET(0, 27), std430) readonly buffer Instances {
    SceneInstance instances[];
};

struct Batch {
    uint firstInstance;
    uint instanceCount;
    uint mesh;
    uint _pad;
};
layout(SFE_SET(0, 28), std430) readonly buffer Batches {
    Batch batches[];
};

struct VisibleInstance {
    uint entity;
    uint material;
    uint boneOffset;
    uint _pad;
};
layout(SFE_SET(0, 29), std430) writeonly buffer Visible {
    VisibleInstance visible[];
};

struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
layout(SFE_SET(0, 30), std430) writeonly buffer Commands {
    DrawCommand commands[];
};

// mode 0: shapes are 6 planes (xyz normal, w distance), inside when dot(n, p) + w >= -radius.
// mode 1: shapes are spheres (xyz centre, w radius), visible when the bounds touch any of them.
// occlusion 0: no occlusion test. 1: only instances visible last frame, packed at the start of the batch's
// range. 2: everything else that passes the depth pyramid, packed at the end of the range with its commands
// after the first phase's, and each instance's visibility recorded for the next frame.
layout(SFE_SET(0, 31), std430) readonly buffer CullView {
    uint mode;
    uint shapeCount;
    uint batchCount;
    uint lodBias;
    vec4 shapes[16];
    mat4 viewProj;
    uint occlusion;
    uint depthWidth;  // the depth texture the pyramid was built from
    uint depthHeight;
    uint levels;
    uint levelOffsets[16];
    // xyz the player camera, w the pixels a unit of radius covers a unit away.
    vec4 lodOrigin;
    // A shadow cascade's texels per metre, 0 for other views, and the fewest texels a caster must span.
    float texelsPerMetre;
    float minTexels;
    // The ground's batches, groundFirst up to groundEnd (none outside the G-buffer list), and the metres from the
    // camera past which its chunks are drawn a level coarser than their size on screen asks for.
    uint groundFirst;
    uint groundEnd;
    float groundLodDistance;
    // The pixels on screen a coarser level may change, and as many texels in a shadow map.
    float lodPixels;
} view;

// The coarsest level of the chain that strays from the full mesh by no more than `allowed`, a share of the mesh's
// extent (MeshLod's errors, growing down the chain).
uint lodForError(GpuMesh mesh, float allowed) {
    uint level = 0u;
    for (uint l = 1u; l < mesh.lodCount; ++l) {
        if (mesh.lodErrors[l - 1u] <= allowed) {
            level = l;
        }
    }
    return level;
}

// Per instance slot: 1 when it passed the occlusion test last frame. A slot that changed owner can hold
// anything, which only decides the phase it is drawn in.
layout(SFE_SET(0, 32), std430) buffer LastVisible {
    uint lastVisible[];
};

// Level 0 is half the depth texture; each level halves the one before, down to 1x1.
layout(SFE_SET(0, 33), std430) readonly buffer Pyramid {
    float pyramid[];
};

// A skinned mesh with pose bounds (skinned = 2 + slot) reads them from the matrix `slot + 1` before its bones:
// column 0 xyz the minimum, column 1 the maximum, and a minimum above the maximum when nothing wrote them yet.
layout(SFE_SET(0, 2), std430) readonly buffer bonesMatrices {
    mat4 bones[];
};

// Without pose bounds, skinned bounds are inflated (sfeSkinnedInflated).

bool visibleInView(vec3 center, vec3 extents) {
    if (view.mode == 0u) {
        for (uint i = 0u; i < view.shapeCount; ++i) {
            vec3 n = view.shapes[i].xyz;
            float r = dot(extents, abs(n));
            if (dot(n, center) + view.shapes[i].w < -r) {
                return false;
            }
        }
        return true;
    }
    for (uint i = 0u; i < view.shapeCount; ++i) {
        vec3 d = max(abs(center - view.shapes[i].xyz) - extents, vec3(0.0));
        if (dot(d, d) <= view.shapes[i].w * view.shapes[i].w) {
            return true;
        }
    }
    return false;
}

// True when the box is certainly behind the depth pyramid. Boxes crossing the near plane are never occluded.
bool occluded(vec3 center, vec3 extents) {
    vec2 lo = vec2(1.0);
    vec2 hi = vec2(-1.0);
    float nearest = 1.0;
    for (int c = 0; c < 8; ++c) {
        vec3 corner = center + extents * vec3((c & 1) != 0 ? 1.0 : -1.0, (c & 2) != 0 ? 1.0 : -1.0, (c & 4) != 0 ? 1.0 : -1.0);
        precise vec4 clip = view.viewProj * vec4(corner, 1.0);
        if (clip.w <= 1e-4) {
            return false;
        }
        precise vec3 ndc = clip.xyz / clip.w;
        lo = min(lo, ndc.xy);
        hi = max(hi, ndc.xy);
        nearest = min(nearest, ndc.z);
    }
    precise float depth = nearest * 0.5 + 0.5;
    if (depth <= 0.0) {
        return false;
    }
    // Pixels the box covers, one wider on every side for the TAA jitter.
    vec2 size = vec2(view.depthWidth, view.depthHeight);
    ivec2 maxPixel = ivec2(size) - 1;
    ivec2 p0 = clamp(ivec2(floor((lo * 0.5 + 0.5) * size)) - 1, ivec2(0), maxPixel);
    ivec2 p1 = clamp(ivec2(floor((hi * 0.5 + 0.5) * size)) + 1, ivec2(0), maxPixel);
    // The level where the box spans at most two texels or so; a texel of level L covers 2^(L+1) pixels.
    int span = max(p1.x - p0.x, p1.y - p0.y) + 1;
    int level = clamp(int(ceil(log2(float(span)))) - 2, 0, int(view.levels) - 1);
    uint w = max(view.depthWidth >> 1, 1u);
    uint h = max(view.depthHeight >> 1, 1u);
    for (int l = 0; l < level; ++l) {
        w = max(w >> 1, 1u);
        h = max(h >> 1, 1u);
    }
    // Floor halving leaves the remainder to the last texel, so a pixel's texel is its index shifted, clamped.
    uint x0 = min(uint(p0.x) >> uint(level + 1), w - 1u);
    uint x1 = min(uint(p1.x) >> uint(level + 1), w - 1u);
    uint y0 = min(uint(p0.y) >> uint(level + 1), h - 1u);
    uint y1 = min(uint(p1.y) >> uint(level + 1), h - 1u);
    uint offset = view.levelOffsets[level];
    float farthest = 0.0;
    for (uint y = y0; y <= y1; ++y) {
        for (uint x = x0; x <= x1; ++x) {
            farthest = max(farthest, pyramid[offset + y * w + x]);
        }
    }
    return depth > farthest;
}

void main() {
    uint b = gl_GlobalInvocationID.x;
    if (b >= view.batchCount) {
        return;
    }
    Batch batch = batches[b];
    GpuMesh mesh = meshes[batch.mesh];
    vec3 meshCenter = (mesh.boundsMin + mesh.boundsMax) * 0.5;
    vec3 meshExtents = (mesh.boundsMax - mesh.boundsMin) * 0.5;

    uint count = 0u;
    for (uint i = 0u; i < batch.instanceCount; ++i) {
        uint slot = batch.firstInstance + i;
        SceneInstance instance = instances[slot];
        mat4 m = model[instance.entity];
        vec3 localCenter = meshCenter;
        vec3 localExtents = meshExtents;
        bool inflate = mesh.skinned != 0u;
        if (mesh.skinned >= 2u && instance.boneOffset >= mesh.skinned - 1u) {
            mat4 pose = bones[instance.boneOffset - (mesh.skinned - 1u)];
            if (pose[0].x <= pose[1].x) {
                localCenter = (pose[0].xyz + pose[1].xyz) * 0.5;
                localExtents = (pose[1].xyz - pose[0].xyz) * 0.5;
                inflate = false;
            }
        }
        vec3 center = (m * vec4(localCenter, 1.0)).xyz;
        vec3 extents = abs(m[0].xyz) * localExtents.x + abs(m[1].xyz) * localExtents.y + abs(m[2].xyz) * localExtents.z;
        if (inflate) {
            extents = sfeSkinnedInflated(extents);
        }
        // The mesh table holds one entry per level and the scene registers the instance once under each, so
        // every batch is asked whether this instance belongs to it at this distance and exactly one says yes.
        bool draw = visibleInView(center, extents);
        float radius = length(extents);
        // In a shadow map a caster covers texels, not pixels: one smaller than a texel leaves the map as it is.
        float texels = radius * view.texelsPerMetre;
        if (view.texelsPerMetre > 0.0 && texels < view.minTexels) {
            draw = false;
        }
        if (mesh.lodCount > 1u) {
            float distance = max(length(center - view.lodOrigin.xyz), 1e-3);
            // A level is drawn once what it changes is under lodPixels: its error times the mesh's extent on screen, at most
            // twice the radius. The pixels are the player's; each step of bias doubles them.
            float allowed = view.lodPixels * float(1u << view.lodBias) / max(2.0 * radius * view.lodOrigin.w / distance, 1e-6);
            // Never finer than the map's texels show, which in a far cascade is coarser than the camera's pixels ask.
            if (view.texelsPerMetre > 0.0) {
                allowed = max(allowed, view.lodPixels / max(2.0 * texels, 1e-6));
            }
            uint level = lodForError(mesh, allowed);
            // A far ground chunk is still large on screen, but seen nearly edge on its triangles are slivers a pixel
            // or two high, shaded mostly for nothing: past the distance it keeps about half of them.
            if (b >= view.groundFirst && b < view.groundEnd && distance > view.groundLodDistance) {
                level = min(level + 1u, mesh.lodCount - 1u);
            }
            draw = draw && level == mesh.lodIndex;
        }
        if (view.occlusion == 1u) {
            draw = draw && lastVisible[slot] == 1u;
        }
        else if (view.occlusion == 2u) {
            bool seen = draw && !occluded(center, extents);
            // Drawn in the first phase already when it was visible last frame.
            draw = seen && lastVisible[slot] != 1u;
            lastVisible[slot] = seen ? 1u : 0u;
        }
        if (draw) {
            uint target = view.occlusion == 2u ? batch.firstInstance + batch.instanceCount - 1u - count : batch.firstInstance + count;
            visible[target] = VisibleInstance(instance.entity, instance.material, instance.boneOffset, 0u);
            ++count;
        }
    }

    if (view.occlusion == 2u) {
        commands[view.batchCount + b] = DrawCommand(mesh.indexCount, count, mesh.firstIndex, int(mesh.firstVertex), batch.firstInstance + batch.instanceCount - count);
    }
    else {
        commands[b] = DrawCommand(mesh.indexCount, count, mesh.firstIndex, int(mesh.firstVertex), batch.firstInstance);
    }
}
