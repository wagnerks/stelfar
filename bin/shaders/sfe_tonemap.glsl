#ifndef SFE_TONEMAP_GLSL
#define SFE_TONEMAP_GLSL
// The tonemap post.fs and post_tonemap.fs share: the frame and its bloom to display colour. Include sfe_post.glsl first.
layout(SFE_SET(0, 0)) uniform sampler2D inputTexture;
layout(SFE_SET(0, 1)) uniform sampler2D bloomColor;
// r the log luminance the eye has adapted to, g how much of the sun is in sight; see luminance_adapt.fs.
layout(SFE_SET(0, 2)) uniform sampler2D adaptation;

// The luminance auto exposure brings a scene's log average to. Middle grey, set against how the lighting is scaled.
const float kExposureKey = 0.11;

// AgX (after Troy Sobotka's, in Benjamin Wrensch's fitted form): highlights desaturate towards white as they
// brighten instead of skewing hue, so a low sun stays gold rather than turning orange, and foliage keeps its greens.
vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 agx(vec3 color) {
    const mat3 toAgx = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    color = toAgx * max(color, vec3(1e-10));
    color = clamp(log2(color), minEv, maxEv);
    return agxContrast((color - minEv) / (maxEv - minEv));
}

// The grade, in AgX's own space, then back to display: contrast as a power around the middle, saturation around the
// luminance.
vec3 agxToDisplay(vec3 color) {
    color = pow(max(color, vec3(0.0)), vec3(params.contrast));
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float saturation = params.saturation;
    // The time of day: a low sun warms the lights and leaves the shade cool, midday is as it is, and after dusk the
    // colour drains out of everything towards blue.
    if (params.extra.z > 0.0) {
        float sunY = params.sunHeight;
        float golden = (1.0 - smoothstep(0.04, 0.45, sunY)) * smoothstep(-0.12, 0.02, sunY) * params.extra.z;
        float night = (1.0 - smoothstep(-0.14, 0.02, sunY)) * params.extra.z;
        float lights = smoothstep(0.25, 0.85, luma);
        color += (vec3(0.05, 0.015, -0.045) * lights - vec3(0.02, 0.0, -0.03) * (1.0 - lights)) * golden;
        color += vec3(-0.015, 0.0, 0.03) * night * (1.0 - lights);
        saturation *= 1.0 + 0.08 * golden - 0.35 * night;
    }
    color = luma + saturation * (max(color, vec3(0.0)) - luma);
    const mat3 fromAgx = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                              -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                              -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    // AgX's inverse gives display-encoded values already.
    return clamp(fromAgx * color, 0.0, 1.0);
}

float exposureScale() {
    if (params.autoExposure < 0.5) {
        return 1.0;
    }
    float adapted = exp(texture(adaptation, vec2(0.5)).r);
    return clamp(kExposureKey / max(adapted, 1e-4), 0.35, 8.0) * params.extra.y;
}

// `gain` the white balance times the exposure: the same for every pixel and every tap, so worked out once.
vec3 mapColor(vec3 hdr, vec3 gain) {
    return agxToDisplay(agx(hdr * gain));
}

// White balance and exposure, the same for every pixel of a frame.
vec3 sfeToneGain() {
    // Warmer takes blue out and adds red.
    return vec3(1.0 + 0.12 * params.warmth, 1.0, 1.0 - 0.12 * params.warmth) * params.exposure * exposureScale();
}

vec3 tonemapped(vec2 uv, vec3 gain) {
    vec3 hdr = texture(inputTexture, uv).rgb;
    if (params.bloomStrength != 0.0) {
        hdr += texture(bloomColor, uv).rgb * params.bloomStrength;
    }
    return mapColor(hdr, gain);
}

#endif
