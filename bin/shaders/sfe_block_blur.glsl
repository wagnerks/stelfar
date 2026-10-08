#ifndef SFE_BLOCK_BLUR_GLSL
#define SFE_BLOCK_BLUR_GLSL
// The blur after a pass whose pattern each texel of a 4x4 block takes its own sixteenth of (sfeBlockNoise): ssao_blur.fs
// and sun_shadow_blur.fs. It averages exactly one block back together, so nothing is left of the pattern on a surface
// and nothing changes from frame to frame. The includer declares sceneDepth, gMaterial and matrices
// (SFE_SHARED_MATRICES); a texel of the input stands for the full-size pixel at `scale` times its place, its position
// from that pixel's depth exactly, so a texel by an edge is weighed by the surface it was worked out for.

vec3 sfeBlockBlurView(ivec2 t, int scale, vec2 texelFull, out bool sky) {
    return sfeSceneViewPosition(sceneDepth, (vec2(t * scale) + 0.5) * texelFull, matrices.invProjection, sky);
}

// A leaf or a blade: the two are not blurred into each other, or a tuft's grey spreads on the ground round it.
float sfeBlockBlurFoliage(ivec2 t, int scale) {
    return sfeIsFoliage(texelFetch(gMaterial, t * scale, 0)) ? 1.0 : 0.0;
}

// `src` round texel `t`; `skyValue` where nothing was drawn. A tap counts for little past `tolPerMetre` metres a metre
// from the eye, and `tolBase` more, off the centre's plane.
float sfeBlockBlur(sampler2D src, ivec2 t, int scale, float tolPerMetre, float tolBase, float skyValue) {
    ivec2 size = textureSize(src, 0);
    vec2 texelFull = 1.0 / vec2(textureSize(sceneDepth, 0));
    bool sky;
    vec3 p0 = sfeBlockBlurView(t, scale, texelFull, sky);
    float center = texelFetch(src, t, 0).r;
    if (sky) {
        return skyValue;
    }

    // A tap is weighed by how far it lies off the centre's plane (from the depth round it), not by how far it is: on
    // the ground seen low neighbouring texels are metres apart along it, and a plain distance left the blur only the
    // texels beside, across the view, so the noise stood in streaks. Off the plane by more than a few centimetres a
    // metre of distance (an edge, what stands behind) it counts for nothing.
    bool skyX;
    bool skyY;
    vec3 dpx = sfeBlockBlurView(min(t + ivec2(1, 0), size - 1), scale, texelFull, skyX) - p0;
    vec3 dpy = sfeBlockBlurView(min(t + ivec2(0, 1), size - 1), scale, texelFull, skyY) - p0;
    bool skyX0;
    bool skyY0;
    vec3 dmx = sfeBlockBlurView(max(t - ivec2(1, 0), ivec2(0)), scale, texelFull, skyX0) - p0;
    vec3 dmy = sfeBlockBlurView(max(t - ivec2(0, 1), ivec2(0)), scale, texelFull, skyY0) - p0;
    vec3 dx = skyX || (!skyX0 && abs(dmx.z) < abs(dpx.z)) ? -dmx : dpx;
    vec3 dy = skyY || (!skyY0 && abs(dmy.z) < abs(dpy.z)) ? -dmy : dpy;
    vec3 plane = cross(dx, dy);
    float plane2 = dot(plane, plane);
    // Degenerate only for the angle between the two steps, not their size: a millimetre a pixel near the eye made the
    // cross product smaller than any fixed bound, and the ground there turned to face the eye in a hard line.
    plane = plane2 > 1e-8 * dot(dx, dx) * dot(dy, dy) ? plane * inversesqrt(plane2) : vec3(0.0, 0.0, 1.0);
    float tolerance = tolPerMetre * -p0.z + tolBase;
    float leaf = sfeBlockBlurFoliage(t, scale);
    float sum = 0.0;
    float wsum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            ivec2 tt = clamp(t + ivec2(x, y), ivec2(0), size - 1);
            bool tapSky;
            vec3 dp = sfeBlockBlurView(tt, scale, texelFull, tapSky) - p0;
            if (tapSky) {
                continue;
            }
            float off = dot(dp, plane) / tolerance;
            // Half weight on the outer ring: the two texels four apart are the same sixteenth of the 4x4 block, so
            // every sixteenth counts once, centred.
            float w = (abs(x) == 2 ? 0.5 : 1.0) * (abs(y) == 2 ? 0.5 : 1.0) * exp(-off * off);
            w *= sfeBlockBlurFoliage(tt, scale) == leaf ? 1.0 : 0.02;
            sum += texelFetch(src, tt, 0).r * w;
            wsum += w;
        }
    }
    return wsum > 1e-6 ? sum / wsum : center;
}

#endif
