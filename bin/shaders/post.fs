#version 450 core
#include "sfe.glsl"
#include "sfe_post.glsl"
layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

#include "sfe_tonemap.glsl"
// The frame already through the tonemap, from post_tonemap.fs.
layout(SFE_SET(0, 3)) uniform sampler2D toneMapped;

// Sun lens flare: a glow round the sun, a faint halo ring, and ghosts strung along the line through the centre of
// the image, all fading as the sun goes out of sight behind anything.
vec3 lensFlare(vec2 uv) {
    float seen = texture(adaptation, vec2(0.5)).g;
    if (params.flareStrength <= 0.0 || params.sunInFront < 0.5 || seen <= 0.001) {
        return vec3(0.0);
    }
    vec2 sun = vec2(params.sunU, params.sunV);
    vec2 scale = vec2(params.aspect, 1.0);
    vec2 toCentre = vec2(0.5) - sun;
    // Weaker as the sun nears the edge of the frame.
    float onScreen = 1.0 - smoothstep(0.35, 0.75, length((sun - 0.5) * scale));
    vec3 flare = vec3(0.0);

    float fromSun = length((uv - sun) * scale);
    flare += vec3(1.0, 0.85, 0.6) * exp(-fromSun * 7.0) * 0.18;
    flare += vec3(0.4, 0.6, 1.0) * exp(-pow((fromSun - 0.32) / 0.015, 2.0)) * 0.03;

    const int kGhosts = 5;
    const float along[kGhosts] = float[](0.45, 0.8, 1.25, 1.6, 2.1);
    const float size[kGhosts] = float[](0.035, 0.06, 0.025, 0.09, 0.05);
    const vec3 tint[kGhosts] = vec3[](vec3(1.0, 0.7, 0.35), vec3(0.45, 0.9, 0.55), vec3(0.6, 0.55, 1.0), vec3(1.0, 0.8, 0.5), vec3(0.5, 0.7, 1.0));
    for (int i = 0; i < kGhosts; ++i) {
        vec2 at = sun + toCentre * along[i];
        float d = length((uv - at) * scale);
        float disc = 1.0 - smoothstep(size[i] * 0.6, size[i], d);
        flare += tint[i] * disc * 0.07;
    }
    return flare * params.sunColor.rgb * params.flareStrength * seen * onScreen;
}

void main() {
    vec2 uv = TexCoords;
    vec2 ca = (uv - 0.5) * params.ca;
    vec3 gain = sfeToneGain();

    vec3 hdr;
    hdr.r = texture(inputTexture, uv + ca).r;
    hdr.g = texture(inputTexture, uv).g;
    hdr.b = texture(inputTexture, uv - ca).b;
    if (params.bloomStrength != 0.0) {
        hdr += texture(bloomColor, uv).rgb * params.bloomStrength;
    }
    hdr += lensFlare(uv);

    vec3 mapped = mapColor(hdr, gain);

    // Contrast adaptive sharpening, after the tonemap and clamped to what the four neighbours already
    // contain. The pass used to add a third of an unsharp mask to the HDR values instead: an edge between
    // a bright and a dark sample overshot at both ends, the tonemap turned the overshoot into a coloured
    // fringe, and every pixel of aliasing TAA had just averaged away came back amplified. The amount here
    // falls off where the neighbourhood is already near black or near white, which is where a plain
    // unsharp mask does its worst.
    // The neighbours from post_tonemap.fs, which mapped every pixel once: four more tonemaps a pixel here cost more than
    // the rest of the pass. Texel centres, so the filter hands back exactly what was written.
    vec2 texel = 1.0 / vec2(textureSize(toneMapped, 0));
    vec3 north = texture(toneMapped, uv - vec2(0.0, texel.y)).rgb;
    vec3 south = texture(toneMapped, uv + vec2(0.0, texel.y)).rgb;
    vec3 west = texture(toneMapped, uv - vec2(texel.x, 0.0)).rgb;
    vec3 east = texture(toneMapped, uv + vec2(texel.x, 0.0)).rgb;
    vec3 lo = min(mapped, min(min(north, south), min(west, east)));
    vec3 hi = max(mapped, max(max(north, south), max(west, east)));
    vec3 headroom = clamp(min(lo, 1.0 - hi) / max(hi, vec3(1e-4)), 0.0, 1.0);
    vec3 weight = sqrt(headroom) * (-1.0 / mix(8.0, 5.0, clamp(params.sharpness, 0.0, 1.0)));
    mapped = clamp(((north + south + west + east) * weight + mapped) / (1.0 + 4.0 * weight), lo, hi);

    FragColor = vec4(mapped, 1.0);
}
