#ifndef SFE_LOD_GLSL
#define SFE_LOD_GLSL
// Which level of a mesh's chain is drawn: one text for the scene's culls (gpu_cull.cs) and for C++ (renderModule/LodRule.h),
// where a pass that picks the levels of its own instances takes it, as the game's litter does. So it is written in what
// GLSL and C++ share -- scalars, uint, a fixed array parameter, max, f-suffixed literals, no vectors and no includes --
// and every function is SFE_LOD_FN, which C++ makes inline.
#ifndef SFE_LOD_FN
#define SFE_LOD_FN
#endif

// The share of the mesh's extent a coarser level may stray by: what it changes, its error times the mesh's extent on
// screen (at most twice the radius of its world bounds), stays under lodPixels. pixelsPerUnit the pixels a unit of
// radius covers a unit away, distance the eye's from the bounds' centre; each step of bias doubles the pixels. In a
// shadow cascade (texelsPerMetre above 0) never finer than the map's texels show, which in a far one is coarser than
// the camera's pixels ask.
SFE_LOD_FN float sfeLodAllowed(float lodPixels, uint bias, float radius, float pixelsPerUnit, float distance, float texelsPerMetre) {
    float allowed = lodPixels * float(1u << bias) / max(2.0f * radius * pixelsPerUnit / max(distance, 1e-3f), 1e-6f);
    if (texelsPerMetre > 0.0f) {
        allowed = max(allowed, lodPixels / max(2.0f * (radius * texelsPerMetre), 1e-6f));
    }
    return allowed;
}

// The coarsest level of a chain of lodCount that strays from the full mesh by no more than `allowed`: errors are those
// of levels 1 on, a share of the mesh's extent growing down the chain (MeshLod's, GpuMesh::lodErrors).
SFE_LOD_FN uint sfeLodLevel(const float errors[6], uint lodCount, float allowed) {
    uint level = 0u;
    for (uint l = 1u; l < lodCount; ++l) {
        if (errors[l - 1u] <= allowed) {
            level = l;
        }
    }
    return level;
}
#endif
