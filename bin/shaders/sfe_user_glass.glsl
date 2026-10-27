#ifndef SFE_USER_GLASS_GLSL
#define SFE_USER_GLASS_GLSL
// What an application lays on a glass surface (glass.fs). It is called once the glass's refraction, reflection and
// highlight are found, before they are put together, and given N, the surface's normal turned to the eye; sunCol, the
// sun's light; nDotL, how squarely the surface faces the sun; and trans, what shows through the glass, which it may
// change. It returns x, how clouded the glass is, and y, how much of it is hidden under `over`, a colour it gives
// already lit. glass.fs then takes the glass's alpha towards 0.97 by x, dims its highlight by 1 - y, and takes its
// colour towards `over` and its alpha towards 1 by y. It is included after sfe.glsl, sfe_material.glsl and
// sfe_glass.glsl, whose declarations it may read, and before glass.fs's own functions; it may declare inputs of its own
// at locations the vertex shader writes. An application stands in for this file with one of the same name in its
// content's shaders/. This default lays nothing: the glass stays clear and uncovered.
vec2 sfeUserGlass(vec3 N, vec3 sunCol, float nDotL, inout vec3 trans, out vec3 over) {
    over = vec3(0.0);
    return vec2(0.0);
}
#endif
