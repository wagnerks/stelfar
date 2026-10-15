#version 450 core
#include "sfe.glsl"
#include "sfe_sky.glsl"
layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 8)) uniform sampler2DArrayShadow shadowMap;
layout(SFE_SET(0, 21)) uniform sampler2D cloudLayer;

layout(SFE_SET(0, 11), std140) uniform LightSpaceMatrices {
    mat4 lightSpaceMatrices[6];
};

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(SFE_SET(0, 15), std140) uniform PushConstants {
    vec3 viewPos;
    float density;
    vec3 lightDir;
    float anisotropy;
    vec3 lightColor;
    float intensity;
    vec2 resolution;
    int cascadeCount;
    float maxDistance;
    float cloudCoverage;
} params;

const int STEPS = 16;
const float PI = 3.14159265;

float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denom = pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
    return (1.0 - g2) / (4.0 * PI * denom);
}

// A point on the ray in every cascade's light space is linear in the distance along it (the light's projection is
// orthographic): each cascade's start and step are worked out once for the ray, not once a step.
vec4 cascadeStart[6];
vec4 cascadeStep[6];

float sunVisibility(float along) {
    int cc = params.cascadeCount < 1 ? 4 : params.cascadeCount;
    int layer = 0;
    vec3 proj = vec3(0.0);
    for (; layer < cc; ++layer) {
        vec4 ls = cascadeStart[layer] + cascadeStep[layer] * along;
        proj = ls.xyz / max(ls.w, 1e-4);
        proj = proj * 0.5 + 0.5;
        if (proj.x > 0.0 && proj.x < 1.0 && proj.y > 0.0 && proj.y < 1.0 && proj.z > 0.0 && proj.z < 1.0) {
            break;
        }
    }
    if (layer >= cc) {
        return 1.0;
    }
    return texture(shadowMap, vec4(proj.xy, float(layer), proj.z - 0.0015));
}

vec3 cameraRay(vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec4 viewP = matrices.invProjection * ndc;
    viewP /= max(viewP.w, 1e-4);
    return normalize(mat3(matrices.invView) * viewP.xyz);
}

void main() {
    vec3 posSample = sfeScenePosition(sceneDepth, TexCoords, matrices.invProjection, matrices.invView);
    vec3 origin = params.viewPos;
    vec3 rayDir;
    float rayLen;
    if (dot(posSample, posSample) > 1e-4) {
        vec3 delta = posSample - origin;
        rayLen = min(length(delta), params.maxDistance);
        rayDir = delta / max(length(delta), 1e-4);
    } else {
        rayDir = cameraRay(TexCoords);
        rayLen = params.maxDistance;
    }

    float stepLen = rayLen / float(STEPS);
    // Each texel of a 4x4 block starts its ray its own sixteenth of a step in (sfeBlockNoise), and volumetric_apply.fs
    // averages the block back together: sixteen times the steps, and nothing of the pattern left, still or moving.
    float t = sfeBlockNoise(gl_FragCoord.xy).x * stepLen;
    vec3 L = normalize(-params.lightDir);
    float phase = henyeyGreenstein(dot(rayDir, L), clamp(params.anisotropy, -0.9, 0.9));
    float inScatter = 0.0;
    float trans = 1.0;
    float dens = max(params.density, 0.0);
    float lit = 0.0;
    int cc = params.cascadeCount < 1 ? 4 : params.cascadeCount;
    for (int c = 0; c < cc; ++c) {
        cascadeStart[c] = lightSpaceMatrices[c] * vec4(origin, 1.0);
        cascadeStep[c] = lightSpaceMatrices[c] * vec4(rayDir, 0.0);
    }

    for (int i = 0; i < STEPS; ++i) {
        float vis = sunVisibility(min(t, rayLen));
        lit += vis;
        inScatter += params.intensity * dens * phase * vis * stepLen * trans;
        trans *= exp(-dens * stepLen);
        t += stepLen;
    }

    // Shafts, not haze: a ray lit all along its length, out in the open, scatters only a little, and one that crosses
    // shade -- under the trees -- keeps its light, which is where the shafts are seen.
    float shaded = 1.0 - lit / float(STEPS);
    float shafts = mix(0.2, 1.0, smoothstep(0.05, 0.5, shaded));
    // The light as one number, its colour the same for every ray (volumetric_apply.fs puts it back on), so the texel has
    // room for how far its ray went: the apply takes a texel at a pixel only where its ray ends about where the pixel's
    // own does.
    FragColor = vec4(inScatter * shafts, mix(1.0, trans, shafts), rayLen, 0.0);
}
