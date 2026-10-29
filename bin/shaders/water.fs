#version 450 core
#include "sfe.glsl"
#include "sfe_sky.glsl"
#include "sfe_water.glsl"
// A lake surface. Under it: the lit image, bent by the waves and dimmed by the water the light crosses on its way
// up from the bottom. On it: what the reflected rays run into on screen, or the sky, by Fresnel; the sun's glitter;
// and foam where it is shallow enough to touch the shore.
layout(location = 0) in vec3 vWorld;
layout(location = 0) out vec4 FragColor;

layout(SFE_SET(0, 15), std140) uniform WaterBlock {
    SFE_WATER_PARAMS
} water;

layout(SFE_SET(0, 6)) uniform sampler2D sceneColor;
layout(SFE_SET(0, 7)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
layout(SFE_SET(0, 20)) uniform sampler2D skyLut;
layout(SFE_SET(0, 21)) uniform sampler2D cloudLayer;
// The fine ripple (scripts/water_ripple_texture.py): rg the slope, over a tile this many metres across.
layout(SFE_SET(0, 22)) uniform sampler2D rippleMap;
const float kRippleTile = 4.0;
// The sun's shadow on the water (sfe_sun_shadow.glsl), on these bindings (WaterPass).
#define SFE_SUN_SHADOW_DEPTH SFE_SET(0, 8)
#define SFE_SUN_SHADOW_BLOCK SFE_SET(0, 9)
#define SFE_SUN_SHADOW_MATRICES SFE_SET(0, 10)
#include "sfe_sun_shadow.glsl"

const float PI = 3.14159265;

// The slope of the surface, in world x/z: a sum of wind waves from about 4 m long down to capillary ripples of a few
// centimetres, each running at the speed deep water gives its length, spread in direction around the wind and
// strongest where a gust roughens the water. Waves shorter than the pixel footprint are left out rather than aliased;
// `variance` gathers the slope they would have had: far off they are too small to draw, but the water they roughen is
// no mirror, and the glitter and reflections spread by it.
vec2 waveSlope(vec2 p, float seconds, float footprint, vec2 stepX, vec2 stepY, vec4 wind, float depth, vec2 toShore, out float variance) {
    vec2 along = wind.xz;
    // Coming into the shallows a wave slows where it is shallowest and swings round until it runs straight at the
    // shore, whatever way the wind blows; it shortens and steepens as it goes. Deeper than a few metres it does not
    // feel the bottom at all.
    float shoaling = dot(toShore, toShore) > 0.5 ? smoothstep(5.0, 0.5, depth) : 0.0;
    float strength = wind.y;
    vec2 slope = vec2(0.0);
    variance = 0.0;

    const int kWaves = 16;
    const float g = 9.81;
    // Wind waves on a lake a few hundred metres across hardly grow past a couple of metres.
    const float kSharpness = 1.05;
    const float kFirstWavelength = 2.2;
    // Gusts darken the water in patches that sweep across it with the wind; between them it lies nearly still.
    float gust = smoothstep(0.25, 0.8, sfeFbm((p - along * seconds * (1.5 + 4.0 * strength)) * 0.05, 3));
    float rough = mix(0.15, 1.0, strength) * mix(0.62, 1.0, gust);
    // Breaks up the regularity of the sines: every wave swells and fades over a few metres, and its crests bend
    // instead of running as straight lines.
    // Both on the scale of the ripples themselves: a slow warp metres wide swirls the whole pattern like something
    // thick being stirred.
    // Waves only shorten along the list, so once one has faded out every later one has too, and all they add is their
    // variance: the breakup, the bend and the sines are only worked out for the waves still drawn.
    // Everything shorter than this is the ripple map's: summing it here only aliases, and the map has mips to
    // average it with.
    const float kFinest = 0.45;
    float wavelength = kFirstWavelength;
    int drawn = 0;
    while (drawn < kWaves && kFirstWavelength * pow(0.72, float(drawn)) > kFinest
        && footprint / (kFirstWavelength * pow(0.72, float(drawn))) < 0.2) {
        ++drawn;
    }
    float breakup = 0.0;
    float wander = 0.0;
    vec2 bent = p;
    if (drawn > 0) {
        // Which way the chop runs here: worked out once for the whole set, not once a wave.
        wander = sfeFbm(p * 0.035, 2) - 0.5;
        breakup = sfeFbm(p * 0.9 + along * seconds * 0.6, 2);
        // Two scales of stirring: a wide slow one that swings whole stretches of the pattern about, and a close one
        // that bends the crests themselves. Without the wide one the same ripple repeats across the water.
        bent = p + (vec2(sfeFbm(p * 0.45, 2), sfeFbm(p * 0.45 + 17.3, 2)) - 0.5) * 1.6
                 + (vec2(sfeFbm(p * 0.07, 2), sfeFbm(p * 0.07 + 5.1, 2)) - 0.5) * 7.0;
    }
    for (int i = 0; i < kWaves; ++i) {
        if (i >= drawn) {
            float steepness = 0.09 * rough * mix(0.45, 1.15, float(i) / float(kWaves)) * mix(0.5, 1.0, strength);
            variance += 0.5 * steepness * steepness;
            continue;
        }
        // Shortened where it feels the bottom.
        float k = 6.2831853 / (shoaling > 0.002 ? wavelength * mix(1.0, 0.7, shoaling) : wavelength);
        // Scattered around the wind, the short ones furthest from it.
        // Wind chop is not one train of waves: they run at every angle to the wind, the short ones most of all.
        float spread = (fract(sin(float(i) * 12.9898 + 4.1) * 43758.5453) * 2.0 - 1.0) * (0.9 + 0.8 * float(i) / float(kWaves));
        // Wandering by where on the water this is, so the crests are not the same straight lines everywhere; each
        // wave takes the turn differently.
        spread += wander * (1.4 + 1.1 * fract(float(i) * 0.618));
        float c = cos(spread);
        float s = sin(spread);
        vec2 dir = vec2(along.x * c - along.y * s, along.x * s + along.y * c);
        if (shoaling > 0.002) {
            dir = normalize(mix(dir, toShore, shoaling));
        }
        // Steepness falls off gently towards the ripples; the longest waves only come up with the wind.
        float steepness = 0.09 * rough * mix(0.45, 1.15, float(i) / float(kWaves)) * mix(0.5, 1.0, strength) * mix(1.0, 1.7, shoaling);
        float amplitude = steepness / k;
        // Its own place in the cycle, from its number rather than a step of it: with even steps the whole set lines
        // up every so many metres and the water reads as a tiled pattern.
        float phase = k * dot(dir, bent) - sqrt(g * k) * seconds + fract(sin(float(i) * 78.233 + 1.7) * 43758.5453) * 6.2831853;
        // Peaked crests over wide flat troughs, as wind ripples are, instead of a sine's rounded swell: the height
        // exp(sharpness * (sin - 1)), its slope scaled back so the mean stays that of the sine.
        float peak = exp(kSharpness * (sin(phase) - 1.0)) * kSharpness;
        // Broken along its own crest: a wind ripple runs a few metres and dies, and the next one starts somewhere
        // else. Without this every crest is a line across the whole lake and the water reads as corrugated.
        vec2 crest = vec2(-dir.y, dir.x);
        float swell = 0.5 + 0.5 * sin(dot(crest, p) * 0.85 + float(i) * 2.3 + breakup * 6.2831853);
        float modulation = mix(0.15, 1.75, swell) * mix(0.6, 1.3, 0.5 + 0.5 * sin(breakup * 6.2831853 * (1.0 + float(i) * 0.37) + float(i)));
        // How far a pixel reaches across this wave, not across the water: seen at a grazing angle a pixel is long
        // one way and short the other, and a wave running along the long way is the one that aliases into streaks.
        float across = abs(dot(dir, stepX)) + abs(dot(dir, stepY));
        // Gone before a wave is two pixels long: nearer the limit its crests alias into lines.
        float fade = 1.0 - smoothstep(0.06, 0.2, across / wavelength);
        slope += dir * (amplitude * k * cos(phase) * peak * modulation * fade);
        variance += 0.5 * steepness * steepness * (1.0 - fade);
        // Not a clean ratio, and not the same one twice: harmonics of one length are what a pattern repeats on.
        wavelength *= 0.72 + (fract(sin(float(i) * 45.164 + 9.7) * 43758.5453) - 0.5) * 0.12;
    }
    return slope;
}

#include "sfe_reflection.glsl"

// What an application makes of its water (sfe_user_water.glsl; the engine's defaults leave it as it is).
#include "sfe_user_water.glsl"

void main() {
    // No discard: the quad is the water square exactly, and without one the depth test rejects what stands in front of
    // the water before this runs.
    float level = water.surface.y;
    float seconds = water.sunColor.w;
    vec3 toSun = water.toSun.xyz;
    vec3 sun = water.sunColor.rgb;
    float brightness = water.cameraPos.w;

    vec3 toEye = water.cameraPos.xyz - vWorld;
    float eyeDistance = length(toEye);
    vec3 V = toEye / eyeDistance;
    vec2 uv = gl_FragCoord.xy * water.look.yz;
    float footprint = length(fwidth(vWorld.xz));

    // What lies under this pixel, straight down the view ray.
    vec3 bottom = sfeScenePosition(sceneDepth, uv, matrices.invProjection, matrices.invView);
    // Nothing drawn under the water here (the far side of the square): take it as deep.
    float depthBelow = dot(bottom, bottom) < 1e-6 ? 50.0 : max(level - bottom.y, 0.0);

    // Which way the shore lies: the depth falls off towards it, so the way it falls fastest, taken from how it
    // changes across the screen, points that way.
    vec2 toShore = vec2(0.0);
    {
        vec2 alongX = dFdx(vWorld.xz);
        vec2 alongY = dFdy(vWorld.xz);
        float det = alongX.x * alongY.y - alongX.y * alongY.x;
        // Only where the screen still tells the world apart: at a grazing angle a pixel covers metres of water and
        // the slope it reads is noise.
        if (abs(det) > 1e-5 && footprint < 1.5 && depthBelow < 12.0) {
            float dx = dFdx(depthBelow);
            float dy = dFdy(depthBelow);
            vec2 gradient = vec2(alongY.y * dx - alongX.y * dy, alongX.x * dy - alongY.x * dx) / det;
            if (dot(gradient, gradient) > 1e-8) {
                toShore = -normalize(gradient);
            }
        }
    }

    float variance;
    vec2 slope = waveSlope(vWorld.xz, seconds, footprint, dFdx(vWorld.xz), dFdy(vWorld.xz), water.wind, depthBelow, toShore, variance);
    float spread = sqrt(variance);
    {
        // The ripples the sum left out because they are smaller than a pixel: taken from the baked map instead, where
        // the mip chain averages them into a sheen rather than aliasing them into streaks. Two layers of it at
        // different sizes, sliding over each other with the wind, so the tile does not read as a pattern.
        vec2 drift = water.wind.xz * (0.4 + 0.9 * water.wind.y) * seconds;
        vec2 fine = (texture(rippleMap, (vWorld.xz + drift) / kRippleTile).xy - 0.5) * 2.0
                  + (texture(rippleMap, (vWorld.xz * 0.63 - drift * 0.8 + 13.7) / kRippleTile).xy - 0.5) * 1.4;
        slope += fine * spread * 0.8;
        // What is drawn is no longer only guessed at: the rest still spreads the glitter and the reflections.
        spread *= 0.55;
    }
    // Painted water is nearly still: the waves are there, but only enough to break a reflection, not to speckle it.
    float cartoon = water.wind.w;
    slope *= mix(1.0, 0.22, cartoon);
    vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));
    vec3 skyLight = sfeSkyRadiance(skyLut, vec3(0.0, 1.0, 0.0)) * brightness * PI;
    // The sun where nothing stands between it and the water, as on the ground: the water a flat plane, its steps across
    // it known exactly. What the sun lights on it -- the water's own colour, the glitter, the foam and what lies on
    // the water -- by that.
    float sunLit = 1.0 - cascadedShadow.direction_intensity.w
        * ShadowCascadedCalculation(vWorld, vec3(0.0, 1.0, 0.0), false, 0.0);
    vec3 sunHere = sun * sunLit;
    // The water here as an application leaves it (sfe_user_water.glsl): its albedo, clarity, mirror and surf colour,
    // and a value of its own for what it lays on the surface further down.
    SfeWaterPlace place = sfeWaterPlace(water.look.x);
    float user = sfeUserWater(vWorld.xz, place);
    // In the shade the sky is dimmed as well, as on the ground (shadowAmbient): the sun alone was too little of dark
    // water's light for a shadow to show on it.
    vec3 waterBody = sfeWaterBody(sunHere, toSun, skyLight * mix(water.current.z, 1.0, sunLit), place.albedo);

    // Seen from under the water: out through the surface only inside the circle overhead the light bends into (about
    // 49 degrees from straight up), and past it the surface is a mirror of the dim water below. The water between the
    // eye and here is the underwater pass's.
    if (water.cameraPos.y < level) {
        vec3 T = refract(-V, -N, 1.33);
        vec3 through = textureLod(sceneDepth, uv, 0.0).r >= 1.0
            ? sfeSky(skyLut, cloudLayer, T, toSun, sun, brightness, water.toSun.w, sfeCloudDrift(water.wind, seconds), true, water.wind.w)
            : texture(sceneColor, uv + T.xz * 0.02).rgb;
        float mirror = dot(T, T) < 1e-6 ? 1.0 : 0.02 + 0.98 * pow(1.0 - max(T.y, 0.0), 5.0);
        FragColor = vec4(mix(through, waterBody * 1.5, mirror), 1.0);
        return;
    }


    // Refraction: the image under the surface displaced by the waves, less where it is shallow, and not onto
    // something that stands above the water.
    vec2 bend = N.xz * 0.05 * clamp(depthBelow, 0.0, 1.0) / max(1.0, eyeDistance * 0.08);
    vec2 refractUv = uv + bend;
    vec3 refractBottom = sfeScenePosition(sceneDepth, refractUv, matrices.invProjection, matrices.invView);
    if (refractBottom.y > level) {
        refractUv = uv;
        refractBottom = bottom;
    }
    vec3 under = texture(sceneColor, refractUv).rgb;
    // Caustics: the waves focus sunlight into a drifting net of bright lines on the bottom.
    if (dot(refractBottom, refractBottom) > 1e-6) {
        under *= sfeCausticLight(refractBottom, level, toSun, seconds, 3.5);
    }
    float path = dot(refractBottom, refractBottom) < 1e-6 ? 50.0 : distance(refractBottom, vWorld);

    // Light that survives the path through the water, and what the water itself scatters back in its place.
    vec3 transmitted = exp(-sfeWaterExtinction(place.clarity) * path);
    vec3 refracted = under * transmitted + waterBody * (1.0 - transmitted);

    // Reflection: whatever on screen the reflected ray runs into, faded out towards the screen's edge, and the sky
    // where it finds nothing. What is off screen is not reflected: a mirrored camera drawing the scene a second time
    // cost more than all the rest of the water, for the tree tops above the top of the frame.
    vec3 R = reflect(-V, N);
    R.y = abs(R.y);
    // The sky only where the reflection lets some of it through.
    vec3 reflected;
    vec2 hitUv;
    // A ray that leaves the water steeply has nothing to meet but sky: what stands on the shore is low and far, and
    // marching for it only to miss is most of the pass. Near the shore, where reeds and branches lean over the water,
    // it is still worth looking.
    bool worthMarching = water.look.w > 0.5 && (R.y < 0.4 || depthBelow < 1.5);
    if (worthMarching && sfeMarchReflection(vWorld + vec3(0.0, 0.02, 0.0), normalize(R), level, eyeDistance, water.look.yz, hitUv)) {
        float onScreen = sfeReflectionOnScreen(hitUv);
        vec3 hitColor = texture(sceneColor, hitUv).rgb;
        reflected = onScreen >= 1.0 ? hitColor
            : mix(sfeSky(skyLut, cloudLayer, R, toSun, sun, brightness, water.toSun.w, sfeCloudDrift(water.wind, seconds), false, water.wind.w), hitColor, onScreen);
    }
    else {
        reflected = sfeSky(skyLut, cloudLayer, R, toSun, sun, brightness, water.toSun.w, sfeCloudDrift(water.wind, seconds), false, water.wind.w);
    }

    float cosTheta = max(dot(N, V), 0.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - cosTheta, 5.0);
    // A painted lake is one blue that takes some sky, not a mirror that turns clear underfoot: the mix stays much the
    // same wherever you look, which is what makes it read as a flat sheet of water.
    fresnel = mix(fresnel, mix(0.35, 0.85, fresnel), cartoon);
    // As much of the mirror as the place keeps.
    fresnel *= place.mirror;
    vec3 color = mix(refracted, reflected, fresnel);

    // This point in a frame the surface current carries along: what is laid out by it drifts with the water, as what
    // floats on it does.
    vec2 drifted = vWorld.xz - water.current.xy * seconds;
    // What an application lays on the surface (sfe_user_water.glsl), drawn over the water's colour; `film` is how much
    // of the sun's glitter it hides.
    float film = sfeUserWaterFilm(user, drifted, depthBelow, toSun, sunHere, skyLight, seconds, color);

    if (cartoon > 0.5) {
        // The light's path drawn as a handful of flat streaks lying across the water, the way a picture shows a moon
        // path: a noise cut hard into ovals, stretched along the line to the light and drifting with the wind.
        vec3 toLight = toSun.y > 0.02 ? toSun : -toSun;
        vec3 lightColour = toSun.y > 0.02 ? sunHere : vec3(0.7, 0.78, 1.0) * brightness * 0.03;
        vec2 along = normalize(toLight.xz + vec2(1e-4));
        vec2 across = vec2(-along.y, along.x);
        vec2 p = vWorld.xz - along * seconds * 0.35;
        // Stretched across the path, short along it.
        // Sized on screen rather than in the world: a painter draws the same little ovals near and far, so the
        // pattern is read finer the further off the water is.
        float scale = 0.45 * (0.5 + eyeDistance * 0.075);
        vec2 stretched = vec2(dot(p, along) * 0.35, dot(p, across) * 1.6) * scale;
        float n = sfeFbm(stretched, 3);
        float streak = smoothstep(0.55, 0.62, n) * (1.0 - smoothstep(0.62, 0.72, n) * 0.35);
        // Only where the light would glance off the water towards the eye, and thinning into the distance.
        vec3 H = normalize(V + toLight);
        float lobe = pow(max(dot(N, H), 0.0), 8.0);
        float path = smoothstep(0.55, 0.95, lobe) * 0.6 + pow(max(dot(reflect(-V, vec3(0.0, 1.0, 0.0)), toLight), 0.0), 60.0);
        color += lightColour * streak * clamp(path, 0.0, 1.5) * 0.32 * (1.0 - smoothstep(90.0, 260.0, eyeDistance));
    }

    // The sun's glitter: a sharp lobe on the waves, softened in the distance where many facets share a pixel.
    vec3 H = normalize(V + toSun);
    // As rough as the detail left out makes it: far off a wide, soft path of light instead of a hard mirror streak.
    float roughness = clamp(sqrt(sqrt(0.0016 + 2.0 * variance + footprint * footprint * 0.0004)), 0.1, 0.6);
    float a2 = roughness * roughness * roughness * roughness;
    float nh = max(dot(N, H), 0.0);
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float ggx = a2 / (PI * d * d);
    float sunFresnel = 0.02 + 0.98 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    color += sunHere * ggx * sunFresnel * max(dot(N, toSun), 0.0) * 0.07 / max(cosTheta, 0.1) * (1.0 - cartoon) * (1.0 - film);

    // Surf where the waves run up the shore: a strip of froth at the water's edge and lines of foam rolling in onto
    // it, reaching further out the harder the wind blows, and furthest onto the shore it blows at. Its colour is
    // the place's.
    float onshore = dot(toShore, toShore) > 0.5 ? max(dot(normalize(water.wind.xz + vec2(1e-4)), toShore), 0.0) : 0.0;
    float reach = 0.2 + (0.4 + 0.8 * onshore) * water.wind.y;
    if (depthBelow < reach) {
        // Cut hard, as foam lies on water in threads and holes rather than a haze: that, not its brightness, is what
        // tells it from the sky the shallows mirror.
        float froth = sfeFbm(drifted * 2.5, 3);
        float lace = smoothstep(0.42, 0.47, froth);
        float width = 0.08 + 0.12 * water.wind.y;
        float edge = 1.0 - smoothstep(0.85 * width, width, depthBelow - width * (froth - 0.5));
        // Lines of equal depth moving into the shallows, a wave every couple of seconds, broken up and dying out as
        // they come in from the edge of the surf.
        float phase = fract(depthBelow / reach * 2.0 + seconds * (0.3 + 0.3 * water.wind.y));
        float band = smoothstep(0.6, 0.66, phase) * (1.0 - smoothstep(0.84, 0.9, phase));
        band *= lace * (1.0 - smoothstep(0.5 * reach, reach, depthBelow));
        float foam = max(edge * mix(1.0, lace, 0.5), band);
        // A heap of bubbles takes the light in from any side, so a low sun lights it more than a flat film.
        vec3 foamLight = (sunHere * (0.5 + 0.5 * max(toSun.y, 0.0)) + skyLight) / PI;
        color = mix(color, place.foamColour * foamLight, foam * 0.95);
    }

    // The water line itself: fade into the wet ground over the last few centimetres instead of a hard edge.
    float waterLine = smoothstep(0.0, 0.04, depthBelow);
    if (waterLine < 1.0) {
        color = mix(texture(sceneColor, uv).rgb, color, waterLine);
    }
    FragColor = vec4(color, 1.0);
}
