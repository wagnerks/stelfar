#version 450 core
#include "sfe.glsl"
#include "sfe_material.glsl"
#include "sfe_glass.glsl"
// Where this is on the model at rest (g_buffer.vs): a piece of litter's look laid over it.
layout(location = 14) in vec3 vRest;
#include "sfe_look.glsl"

uint hashGridTint(vec3 worldPos, float spacing) {
    int gx = int(floor(worldPos.x / spacing - 0.5));
    int gz = int(floor(worldPos.z / spacing - 0.5));
    uint h = uint(gx) * 374761393u + uint(gz) * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return h;
}

vec3 hsvToRgb(float h, float s, float v) {
    float c = v * s;
    float x = c * (1.0 - abs(mod(h * 6.0, 2.0) - 1.0));
    float m = v - c;
    vec3 rgb;
    if (h < 1.0 / 6.0) rgb = vec3(c, x, 0.0);
    else if (h < 2.0 / 6.0) rgb = vec3(x, c, 0.0);
    else if (h < 3.0 / 6.0) rgb = vec3(0.0, c, x);
    else if (h < 4.0 / 6.0) rgb = vec3(0.0, x, c);
    else if (h < 5.0 / 6.0) rgb = vec3(x, 0.0, c);
    else rgb = vec3(c, 0.0, x);
    return rgb + m;
}

vec3 glassTintFromPos(vec3 worldPos) {
    uint h = hashGridTint(worldPos, max(params.sphereSpacing, 1.0));
    float hue = float(h % 12u) / 12.0;
    float sat = 0.55 + float((h >> 4u) & 3u) * 0.12;
    float val = 0.75 + float((h >> 6u) & 3u) * 0.08;
    return hsvToRgb(hue, sat, val);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec2 toUv(vec3 worldPos, out float clipW) {
    vec4 clip = matrices.PV * vec4(worldPos, 1.0);
    clipW = clip.w;
    return clip.xy / max(clip.w, 1e-4) * 0.5 + 0.5;
}

vec3 envColor(vec3 dir) {
    vec3 d = normalize(dir);
    vec3 cube = textureLod(prefilterMap, d, 0.0).rgb;

    vec3 L = normalize(-params.lightDir);
    if (dot(params.lightDir, params.lightDir) < 1e-4) {
        L = normalize(vec3(0.35, 0.8, 0.25));
    }
    vec3 sunCol = params.lightColor;
    if (dot(sunCol, sunCol) < 1e-6) {
        sunCol = vec3(1.0, 0.96, 0.88);
    }
    float sun = pow(max(dot(d, L), 0.0), 64.0);
    return cube + sunCol * sun * 8.0;
}

vec3 sampleScene(vec2 uv) {
    return texture(sceneCopy, uv).rgb;
}

// Ray through a convex sphere front cap: longer path near center, thin at silhouette.
float spherePathFactor(vec3 N, vec3 V) {
    float nDotV = clamp(dot(N, V), 0.0, 1.0);
    return sqrt(nDotV);
}

vec3 refractScene(vec3 origin, vec3 dir, float marchDist) {
    float w = 0.0;
    vec2 uv = toUv(origin + dir * marchDist, w);
    if (w <= 1e-4) {
        return vec3(0.0);
    }
    vec2 ca = mat3(matrices.view)[0].xy * 0.0045;
    vec3 ss;
    ss.r = sampleScene(uv + ca).r;
    ss.g = sampleScene(uv).g;
    ss.b = sampleScene(uv - ca).b;
    return ss;
}

vec3 screenReflect(vec3 origin, vec3 dir, vec3 fallback) {
    vec3 hit = fallback;
    float best = 0.0;
    for (int i = 1; i <= 10; ++i) {
        float t = float(i) * 1.15;
        float w = 0.0;
        vec2 uv = toUv(origin + dir * t, w);
        if (w <= 1e-4) {
            continue;
        }
        float onScreen = float(uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0);
        float weight = onScreen * (1.0 - float(i - 1) / 10.0);
        if (weight > best) {
            best = weight;
            hit = sampleScene(uv);
        }
    }
    return mix(fallback, hit, clamp(best, 0.0, 1.0));
}

void main() {
    if (sfeNearFaded(gl_FragCoord)) {
        discard;
    }
    vec3 Ns = cross(dFdx(FragPos), dFdy(FragPos));
    vec3 Nv = TBN[2];
    vec3 N = (dot(Nv, Nv) > 1e-4) ? normalize(Nv) : vec3(0.0);
    if (dot(Ns, Ns) > 1e-12) {
        Ns = normalize(Ns);
        if (dot(N, N) < 0.5) {
            N = Ns;
        } else {
            N = normalize(N + Ns);
        }
    }
    vec3 V = normalize(params.viewPos - FragPos);
    if (dot(N, V) < 0.0) {
        N = -N;
    }
    float NdotV = clamp(dot(N, V), 0.0, 1.0);

    SfeMaterial material = sfeMaterials[vMaterial];
    float ior = max(material.ior, 1.02);
    float eta = 1.0 / ior;
    vec3 I = -V;
    vec3 R = reflect(I, N);
    vec3 T = refract(I, N, eta);
    bool totalReflect = dot(T, T) <= 1e-6;

    // opacity drives colored absorption inside the volume, not surface alpha.
    float density = clamp(material.opacity, 0.02, 1.0);
    float path = spherePathFactor(N, V);
    vec3 albedo = pow(max(texture(albedoMaps[uint(material.albedoSlot)], TexCoords).rgb, vec3(0.0)), vec3(2.2));
    vec3 tint = max(material.tint * max(albedo, vec3(0.02)), vec3(0.02));
    vec3 absorb = (vec3(1.0) - tint) * density * 5.0 + vec3(0.015);

    float roughness = clamp(params.roughness, 0.01, 0.12);
    float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    vec3 F = fresnelSchlick(NdotV, vec3(f0));
    if (totalReflect) {
        F = vec3(1.0);
    }

    // Refraction: near surface + deeper sample for bulk (solid ball, not soap film).
    float refractScale = mix(0.35, 2.8, path) / ior;
    vec3 transNear = refractScene(FragPos, totalReflect ? R : T, refractScale * 0.55);
    vec3 transFar = refractScene(FragPos, totalReflect ? R : T, refractScale * 1.65);
    vec3 trans = transNear;
    if (dot(transFar, transFar) > 1e-6) {
        trans = mix(transNear, transFar, 0.55);
    }
    if (dot(trans, trans) <= 1e-6) {
        trans = envColor(totalReflect ? R : T);
    }
    trans *= exp(-absorb * path * 2.2);

    vec3 L = normalize(-params.lightDir);
    if (dot(params.lightDir, params.lightDir) < 1e-4) {
        L = normalize(vec3(0.35, 0.8, 0.25));
    }
    vec3 sunCol = params.lightColor;
    if (dot(sunCol, sunCol) < 1e-6) {
        sunCol = vec3(1.0, 0.96, 0.88);
    }

    vec3 envR = envColor(R);
    vec3 ssR = screenReflect(FragPos, R, envR);
    vec3 cubeR = textureLod(prefilterMap, R, roughness * 2.0).rgb;
    vec3 refl = mix(envR, ssR, 0.18);
    refl = max(refl, cubeR * 0.85);
    float envSun = pow(max(dot(normalize(R), L), 0.0), 48.0);
    refl += sunCol * envSun * 6.0;

    vec3 H = normalize(V + L);
    float nh = max(dot(N, H), 0.0);
    float nDotL = max(dot(N, L), 0.0);
    // Wide + hot core: a GGX needle is subpixel on small meshes and dies in TAA.
    float spec = (pow(nh, 16.0) * 6.0 + pow(nh, 64.0) * 10.0) * nDotL;
    vec3 highlight = sunCol * spec + sunCol * envSun * 4.0;

    // A piece of litter's glass (sfeLookedGlass): clouded, what shows through it scattered milky; soot, silt and the
    // ground's dirt over it, lit as a rough surface by the sun and the sky.
    vec2 look = vec2(0.0);
    vec3 soil = vec3(0.0);
    if (vLook.x != 0u) {
        look = sfeLookedGlass(soil);
        vec3 sky = textureLod(prefilterMap, N, 4.0).rgb;
        trans = mix(trans, mix(trans, vec3(sfeLuma(trans)), 0.6) * 0.7 + sky * 0.2, look.x);
        soil *= sky + sunCol * nDotL / 3.14159;
    }

    vec3 base = mix(trans, refl, F);
    float alpha = mix(0.70, 0.92, F.x);
    alpha = max(alpha, density * 0.75);
    alpha = max(alpha, clamp(spec * 0.4, 0.0, 1.0));
    alpha = mix(alpha, 0.97, look.x);
    vec3 color = max(base * alpha + highlight * (1.0 - look.y), vec3(0.0));
    FragColor = vec4(mix(color, soil, look.y), mix(alpha, 1.0, look.y));
}
