#ifndef SFE_USER_SURFACE_GLSL
#define SFE_USER_SURFACE_GLSL
// An application's own say over the G-buffer surface (sfe_gbuffer_surface.glsl), in two calls. An application stands
// in for this file with one of the same name in its content's shaders/. The surface includes it at file scope after all
// its own declarations and before main, so a body here may read everything declared there: the surface's inputs
// (FragPos, TBN, the texture coordinates, vSurface, vPiece, vRest), the material's textures, its entry pc in the
// material table, uvOf and the helpers of sfe.glsl and sfe_material.glsl. It may declare inputs of its own at locations
// the vertex shader writes, a sampler only at a binding the pass already lays out, and file-scope variables to carry
// what the first call finds over to the second. It reads nothing that a shader including the surface declares after
// its include.
//
// sfeUserSurface is given the surface's linear albedo (its colour map's, after a triplanar texture and a shader's own
// style, times the material's own colour), its world normal (after the normal map, the clear coat's flattening and a
// triplanar texture's normals), and the roughness and the metalness as the material and its maps give them. It may
// change all four and may discard the fragment. The surface then dissolves a piece of a cut-out material
// (sfe_user_dissolve.glsl), takes a cut-out's normal from its geometry, guesses skin from the albedo, writes the albedo
// and the normal, and works the roughness and the metalness on for the scatter under the skin, the clear coat, the maps
// and a cut-out.
//
// sfeUserSurfaceLate is given the roughness and the metalness so worked, the specular factor (the material's, by its
// specular map, capped for a cut-out) and the clear coat's strength, and may change all four. The surface then floors a
// cut-out's roughness, filters the roughness by how the normal spreads in the pixel, and writes the four clamped.
//
// The engine's own: the surface as its material and maps give it.
void sfeUserSurface(inout vec3 linAlb, inout vec3 Nw, inout float roughness, inout float metallic) {
}

void sfeUserSurfaceLate(inout float roughness, inout float metallic, inout float specF, inout float coat) {
}
#endif
