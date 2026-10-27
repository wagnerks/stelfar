#ifndef SFE_USER_MIST_GLSL
#define SFE_USER_MIST_GLSL
// A hook of mist.fs: a medium of the application's own along the view ray, besides the engine's mist and haze. An
// application stands in for this file with one of the same name in its content's shaders/. mist.fs includes it at file
// scope after all its own declarations and before main, so a body here may read everything mist.fs declares: its
// MistBlock `fog`, the shared matrices, the scene depth, the sky, PI and the helpers of sfe.glsl. It may declare its
// own frame data in the user frame set, set 3, bindings 40-63 (UserFrameSet.h), which every pipeline compiling this
// hook lays out and binds.
//
// sfeUserMist is given the camera's position, the ray's unit direction, how far along it the pass looks (metres up to
// whatever the pixel shows, ending where the ray goes down through the mist's base and at the sky's distance), the
// camera's height above that base (never below 0) and the base's height in the world. It returns the medium's optical
// depth along that stretch of the ray, and writes in colour what the light scattered in it is multiplied by and in
// sunShare how much of the sun's forward glow it has (1 as much as the mist). Both are written on every path. The pass
// adds the depth to the mist's and the haze's, and lights the medium's part of the whole depth with sunShare of the
// sun's glow, multiplied by colour.
//
// The engine's own: no medium.
float sfeUserMist(vec3 camera, vec3 dir, float rayLength, float startHeight, float level, out vec3 colour,
                  out float sunShare) {
    colour = vec3(1.0);
    sunShare = 1.0;
    return 0.0;
}
#endif
