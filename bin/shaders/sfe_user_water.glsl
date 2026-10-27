#ifndef SFE_USER_WATER_GLSL
#define SFE_USER_WATER_GLSL
// What an application makes of its water, in the water's two fragment shaders: water.fs, the surface, and
// water_under.fs, the eye under it. These are the engine's defaults, which leave the water as WaterComponent and the
// engine have it and lay nothing on it. An application stands in for this file with one of the same name in its
// content's shaders/.
//
// Both shaders include it at file scope before main, after their `water` block (SFE_WATER_PARAMS) and PI, so a body
// of the application's may read those and everything sfe.glsl and sfe_water.glsl declare. A sampler of its own must
// be on a binding the pass already sets.

// The water at `xz` on the ground. `place` comes in as the engine has it (sfeWaterPlace: its own albedo, the
// clarity of WaterComponent, the whole mirror and white surf), and what this leaves in it is what the engine draws
// by: the albedo the water scatters back with (sfeWaterBody), the clarity its light is taken away by
// (sfeWaterExtinction), the share of the reflection kept, and the colour of the surf; under the water only the albedo
// and the clarity count. What it returns is the application's own, handed back unchanged to sfeUserWaterFilm as
// `user`.
float sfeUserWater(vec2 xz, inout SfeWaterPlace place) {
    return 0.0;
}

// What lies on the surface, seen from above it (water.fs only), laid over `color` (the water lit, refracted and
// reflected) before the engine adds the stylised light path, the sun's glitter, the surf and the water line. It is
// given sfeUserWater's `user`; `drifted`, this point in a frame the surface current carries along; the depth of water
// under it in metres; the way to the sun; the sun's light at the point, dimmed where it lies in shadow; the sky's
// light from straight up; and the clock in seconds. It returns how much of the sun's glitter it hides, 0 none to 1
// all.
float sfeUserWaterFilm(float user, vec2 drifted, float depthBelow, vec3 toSun, vec3 sunHere, vec3 skyLight,
                       float seconds, inout vec3 color) {
    return 0.0;
}

#endif
