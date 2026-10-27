#ifndef SFE_USER_GROUND_GLSL
#define SFE_USER_GROUND_GLSL
// The application's say in the ground drawn dense near the camera (sfe_ground_patch.glsl, g_buffer_ground.vs): how far
// it raises or lowers that ground at a place. The engine takes all of the offset inside the square and eases it out to
// none over its outer cell of the terrain's mesh (sfeGroundPatchTake), adding it to the mesh's own height there; and it
// puts the terrain's own mesh under the square at least twice as deep as the deepest the application lowers it, which
// the patch's producer gives (TerrainPatch::deepest). An application stands in for this file with one of the same name
// in its content's shaders/. It is included at file scope by g_buffer_ground.vs alone, after its SharedMatrices
// block, named `matrices`, which the application's copy may read. These defaults leave the ground as the terrain's
// mesh has it.
//
// The application's own frame data is in the user frame set, set 3, bindings 40-63 (UserFrameSet.h), which the ground
// patch's pipeline lays out and binds: what the application's copy reads of its own is declared there.

// Metres the ground is raised at world `xz` (lowered where it is below zero), filtered over `footprint` metres either
// way: the spacing of the vertices it is drawn with there.
float sfeUserGroundOffset(vec2 xz, float footprint) {
    return 0.0;
}

#endif
