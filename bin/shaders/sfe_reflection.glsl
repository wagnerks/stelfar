#ifndef SFE_REFLECTION_GLSL
#define SFE_REFLECTION_GLSL
// Reflections off the screen's picture, for what mirrors the scene (water.fs, particle_liquid_surface.fs). Included
// after the shader declares `matrices` (SFE_SHARED_MATRICES) and `sceneDepth`, the scene's depth.

// The view distance along -z of an NDC depth, from the camera's projection (OpenGL convention).
float sfeViewDistance(float ndcDepth) {
    return matrices.projection[3][2] / ((ndcDepth * 2.0 - 1.0) + matrices.projection[2][2]);
}

// Screen-space reflection: marches `dir` from `origin` through the scene's depth and refines where it first passes
// behind a surface. The step is a fixed number of pixels rather than metres, so the far end of a long reflection is
// searched as finely as the near end: with steps that grow in metres a ray crosses whole trees at a stride and the
// reflection lands in blocks. How far it looks is cut down with distance -- a reflection thirty metres off is a few
// pixels of the picture, and nobody reads the far shore in it. The bottom (seen through the water at that pixel) does
// not count: what is below `level` (the lake's surface; far below it for none).
bool sfeMarchReflection(vec3 origin, vec3 dir, float level, float eyeDistance, vec2 pixelSize, out vec2 hitUv) {
    // Fewer steps, and a shorter reach, the further the water is from the eye: a reflection thirty metres off is a
    // few pixels of the picture, and nobody reads the far shore in it.
    float far = smoothstep(8.0, 70.0, eyeDistance);
    int steps = int(mix(26.0, 12.0, far));
    // A step is so many pixels of the screen -- fine enough not to jump over a tree -- but never so short that a ray
    // running along the water never gets anywhere: at a grazing angle a pixel is worth tens of metres out there.
    float stride = mix(5.0, 12.0, far);
    // Then a long binary search for where the crossing is: with a short one the hit is only known to the width of a
    // step, which lays the reflection down in blocks.
    const int kRefine = 6;
    // The ray's clip position is linear in the distance along it: two products once instead of one a step.
    vec4 clipOrigin = matrices.PV * vec4(origin, 1.0);
    vec4 clipDir = matrices.PV * vec4(dir, 0.0);
    float travelled = 0.0;
    float lastTravelled = 0.0;
    hitUv = vec2(0.0);
    for (int i = 0; i < steps; ++i) {
        vec4 clip = clipOrigin + clipDir * travelled;
        if (clip.w <= 1e-4) {
            return false;
        }
        // A step that moves the point `stride` pixels across the screen -- fine enough not to jump over a tree --
        // and never shorter than a small part of the distance: a ray running almost along the water hardly moves on
        // screen at all, and steps measured in pixels alone would never carry it to the far shore.
        vec2 onScreen = (clipDir.xy * clip.w - clip.xy * clipDir.w) / (clip.w * clip.w) * 0.5;
        float perMetre = length(onScreen / pixelSize);
        float step = clamp(stride / max(perMetre, 1e-4), max(0.3, clip.w * 0.04), 40.0);
        lastTravelled = travelled;
        travelled += step;
        clip = clipOrigin + clipDir * travelled;
        if (clip.w <= 1e-4) {
            return false;
        }
        vec2 at = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, vec2(1.0)))) {
            return false;
        }
        float sceneDepthValue = textureLod(sceneDepth, at, 0.0).r;
        float rayDistance = clip.w;
        float surfaceDistance = sfeViewDistance(sceneDepthValue);
        // Behind a surface, and not so far behind it that the ray has only passed under something standing clear.
        // How far behind a surface the ray may be and still count as having hit it: a step's worth at least, and a
        // slice of the distance besides -- what a tree is thick from here. Too tight and the ray steps clean through
        // the far shore and the water shows only sky; too loose and it catches things it passed under.
        float thickness = max(step * 2.0, rayDistance * 0.1) + 0.3;
        if (sceneDepthValue < 1.0 && rayDistance > surfaceDistance && rayDistance - surfaceDistance < thickness) {
            float lo = lastTravelled;
            float hi = travelled;
            for (int r = 0; r < kRefine; ++r) {
                float mid = 0.5 * (lo + hi);
                vec4 midClip = clipOrigin + clipDir * mid;
                vec2 midAt = midClip.xy / midClip.w * 0.5 + 0.5;
                if (midClip.w > sfeViewDistance(textureLod(sceneDepth, midAt, 0.0).r)) {
                    hi = mid;
                }
                else {
                    lo = mid;
                }
            }
            vec4 hitClip = clipOrigin + clipDir * hi;
            hitUv = hitClip.xy / hitClip.w * 0.5 + 0.5;
            vec3 hitPos = sfeScenePosition(sceneDepth, hitUv, matrices.invProjection, matrices.invView);
            return hitPos.y > level - 0.05;
        }
    }
    return false;
}

// How much of a reflection found at `hitUv` to take, fading out toward the screen's edges where it runs off it.
float sfeReflectionOnScreen(vec2 hitUv) {
    vec2 edge = smoothstep(vec2(0.0), vec2(0.08), hitUv) * (1.0 - smoothstep(vec2(0.92), vec2(1.0), hitUv));
    return edge.x * edge.y;
}
#endif
