#ifndef SFE_GROUND_PATCH_GLSL
#define SFE_GROUND_PATCH_GLSL
// The ground drawn dense near the camera (GroundMarksFrame::patch): a square of the terrain's mesh cut finer, on the very
// triangles the mesh has there, and pressed in by the marks as deep as they go, eased out to the mesh at its edge
// (sfeGroundPatchTake), where the mesh itself is put out of sight under it (sfeGroundUnderPatch). Drawn by
// Utils::drawGroundPatch: a row of quads an instance, a column's vertices two to it. Included after the SharedMatrices
// block, named `matrices`; its pass binds the terrain's heights at 35 of set 0 and the marks at 34.
#include "sfe_marks.glsl"

layout(SFE_SET(0, 35)) uniform sampler2D groundHeights;

struct SfeGroundVertex {
    vec3 world;
    vec2 local;   // in the terrain's own space, as its mesh's texCoords
    float height; // its mesh's there, in its own space, as its texCoords1.x
    vec3 normal;  // its mesh's, as it is shaded between its vertices
    vec3 tangent;
    float take;   // sfeGroundPatchTake
};

float sfeGroundHeight(ivec2 at) {
    return texelFetch(groundHeights, clamp(at, ivec2(0), textureSize(groundHeights, 0) - 1), 0).r;
}

// A vertex of the terrain's mesh as Terrain.cpp makes it: its height, its normal and tangent from its neighbours'.
void sfeGroundMeshVertex(ivec2 at, out float height, out vec3 normal, out vec3 tangent) {
    float spacing = matrices.marksGrid.z;
    height = sfeGroundHeight(at);
    float dx = (sfeGroundHeight(at + ivec2(1, 0)) - sfeGroundHeight(at - ivec2(1, 0))) / (2.0 * spacing);
    float dz = (sfeGroundHeight(at + ivec2(0, 1)) - sfeGroundHeight(at - ivec2(0, 1))) / (2.0 * spacing);
    normal = normalize(vec3(-dx, 1.0, -dz));
    tangent = normalize(vec3(1.0, dx, 0.0));
}

// The `vertex`th vertex of the `instance`th row: column vertex / 2 on the row's near side, or its far side if odd.
SfeGroundVertex sfeGroundPatchVertex(int vertex, int instance) {
    vec4 grid = matrices.marksGrid;
    vec4 square = matrices.marksPatch;
    ivec2 at = ivec2(vertex >> 1, instance + (vertex & 1));
    int per = max(int(round(grid.z / square.w)), 1);
    ivec2 cell = ivec2(round((square.xy - grid.xy) / grid.z)) + at / per;
    vec2 into = vec2(at % per) / float(per);
    // The cell's corners: a, b along x, c along z, d across; split along b-c as the mesh is.
    vec4 weights = into.x + into.y <= 1.0 ? vec4(1.0 - into.x - into.y, into.x, into.y, 0.0)
        : vec4(0.0, 1.0 - into.y, 1.0 - into.x, into.x + into.y - 1.0);
    float ha, hb, hc, hd;
    vec3 na, nb, nc, nd, ta, tb, tc, td;
    sfeGroundMeshVertex(cell, ha, na, ta);
    sfeGroundMeshVertex(cell + ivec2(1, 0), hb, nb, tb);
    sfeGroundMeshVertex(cell + ivec2(0, 1), hc, nc, tc);
    sfeGroundMeshVertex(cell + ivec2(1, 1), hd, nd, td);

    SfeGroundVertex v;
    v.height = dot(weights, vec4(ha, hb, hc, hd));
    v.normal = na * weights.x + nb * weights.y + nc * weights.z + nd * weights.w;
    v.tangent = ta * weights.x + tb * weights.y + tc * weights.z + td * weights.w;
    vec2 xz = grid.xy + (vec2(cell) + into) * grid.z;
    v.local = xz - grid.xy - 0.5 * grid.z * vec2(textureSize(groundHeights, 0) - 1);
    v.take = sfeGroundPatchTake(xz);
    float marks = 0.0;
    if (v.take > 0.0) {
        marks = sfeMarksHeight(xz, vec2(square.w, 0.0), vec2(0.0, square.w), sfeMarksCover(xz)) * v.take;
    }
    v.world = vec3(xz.x, grid.w + v.height + marks, xz.y);
    return v;
}

#endif
