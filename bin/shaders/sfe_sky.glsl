#ifndef SFE_SKY_GLSL
#define SFE_SKY_GLSL

// The outdoor sky: single-scattering atmosphere, a cloud layer and the sun disk. The atmosphere itself is computed
// on the CPU by renderModule/Atmosphere.cpp and looked up here.

// Mirrors SFE::Render::SkyParams. std140.
#define SFE_SKY_PARAMS \
    mat4 invViewProj;  \
    vec4 toSun;        /* xyz towards the sun, w cloud coverage */ \
    vec4 sunColor;     /* rgb sun radiance reaching the ground, w seconds for the cloud drift */ \
    vec4 cameraPos;    /* xyz, w sky brightness per unit of sun */ \
    vec4 wind;         /* xz the direction the wind blows along, y its strength, w how cartoon the look is */

// The sky's radiance towards `dir` for this frame's sun, per unit of sky brightness, from the texture
// Atmosphere::skyTexture bakes: marching the atmosphere for every pixel cost a pass as much as all its other work.
vec3 sfeSkyRadiance(sampler2D skyLut, vec3 dir) {
    dir = normalize(dir);
    vec2 uv = vec2(atan(dir.z, dir.x) / 6.2831853 + 0.5, sqrt(clamp(dir.y, 0.0, 1.0)));
    return textureLod(skyLut, uv, 0.0).rgb;
}

// How far the wind has carried the cloud layer, in metres: the layer moves the way the wind blows (the pattern is read
// at the point minus this). Faster up there than on the ground, and faster than real clouds, whose drift a mile up is
// too slow to see in a few seconds.
vec2 sfeCloudDrift(vec4 wind, float seconds) {
    return -wind.xz * (20.0 + 60.0 * wind.y) * seconds;
}

// The cloud layer's texture (scripts/cloud_texture.py): r where the clouds are, g the lumps they are piled from, b a
// finer curl over them. One tile covers this many metres, which the script's SIDE_METRES sets.
const float kSfeCloudTile = 27273.0;

// How high the clouds float, and how thick the layer is where a cloud is piled highest.
const float kSfeCloudHeight = 1400.0;
const float kSfeCloudThickness = 800.0;

// The cloud field at a point of the layer: below zero there is no cloud, and it grows towards the middle of one.
// `detail` is how much of the finest scale to take: none of it is all a shadow on the ground needs, and near the
// horizon, where the layer is seen almost edge on, it only compresses into a ripple.
float sfeCloudField(sampler2D clouds, vec2 xz, float coverage, vec2 drift, float detail) {
    vec3 baked = texture(clouds, (xz + drift) / kSfeCloudTile).rgb;
    // The baked field is flat, so the threshold is simply how much of the sky is left clear.
    float threshold = 1.0 - clamp(coverage, 0.0, 1.0);
    float lumps = mix(baked.g, baked.g * 0.8 + baked.b * 0.2, detail);
    // Deep inside the cloud the lumps cannot open it up again; at its edge they are what eats it away.
    return (baked.r - threshold) * 1.0 + (lumps - 0.42) * 0.95 + 0.04;
}

// How much of the sky a point of the layer hides, in [0, 1].
float sfeCloudDensity(sampler2D clouds, vec2 xz, float coverage, vec2 drift, float detail) {
    return smoothstep(0.0, 0.17, sfeCloudField(clouds, xz, coverage, drift, detail));
}

// How far a cloud stands above the bottom of the layer, in metres: thin and flat at the edges, piled high in the
// middle. This is what gives the layer a shape to catch the light on and a silhouette that bulges.
float sfeCloudTop(float field) {
    return smoothstep(-0.05, 0.5, field) * kSfeCloudThickness;
}

// The stars, as points on a grid over the sky: one to a cell, where the cell's own number says there is one. The whole
// field turns with the sun, so the night sky wheels over the night the way the real one does.
float sfeStars(vec3 dir, vec3 toSun) {
    if (dir.y < -0.02) {
        return 0.0;
    }
    // Turned by where the sun stands, so the stars keep their places against it rather than against the ground.
    float turn = atan(toSun.z, toSun.x);
    float c = cos(turn);
    float sn = sin(turn);
    vec3 d = vec3(dir.x * c - dir.z * sn, dir.y, dir.x * sn + dir.z * c);
    const float kCells = 220.0;
    vec2 sky = vec2(atan(d.z, d.x) * 0.15915494, asin(clamp(d.y, -1.0, 1.0)) * 0.31830989) * kCells;
    vec2 cell = floor(sky);
    uint h = sfeCellHash(ivec2(cell));
    // Two thirds of the cells are empty, and of the rest most stars are faint.
    float lit = float(h & 0xffu) / 255.0;
    if (lit < 0.62) {
        return 0.0;
    }
    vec2 at = vec2(float((h >> 8) & 0xffu), float((h >> 16) & 0xffu)) / 255.0;
    float star = 1.0 - smoothstep(0.0, 0.22, length(sky - cell - at));
    float bright = (lit - 0.62) / 0.38;
    return star * star * bright * bright;
}

// The sky painted, for the cartoon look: two colours and a clean sweep between them where the real one scatters, a
// sun and a moon drawn large enough to read, and clouds as round heaps in two flat tones with a bright rim where the
// light catches their edge. Cheaper than the real one: two reads of the cloud layer, both coarse.
vec3 sfeCartoonSky(sampler2D skyLut, sampler2D clouds, vec3 dir, vec3 toSun, vec3 sunColor, float brightness, float coverage, vec2 drift, bool withSunDisk) {
    dir = normalize(dir);
    vec3 around = normalize(vec3(dir.x, 0.0, dir.z) + vec3(1e-4, 0.0, 0.0));
    vec3 horizon = sfeSkyRadiance(skyLut, normalize(around + vec3(0.0, 0.04, 0.0))) * brightness;
    vec3 zenith = sfeSkyRadiance(skyLut, vec3(0.0, 1.0, 0.0)) * brightness;
    float night = clamp((0.06 - toSun.y) / 0.16, 0.0, 1.0);
    // A painted sky is a clean blue overhead, however murky the real one is with a low sun: the real zenith's
    // brightness, in the blue a picture book would use -- a deep one at night.
    float greyZenith = dot(zenith, vec3(0.2126, 0.7152, 0.0722));
    // And bright by day whatever the hour: a low sun darkens the real sky long before a painter's does.
    greyZenith = mix(greyZenith, max(greyZenith, 0.1 * brightness), 1.0 - night);
    vec3 paintedBlue = mix(vec3(0.32, 0.56, 1.0), vec3(0.18, 0.26, 0.62), night);
    zenith = mix(zenith, paintedBlue * greyZenith * 1.9, 0.7);
    // The horizon a paler blue, except towards the sun, where the warm glow of the real one is what a painter keeps.
    float greyHorizon = dot(horizon, vec3(0.2126, 0.7152, 0.0722));
    greyHorizon = mix(greyHorizon, max(greyHorizon, 0.12 * brightness), 1.0 - night);
    vec2 sunAround = normalize(toSun.xz + vec2(1e-4));
    float sunSide = pow(max(dot(around.xz, sunAround), 0.0), 2.0) * (1.0 - night);
    vec3 paleBlue = mix(vec3(0.66, 0.8, 1.0), vec3(0.3, 0.36, 0.62), night);
    horizon = mix(horizon, paleBlue * greyHorizon * 1.35, 0.7 * (1.0 - 0.75 * sunSide));
    vec3 sky = mix(horizon, zenith, pow(smoothstep(-0.02, 0.6, dir.y), 0.8));

    if (withSunDisk) {
        // Drawn several times the size of the real one, with a wide soft glow: a sun a child would draw.
        float cosSun = dot(dir, toSun);
        float disk = smoothstep(0.99905, 0.99935, cosSun);
        float glow = pow(max(cosSun, 0.0), 60.0) * 0.3 + pow(max(cosSun, 0.0), 8.0) * 0.08;
        sky += sunColor * (disk * 5.0 + glow) * step(0.0, dir.y + 0.02) * (1.0 - night);
    }
    if (night > 0.0) {
        sky += vec3(0.85, 0.9, 1.0) * sfeStars(dir, toSun) * night * 0.08;
        if (withSunDisk) {
            float cosMoon = dot(dir, -toSun);
            float disk = smoothstep(0.9989, 0.99915, cosMoon);
            float halo = pow(max(cosMoon, 0.0), 90.0) * 0.35 + pow(max(cosMoon, 0.0), 12.0) * 0.06;
            sky += vec3(1.0, 0.96, 0.82) * (disk * 1.6 + halo * brightness * 0.05) * night * step(0.0, dir.y + 0.02);
        }
    }

    if (dir.y > 0.01 && coverage > 0.001) {
        vec2 xz = dir.xz * (kSfeCloudHeight / dir.y);
        // The coarse lumps only: a cartoon cloud is a few big round heaps, not a froth of small ones.
        float field = sfeCloudField(clouds, xz, coverage, drift, 0.0);
        // A clean edge: the body is there or it is not, over a narrow soft rim.
        float body = smoothstep(0.0, 0.05, field);
        if (body > 0.001) {
            // What lights it: the sun by day, the moon by night.
            vec3 toLight = night > 0.5 ? -toSun : toSun;
            vec3 lightColour = night > 0.5 ? horizon * 3.5 + vec3(0.06, 0.07, 0.1) * brightness * 0.02 : sunColor;
            vec2 towardLight = normalize(toLight.xz + vec2(1e-4));
            float ahead = sfeCloudField(clouds, xz + towardLight * 700.0, coverage, drift, 0.0);
            // Two tones with a soft step between: lit along its edges and on the side towards the light, and a
            // darker belly deep inside, away from it -- a heap seen from below, not a ring drawn round a shape.
            float facing = clamp(0.5 + (field - ahead) * 2.0, 0.0, 1.0);
            float deep = smoothstep(0.08, 0.4, field);
            float tone = smoothstep(0.4, 0.6, facing * 0.7 + (1.0 - deep) * 0.6);
            vec3 skyTone = mix(zenith, horizon, 0.55);
            float skyGrey = dot(skyTone, vec3(0.2126, 0.7152, 0.0722));
            // The belly the sky's own colour, a touch towards violet, as painted shade is.
            vec3 shade = skyTone * 1.2 + vec3(0.04, 0.0, 0.1) * skyGrey;
            // The lit side a warm white a little brighter than the sky round it, not the sun's full glare: a
            // painted cloud sits in its sky rather than burning out of it.
            vec3 lit = vec3(1.0, 0.97, 0.92) * skyGrey * 2.1 + lightColour * 0.12;
            vec3 cloud = mix(shade, lit, tone);
            // The rim: a thin bright band round the edge, strongest towards the light.
            float edge = 1.0 - smoothstep(0.03, 0.14, field);
            float towards = pow(max(dot(dir, toLight), 0.0), 3.0);
            cloud += (lightColour * 0.2 + vec3(skyGrey)) * edge * (0.1 + 0.8 * towards);
            float fade = smoothstep(0.015, 0.2, dir.y);
            sky = mix(sky, cloud, body * fade);
        }
    }
    return sky;
}

// Everything above the ground along dir: the atmosphere, the clouds in front of it and, when asked, the sun disk.
vec3 sfeSky(sampler2D skyLut, sampler2D clouds, vec3 dir, vec3 toSun, vec3 sunColor, float brightness, float coverage, vec2 drift, bool withSunDisk, float cartoon) {
    // The painted sky instead, past halfway: the two are too different to blend into anything but mud.
    if (cartoon > 0.5) {
        return sfeCartoonSky(skyLut, clouds, dir, toSun, sunColor, brightness, coverage, drift, withSunDisk);
    }
    dir = normalize(dir);
    vec3 sky = sfeSkyRadiance(skyLut, dir) * brightness;

    if (withSunDisk) {
        float cosSun = dot(dir, toSun);
        // About half a degree across, with a soft limb.
        float disk = smoothstep(0.99996, 0.99999, cosSun);
        float glow = pow(max(cosSun, 0.0), 900.0) * 0.6;
        sky += sunColor * (disk * 40.0 + glow) * step(0.0, dir.y + 0.02);
    }

    // Night: the stars come out as the sun goes down, and the moon stands opposite it.
    float night = clamp((0.06 - toSun.y) / 0.16, 0.0, 1.0);
    if (night > 0.0) {
        sky += vec3(0.85, 0.9, 1.0) * sfeStars(dir, toSun) * night * 0.05;
        if (withSunDisk) {
            vec3 toMoon = -toSun;
            float cosMoon = dot(dir, toMoon);
            float disk = smoothstep(0.99965, 0.99985, cosMoon);
            float halo = pow(max(cosMoon, 0.0), 700.0) * 0.5;
            sky += vec3(0.95, 0.96, 1.0) * (disk * 2.5 + halo) * night * step(0.0, dir.y + 0.02);
        }
    }

    if (dir.y > 0.01 && coverage > 0.001) {
        // The ray meets the bottom of the layer here; where the cloud is piled high it meets its dome earlier, so the
        // point is read again at that height. That one step is what makes a cloud a body standing in the sky rather
        // than a pattern painted on it: the near side of a heap hides its far side, and its edge bulges.
        // Walked up the dome of the cloud rather than straight to the flat layer: the step raises the point to the
        // height the cloud reaches there and reads it again, so a heap hides its own far side and its edge bulges.
        // The walk and the slopes read the coarse lumps only; the fine ones are for the silhouette, once, at the end.
        if (!withSunDisk) {
            // A reflection reads the sky for every ray that leaves the screen, and what it shows is a broken,
            // shifting patch of it: the dome walk and its slopes would be paid for a hundred times over for
            // something no eye can follow. The flat layer, lit by how thick it is, is enough there.
            float sheet = sfeCloudField(clouds, dir.xz * (kSfeCloudHeight / dir.y), coverage, drift, 0.0);
            float density = smoothstep(0.0, 0.17, sheet);
            if (density > 0.001) {
                float high = sfeCloudTop(sheet) / kSfeCloudThickness;
                vec3 ambient = sfeSkyRadiance(skyLut, vec3(0.0, 1.0, 0.0)) * brightness;
                vec3 cloud = ambient * (1.15 + 0.85 * high) + sunColor * (1.4 * high * high + 0.12);
                sky = mix(sky, cloud, density * smoothstep(0.015, 0.22, dir.y) * 0.94);
            }
            return sky;
        }
        float detail = smoothstep(0.08, 0.32, dir.y);
        vec2 base = dir.xz * (kSfeCloudHeight / dir.y);
        vec2 xz = base;
        float coarse = sfeCloudField(clouds, xz, coverage, drift, 0.0);
        if (coarse > -0.25) {
            // Along the ray to the height the cloud reaches here -- but not far: seen at a low angle that step runs
            // kilometres sideways, and the cloud smears into ribbons instead of standing up.
            vec2 shift = dir.xz * (sfeCloudTop(coarse) * 0.32 / dir.y);
            float reach = length(shift);
            if (reach > 260.0) {
                shift *= 260.0 / reach;
            }
            xz = base + shift;
            coarse = sfeCloudField(clouds, xz, coverage, drift, 0.0);
        }
        float field = sfeCloudField(clouds, xz, coverage, drift, detail);
        float density = smoothstep(0.0, 0.17, field);
        if (density > 0.001) {
            // The dome's own slope, from how the height changes a step either way: the light falls on it as on any
            // surface, so one side of every heap is lit and the other is not.
            const float kStep = 120.0;
            float here = sfeCloudTop(coarse);
            float slopeX = (sfeCloudTop(sfeCloudField(clouds, xz + vec2(kStep, 0.0), coverage, drift, 0.0)) - here) / kStep;
            float slopeZ = (sfeCloudTop(sfeCloudField(clouds, xz + vec2(0.0, kStep), coverage, drift, 0.0)) - here) / kStep;
            vec3 N = normalize(vec3(-slopeX, 1.0, -slopeZ));
            // Wrapped: a cloud is lit through as much as on, so the turn from light to shade is a long one.
            float lambert = clamp((dot(N, toSun) + 0.35) / 1.35, 0.0, 1.0);
            // The shade of the clouds up-sun of this one, which is what makes a bank of them dark underneath.
            vec2 towardSun = normalize(toSun.xz + vec2(1e-4));
            float far = sfeCloudDensity(clouds, xz + towardSun * 2200.0, coverage, drift, 0.0);
            float deep = exp(-far * 1.6);
            float mu = dot(dir, toSun);
            // The rim of a cloud with the sun behind it: thin there, so the light comes through.
            float silver = pow(max(mu, 0.0), 8.0) * (1.0 - density * 0.85) * 2.2;
            vec3 ambient = sfeSkyRadiance(skyLut, vec3(0.0, 1.0, 0.0)) * brightness;
            // How high this part of the cloud stands: the tops are in the sun and white, the flanks and the base grey.
            float high = sfeCloudTop(max(field, coarse)) / kSfeCloudThickness;
            float light = mix(high, lambert, 0.5);
            vec3 cloud = ambient * (1.15 + 0.5 * high + 0.35 * lambert) + sunColor * (2.4 * light * light * deep + 0.5 * silver + 0.12 * deep);
            // Thin towards the horizon, where the layer is seen edge on through a lot of haze.
            float fade = smoothstep(0.015, 0.22, dir.y);
            sky = mix(sky, cloud, density * fade * 0.94);
        }
    }
    return sky;
}

#endif
