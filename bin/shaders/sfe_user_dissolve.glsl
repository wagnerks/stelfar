#ifndef SFE_USER_DISSOLVE_GLSL
#define SFE_USER_DISSOLVE_GLSL
// Which texels of a dissolving piece stay, and in what colour: an application's say over the engine's dissolve. A
// material dissolves the pieces of its cut-out surface whose key (vPiece, sfePieceKey) is under its share gone
// (SfeMaterial::bare): in the masked surface and its depth pre-pass (sfe_gbuffer_masked.glsl), in the default surface
// where it cuts out its alpha (sfe_gbuffer_surface.glsl), and in the cut-out casters of the sun's and the point lights'
// shadows (cascadeShadowMap.fs, pointLightMap.fs). An application stands in for this file with one of the same name in
// its content's shaders/. It is included at file scope after sfe.glsl and sfe_material.glsl, in every one of those
// passes, so a body here reads nothing but its argument and the helpers of those two.
//
// sfeUserDissolveKeeps is given a texel's linear albedo (its colour map's, times the material's own colour) and says
// whether it stays: a texel that does not is discarded, and casts no shadow. sfeUserDissolved is given the albedo of a
// texel that stays and returns the colour it is drawn in; the shadow passes do not call it.
//
// The engine's own: a dissolving piece is gone whole.
bool sfeUserDissolveKeeps(vec3 albedo) {
    return false;
}

vec3 sfeUserDissolved(vec3 albedo) {
    return albedo;
}
#endif
