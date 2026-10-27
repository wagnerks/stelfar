#ifndef SFE_GROUND_PATCH_GLSL
#define SFE_GROUND_PATCH_GLSL
// The ground drawn dense near the camera: a square of the terrain's mesh cut finer, on the very triangles the mesh has
// there (g_buffer_ground.vs builds its vertices), raised or lowered as the application has it (sfeUserGroundOffset),
// eased out to the mesh at its edge (sfeGroundPatchTake), where the mesh itself is put out of sight under it
// (sfeGroundUnderPatch). Included after the SharedMatrices block, named `matrices`, whose square and grid it reads; it
// binds nothing itself.
#include "sfe_user_ground.glsl"

// How much of the application's offset (sfeUserGroundOffset) the ground drawn dense near the camera
// (g_buffer_ground.vs) takes at `xz`: all of it but for its outer cell of the terrain's mesh, over which it eases out to
// none at its edge, where it meets the terrain's own mesh. None outside it, or with none.
float sfeGroundPatchTake(vec2 xz) {
    vec4 square = matrices.marksPatch;
    if (square.z <= 0.0) {
        return 0.0;
    }
    vec2 local = xz - square.xy;
    float edge = min(min(local.x, local.y), min(square.z - local.x, square.z - local.y));
    return smoothstep(0.0, matrices.marksGrid.z, edge);
}

#ifdef SFE_MATERIAL_TABLE_GLSL
// How far, at the least, the terrain's own mesh is put down where the ground is drawn dense; deeper still where the
// application lowers that ground further (twice its sfeUserGroundDeepest), so that the mesh's cells round the square's
// edge, sloping down from it, stay under the ground sfeGroundPatchTake eases out.
const float kSfeGroundUnder = 1.0;

// `world` on a mesh of `material`: put out of sight under the ground drawn dense if it is a vertex of the terrain's
// mesh (bit 128 of its map mask) strictly inside that square; those on its edge stay, so that the mesh round it is
// whole.
vec3 sfeGroundUnderPatch(vec3 world, uint material) {
    vec4 square = matrices.marksPatch;
    if (square.z > 0.0 && (uint(sfeMaterials[material].mapMask + 0.5) & 128u) != 0u) {
        vec2 local = world.xz - square.xy;
        float margin = 0.5 * matrices.marksGrid.z;
        if (all(greaterThan(local, vec2(margin))) && all(lessThan(local, vec2(square.z - margin)))) {
            world.y -= max(kSfeGroundUnder, 2.0 * sfeUserGroundDeepest());
        }
    }
    return world;
}
#endif

#endif
