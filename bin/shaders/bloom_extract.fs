#version 450 core
#include "sfe.glsl"
#include "sfe_post.glsl"
layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D inputTexture;

// The frame a glare source (kGlare) was last seen in, counted as params.extra.w counts this one's (modulo kGlareFrames).
layout(SFE_SET(0, 16), std430) buffer GlareSeen {
    uint glareSeenAt;
};

// What is far brighter than anything lit: a glint mirroring the sun (kSfeGlint).
const float kGlare = 60.0;
const uint kGlareFrames = 1u << 20;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(inputTexture, 0));
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    const float k[3] = float[](0.227027, 0.316216, 0.070270);
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            float w = k[abs(x) < 3 ? abs(x) : 2] * k[abs(y) < 3 ? abs(y) : 2];
            vec3 c = texture(inputTexture, TexCoords + vec2(x, y) * texel * 2.0).rgb;
            acc += max(c, vec3(0.0)) * w;
            wsum += w;
        }
    }
    vec3 filtered = acc / max(wsum, 1e-4);
    // The sun's rays: the light between the camera and the sun, gathered along the line towards it, so every gap in
    // the clouds or the crowns that lets the bright sky through draws a shaft out of it. Bloom carries them, so they
    // are divided by its strength here and multiplied by it again with the rest of the glow.
    vec2 sun = vec2(params.sunU, params.sunV);
    // Only near the sun: a shaft is gathered along the line to it, and far enough away that line holds nothing but
    // what the fade below throws away anyway.
    if (params.extra.x > 0.0 && params.sunInFront > 0.5 && params.bloomStrength > 0.0 && distance(TexCoords, sun) < 0.8) {
        const int kSteps = 16;
        vec2 toSun = (sun - TexCoords) / float(kSteps) * 0.9;
        vec2 at = TexCoords;
        vec3 gathered = vec3(0.0);
        float weight = 1.0;
        float total = 0.0;
        for (int i = 0; i < kSteps; ++i) {
            at += toSun;
            // Only what is brighter than the scene around it: the sky in a gap, not the lit ground.
            gathered += max(texture(inputTexture, at).rgb - 1.6, vec3(0.0)) * weight;
            total += weight;
            weight *= 0.94;
        }
        // Fading out towards the edges of the screen, and away altogether once the sun itself is off it: the line to
        // a sun beyond the edge ends there, and what it gathers piles up along that edge as a bright band.
        vec2 off = abs(TexCoords - 0.5) * 2.0;
        float beyond = max(max(-sun.x, sun.x - 1.0), max(-sun.y, sun.y - 1.0));
        float onScreen = (1.0 - smoothstep(0.7, 1.0, max(off.x, off.y))) * (1.0 - smoothstep(0.0, 0.25, beyond));
        filtered += gathered / max(total, 1e-4) * params.extra.x * onScreen / params.bloomStrength;
    }
    // A glare: what is far brighter than anything lit (kGlare) throws a star of rays well past the glow, a flash that
    // blinds. Nothing else in a frame comes near that bright, so elsewhere it is nothing -- and most frames have no
    // source at all, while its rays cost most of this pass. So a source tells the frames after it, and the rays are
    // gathered only in the two after one was seen: they read the input on the same texels as this pixel's own (its
    // middle), stepped a pixel of this target at a time, so where no pixel's own was that bright they gather nothing.
    uint frame = uint(params.extra.w);
    if (any(greaterThan(texture(inputTexture, TexCoords).rgb, vec3(kGlare)))) {
        glareSeenAt = frame;
    }
    if (params.bloomStrength > 0.0 && ((frame - glareSeenAt) & (kGlareFrames - 1u)) <= 2u) {
        vec3 star = vec3(0.0);
        // Four rays, a cross; a step of four texels is one of this target's.
        const vec2 dirs[4] = vec2[](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
        for (int d = 0; d < 4; ++d) {
            for (int i = 1; i <= 24; ++i) {
                vec2 at = TexCoords + dirs[d] * texel * 4.0 * float(i);
                star += max(texture(inputTexture, at).rgb - kGlare, vec3(0.0)) * (1.0 - float(i) / 25.0);
            }
        }
        filtered += star * 0.01 / params.bloomStrength;
    }
    float lum = dot(filtered, vec3(0.2126, 0.7152, 0.0722));
    float knee = 0.6;
    float t = max(lum - 1.15, 0.0);
    float w = t * t / (t + knee);
    FragColor = vec4(filtered * (w / max(lum, 1e-4)), 1.0);
}
