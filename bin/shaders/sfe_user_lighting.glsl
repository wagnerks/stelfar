#ifndef SFE_USER_LIGHTING_GLSL
#define SFE_USER_LIGHTING_GLSL
// The lighting pass's hooks (deferred_shading.fs): where an application changes the light of what is drawn. An
// application stands in for this file with one of the same name in its content's shaders/; these, the engine's own,
// change nothing.
//
// deferred_shading.fs takes this file once, at file scope, after all its declarations and before its main: a copy may
// read every one of them (the PushConstants block pc, the G-buffer's and the IBL's samplers, matrices, the sun's
// shadow, sfe.glsl and sfe_pbr.glsl), and may declare a sampler of its own only on a binding the pass's layout already
// has. A copy cannot take this file in by its name: the name leads to the copy itself.

// The albedo a surface is lit with, at the world position FragPos; ground is true on the ground's material
// (sfeIsGround), metallic is the G-buffer's. Called once a pixel, before anything reads the albedo: the sun's and the
// point lights' direct light, the F0, the ambient light, the sheen, the light under the skin and the debug view of the
// albedo all take what it hands back. The G-buffer itself keeps the albedo it has (the reflections read that).
void sfeUserLitAlbedo(vec3 FragPos, bool ground, float metallic, inout vec3 albedo) {
}

// The colour of the pixel once lit and fogged, with whatever is in the air between the eye (pc.viewPos) and FragPos
// over it: V is the unit vector from FragPos towards the eye, Depth how far FragPos is along the view. Called after
// sfeUserLitAlbedo of the same pixel (so what that keeps in a variable at file scope is there to read), and not in a
// debug view. The stylise's fuller colours and the outlines go over what it returns.
vec3 sfeUserAir(vec3 color, vec3 FragPos, vec3 V, float Depth) {
    return color;
}

#endif
