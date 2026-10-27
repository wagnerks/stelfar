#ifndef SFE_USER_GROUND_GLSL
#define SFE_USER_GROUND_GLSL
// The application's say in the ground drawn dense near the camera (sfe_ground_patch.glsl, g_buffer_ground.vs): how far
// it raises or lowers that ground at a place, and how far down it lowers it at most. The engine takes all of the offset
// inside the square and eases it out to none over its outer cell of the terrain's mesh (sfeGroundPatchTake), adding it
// to the mesh's own height there; and it puts the terrain's own mesh under the square at least twice as deep as the
// deepest. An application stands in for this file with one of the same name in its content's shaders/. It is included
// at file scope by sfe_ground_patch.glsl, after the includer's SharedMatrices block, named `matrices`, which the
// application's copy may read. These defaults leave the ground as the terrain's mesh has it.
//
// Through sfe_ground_patch.glsl the file is compiled into g_buffer.vs too, the vertex shader of every mesh, where only
// sfeUserGroundDeepest runs (sfeGroundUnderPatch); and into any shader of the application's that includes the patch.
// So everything the application's copy declares is declared in all of them: a sampler of its own must be on a binding
// every such pass already lays out, or only declared and never read where the pass does not.

// Metres the ground is raised at world `xz` (lowered where it is below zero), filtered over `footprint` metres either
// way: the spacing of the vertices it is drawn with there.
float sfeUserGroundOffset(vec2 xz, float footprint) {
    return 0.0;
}

// The deepest sfeUserGroundOffset lowers the ground anywhere, metres, as a positive number.
float sfeUserGroundDeepest() {
    return 0.0;
}

#endif
