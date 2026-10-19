#ifndef SFE_GBUFFER_SURFACE_GLSL
#define SFE_GBUFFER_SURFACE_GLSL
// The G-buffer surface of a mesh with the default material shader. g_buffer.fs defines SFE_GBUFFER_CUTOUT and cuts
// the material's alpha out; g_buffer_solid.fs, for materials with nothing to cut, leaves the discard out entirely:
// its mere presence stops the depth test from rejecting hidden fragments before they are shaded.
#include "sfe.glsl"
#include "sfe_material.glsl"
layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec2 TexCoords;
layout(location = 2) in mat3 TBN;
layout(location = 5) in vec2 TexCoords1;
layout(location = 6) flat in uint vMaterial;
layout(location = 7) in vec4 vClip;
layout(location = 8) in vec4 vPrevClip;
layout(location = 9) in vec3 vSurface;
layout(location = 12) flat in float vPiece;
layout(location = 14) in vec3 vRest;

layout(location = 0) out vec4 gMaterial;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;
layout(location = 3) out vec4 gVelocity;
layout(location = 4) out vec4 gEmissive;

layout(SFE_SET(1, 1)) uniform sampler2D texture_diffuse1;
layout(SFE_SET(1, 2)) uniform sampler2D normalMap;
layout(SFE_SET(1, 3)) uniform sampler2D texture_specular1;
layout(SFE_SET(1, 4)) uniform sampler2D texture_roughness;
layout(SFE_SET(1, 5)) uniform sampler2D texture_metallic;
layout(SFE_SET(1, 6)) uniform sampler2D texture_subsurface;
layout(SFE_SET(1, 7)) uniform sampler2D triplanarAlbedo;
layout(SFE_SET(1, 8)) uniform sampler2D triplanarNormals;
layout(SFE_SET(1, 9)) uniform sampler2D triplanarAlbedo2;
layout(SFE_SET(1, 10)) uniform sampler2D triplanarNormals2;
layout(SFE_SET(1, 11)) uniform sampler2D texture_occlusion;
layout(SFE_SET(1, 12)) uniform sampler2D texture_emissive;
// The vehicles' mud maps (MudMapsFrame), a material's by its mudMap.
layout(SFE_SET(0, 36)) uniform sampler2DArray sfeMudMaps;

#define pc sfeMaterials[vMaterial]

bool inRect(vec2 uv, vec4 rect) {
    return all(greaterThanEqual(uv, rect.xy)) && all(lessThanEqual(uv, rect.zw));
}

// A texture projected along the model's axes, weighted towards the axis the surface faces, its normals blended as
// whiteout: the albedo and the world normal.
// `rotated` swaps each projection's u and v, and the normal map's x and y with them.
void sfeTriplanar(sampler2D albedo, sampler2D normals, float repeat, bool rotated, inout vec3 colour, inout vec3 Nw) {
    vec3 n = normalize(TBN[2]);
    vec3 w = pow(abs(n), vec3(4.0));
    w /= w.x + w.y + w.z;
    vec3 side = vec3(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0, n.z < 0.0 ? -1.0 : 1.0);
    vec3 p = vSurface * repeat;
    vec2 uvX = vec2(p.z * side.x, p.y);
    vec2 uvY = vec2(p.x * side.y, p.z);
    vec2 uvZ = vec2(-p.x * side.z, p.y);
    if (rotated) {
        uvX = uvX.yx;
        uvY = uvY.yx;
        uvZ = uvZ.yx;
    }
    colour = texture(albedo, uvX).rgb * w.x + texture(albedo, uvY).rgb * w.y + texture(albedo, uvZ).rgb * w.z;
    vec3 tX = texture(normals, uvX).xyz * 2.0 - 1.0;
    vec3 tY = texture(normals, uvY).xyz * 2.0 - 1.0;
    vec3 tZ = texture(normals, uvZ).xyz * 2.0 - 1.0;
    if (rotated) {
        tX.xy = tX.yx;
        tY.xy = tY.yx;
        tZ.xy = tZ.yx;
    }
    tX.x *= side.x;
    tY.x *= side.y;
    tZ.x *= -side.z;
    tX = vec3(tX.xy + n.zy, abs(tX.z) * n.x);
    tY = vec3(tY.xy + n.xz, abs(tY.z) * n.y);
    tZ = vec3(tZ.xy + n.xy, abs(tZ.z) * n.z);
    Nw = normalize(tX.zyx * w.x + tY.xzy * w.y + tZ.xyz * w.z);
}

// Mud thrown up off the ground by the model's wheels (MaterialComponent::mudWheels), caked on it: on a wheel's tyre
// round all of it from its tread inward, kSfeMudInward of its radius in once the tread is wholly caked (mudTreads); on
// the rest where the drops it threw struck it, as its mud map has it (mudMap), in patches and drops where little
// struck. The surface's own crevices (its occlusion map: a tyre's tread, the seams) fill first. All of it laid on the
// model as it rests (vRest), so it stays where it struck as the model moves and its parts turn: a wheel's mud goes
// round with it, a door's swings with it.
const float kSfeMudInward = 0.75;
const float kSfeMudGrain = 6.0;                      // splashes a metre, and drops kSfeMudGrain * 4
const float kSfeMudClods = 10.0;                     // clods a metre in the cake, and lumps on them kSfeMudLumps
const float kSfeMudLumps = 30.0;
const float kSfeMudDepth = 0.015;                    // metres the cake stands out at its thickest, for its relief
const vec3 kSfeMud = vec3(0.085, 0.064, 0.042);     // wet mud's colour, linear: a brown, lighter than a tyre
const vec3 kSfeMudDry = vec3(0.16, 0.13, 0.095);     // dried out where it is thin, lighter and greyer
// A mud map's cells: as MudMapsFrame::kCell and kCells.
const float kSfeMudCell = 0.05;
const vec3 kSfeMudCells = vec3(56.0, 60.0, 128.0);

// Noise over the model's rest space at `scale` a metre, three planes of it taken together: 0..1 round a half.
float sfeMudNoise(float scale, int octaves) {
    vec3 q = vRest * scale;
    float n = (sfeFbm(q.xy, octaves) + sfeFbm(q.yz + 17.0, octaves) + sfeFbm(q.zx + 31.0, octaves)) / 3.0;
    // Spread out again from the middle that taking three of them brings it to.
    return clamp((n - 0.5) * 2.5 + 0.5, 0.0, 1.0);
}

// How caked the model is here by the drops that struck it, as its mud map has it (mudMap), 0 none to 1 wholly: the
// map's cells taken smoothly, across and down within a layer by the sampler, from layer to layer here.
float sfeMudStruck() {
    vec3 c = (vRest - pc.mudMap.xyz) / kSfeMudCell;
    if (pc.mudMap.w <= 0.0 || any(lessThan(c, vec3(0.0))) || any(greaterThan(c, kSfeMudCells))) {
        return 0.0;
    }
    float height = clamp(c.y - 0.5, 0.0, kSfeMudCells.y - 1.0);
    float below = floor(height);
    vec2 uv = c.xz / kSfeMudCells.xz;
    float first = pc.mudMap.w - 1.0;
    float low = texture(sfeMudMaps, vec3(uv, first + below)).r;
    float high = texture(sfeMudMaps, vec3(uv, first + min(below + 1.0, kSfeMudCells.y - 1.0))).r;
    return mix(low, high, height - below);
}

// How much mud is caked here, 0 none to 1 wholly: on a tyre (and what turns with it, within its radius round its
// axle, the model's x, and a little more than half a radius aside) by how caked its tread is, from the tread in; on
// the rest as the drops that struck it caked it.
float sfeMudThrown() {
    for (int w = 0; w < 4; ++w) {
        vec4 wheel = pc.mudWheels[w];
        if (wheel.w <= 0.0) {
            continue;
        }
        vec3 off = vRest - wheel.xyz;
        float across = length(off.yz) / wheel.w;
        if (across <= 1.02 && abs(off.x) < 0.6 * wheel.w) {
            float tread = pc.mudTreads[w];
            return tread > 0.0 ? smoothstep(-0.12, 0.12, across - 1.0 + kSfeMudInward * tread) : 0.0;
        }
    }
    return sfeMudStruck();
}

// The mud here: x how much of the surface it hides (0..1, a sharp edge), y how thick it is caked (0 at its edge, 1 a
// full cake), z how high it stands, metres (its lumps). `crevice` how sunk the surface is there, 0..1. Where little was
// thrown, only the highest of its splashes and drops show.
vec3 sfeMud(float crevice) {
    float thrown = sfeMudThrown();
    if (thrown <= 0.0) {
        return vec3(0.0);
    }
    float splash = 0.65 * sfeMudNoise(kSfeMudGrain, 3) + 0.35 * sfeMudNoise(kSfeMudGrain * 4.0, 2);
    float caked = splash + 1.3 * thrown - 0.75 + 0.3 * crevice * min(4.0 * thrown, 1.0);
    float cover = smoothstep(0.47, 0.53, caked);
    float thick = smoothstep(0.5, 0.9, caked);
    float lumps = 0.6 * sfeMudNoise(kSfeMudClods, 2) + 0.4 * sfeMudNoise(kSfeMudLumps, 2);
    return vec3(cover, thick, cover * kSfeMudDepth * (0.3 + 0.7 * thick) * (0.4 + 1.2 * lumps));
}

// `N` tilted up the slope of a height `h` (metres) over the surface: bump mapping from the screen's derivatives
// (Mikkelsen's surface gradient), no tangents needed.
vec3 sfeBumped(vec3 N, float h) {
    vec3 dpx = dFdx(FragPos);
    vec3 dpy = dFdy(FragPos);
    vec3 r1 = cross(dpy, N);
    vec3 r2 = cross(N, dpx);
    float det = dot(dpx, r1);
    vec3 grad = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);
    return normalize(abs(det) * N - grad);
}

vec2 uvOf(uint bit) {
    return ((uint(pc.uvSel + 0.5) & bit) != 0u) ? TexCoords1 : TexCoords;
}

void main() {
    if (sfeNearFaded(gl_FragCoord)) {
        discard;
    }
#ifdef SFE_GBUFFER_DEPTH
    // A game's own depth for the surface: a shader that defines SFE_GBUFFER_DEPTH declares sfeSurfaceDepth before
    // including this file and defines it after. It returns the depth to keep for the pixel -- for a surface its vertex
    // shader moved, worked out exactly per pixel rather than between moved vertices.
    gl_FragDepth = sfeSurfaceDepth();
#endif
    uint maps = uint(pc.mapMask + 0.5);
    vec2 uvDiff = uvOf(1u);
    vec2 uvNrm = uvOf(2u);
    vec2 uvSpec = uvOf(4u);
    vec2 uvRough = uvOf(8u);
    vec2 uvMetal = uvOf(16u);
    vec2 uvSss = uvOf(32u);
    vec2 uvAo = uvOf(64u);

    // Filtered, not level 0: reading the mask at the top level made the coverage of a distant character
    // depend on which single texel the jitter happened to land on. The level is the hardware's, so the
    // anisotropic filter still applies and the two backends pick it the same way.
    float diffLod = textureQueryLod(texture_diffuse1, uvDiff).y;
    // Read once: the mask, the albedo and the edge search below all start from it.
    vec4 diffuseSample = texture(texture_diffuse1, uvDiff);
    float mask = diffuseSample.a;
    if ((maps & 8u) != 0u && mask > 0.99) {
        mask = max(texture(texture_specular1, uvSpec).r, texture(texture_roughness, uvRough).g);
    }
    uint uvBits = uint(pc.uvSel + 0.5);
    if ((uvBits & 1u) == 0u && (uvBits & 6u) != 0u) {
        mask = min(mask, texture(texture_diffuse1, TexCoords1).a);
    }
#ifdef SFE_GBUFFER_STYLE_MASK
    // A game's own look over what is cut out: sfeStyleMask, declared before this file is included and defined after,
    // gets the texture and the mask as read, and returns the mask to cut by.
    mask = sfeStyleMask(texture_diffuse1, uvDiff, diffLod, mask);
#endif
#ifdef SFE_GBUFFER_CUTOUT
    float cutoutFade = sfeCutoutFade(texture_diffuse1, diffLod);
    if (pc.alphaCutoff > 1.0) {
        float cov = mix(clamp(pc.alphaCutoff - 1.0, 0.05, 1.0), 1.0, cutoutFade);
        mask = min(mask, cov);
        float d = sfeCutoutDither(texture_diffuse1, uvDiff, diffLod);
        if (mask < d) {
            discard;
        }
    } else if (pc.alphaCutoff > 0.0 && mask < pc.alphaCutoff * (1.0 - cutoutFade)) {
        discard;
    }
#endif
    vec4 diff = diffuseSample;
    if (pc.alphaCutoff > 0.0) {
        diff.rgb = sfeMaskedAlbedo(texture_diffuse1, uvDiff, diffuseSample);
    }

    gNormal.a = gl_FragCoord.z / gl_FragCoord.w;

    vec3 nSample = texture(normalMap, uvNrm).rgb;
    vec3 Nw;
    // 1 where the map says nothing: then only the geometry spreads the normal inside the pixel.
    float normalLen = 1.0;
    if (dot(nSample, nSample) < 0.02) {
        Nw = normalize(TBN[2]);
    } else {
        vec3 nts = nSample * 2.0 - 1.0;
        // Before normalising: the mip handed back an average of several texels, and how much shorter that
        // average is says how much the normals under this pixel disagree.
        normalLen = min(1.0, length(nts));
        Nw = normalize(TBN * nts);
    }
    if (pc.clearcoat > 0.5) {
        Nw = normalize(TBN[2]);
    } else if (pc.clearcoat > 0.01) {
        Nw = normalize(mix(Nw, normalize(TBN[2]), pc.clearcoat));
    }
    // The triplanar textures where the coordinates fall in their rectangles; one pixel reads one of them at most.
    bool triplanar = false;
    if (pc.triplanarRepeat > 0.0 && inRect(TexCoords, pc.triplanarUv)) {
        sfeTriplanar(triplanarAlbedo, triplanarNormals, pc.triplanarRepeat, (uint(pc.triplanarRotated + 0.5) & 1u) != 0u, diff.rgb, Nw);
        triplanar = true;
    } else if (pc.triplanarRepeat2 > 0.0 && inRect(TexCoords, pc.triplanarUv2)) {
        sfeTriplanar(triplanarAlbedo2, triplanarNormals2, pc.triplanarRepeat2, (uint(pc.triplanarRotated + 0.5) & 2u) != 0u, diff.rgb, Nw);
        triplanar = true;
    }

#ifdef SFE_GBUFFER_STYLE
    // A game's own look over the surface: a shader that defines SFE_GBUFFER_STYLE declares sfeStyleSurface before
    // including this file and defines it after. It gets the colour as read, the normal, and the coat written below with
    // its last channel (one of sfe.glsl's markers, kSfeGlint..., and what that channel says there: sfeGBufferMaterial),
    // and turns them into its look before anything below is worked out from them.
    float coat = pc.clearcoat;
    float coatRough = pc.clearcoatRough;
    sfeStyleSurface(texture_diffuse1, uvDiff, diffLod, diff.rgb, Nw, coat, coatRough);
#else
    const float coat = pc.clearcoat;
    const float coatRough = pc.clearcoatRough;
#endif
    vec3 linAlb = pow(diff.rgb, vec3(2.2)) * pc.tint;
    // Soaked below a line up the model (MaterialComponent::wetBelow): with water filling the weave, much darker and a
    // little deeper in colour.
    float soaked = 1.0 - smoothstep(pc.wetBelow - 0.03, pc.wetBelow + 0.03, vSurface.y);
    linAlb = mix(linAlb, pow(linAlb, vec3(1.15)) * 0.4, soaked);
    // Mud caked over it: wet and dark where it is thick, dried lighter at its thin edges and on its lumps' tops; standing
    // out in lumps over the surface, whose own relief (its normal map) it buries. Worked out over the whole material once
    // it is muddy at all, as the relief takes the neighbouring pixels' heights.
    vec3 mud = vec3(0.0);
    if (pc.mudMap.w > 0.0) {
        mud = sfeMud(1.0 - texture(texture_occlusion, uvOf(64u)).r);
        vec3 buried = normalize(mix(Nw, normalize(TBN[2]), mud.y));
        Nw = normalize(mix(Nw, sfeBumped(buried, mud.z), mud.x));
        // How high it stands over its edge, its tallest lumps about 1.6: darker down between them.
        float high = mud.z / kSfeMudDepth;
        float dry = clamp(0.45 * (1.0 - mud.y) + 0.3 * (high - 1.0), 0.0, 1.0);
        linAlb = mix(linAlb, mix(kSfeMud, kSfeMudDry, dry) * mix(0.7, 1.1, clamp(high, 0.0, 1.0)), mud.x);
    }
    float muddy = mud.x;
    // A plant going bare (SfeMaterial::bare): its bark greys with it, and a leaf past its point is gone (sfeBareKeeps).
    float dead = pc.alphaCutoff > 0.0 ? 0.0 : pc.bare;
#ifdef SFE_GBUFFER_CUTOUT
    if (pc.alphaCutoff > 0.0 && vPiece < pc.bare) {
        if (!sfeBareKeeps(linAlb)) {
            discard;
        }
        dead = 1.0;
    }
#endif
    linAlb = mix(linAlb, vec3(sfeLuma(linAlb)), dead * 0.7);
    const bool cutout = pc.alphaCutoff > 0.0;
    // Cut-out surfaces (leaves, grass cards) scatter nothing under the skin: their subsurface map is not even read.
    float sss = 0.0;
    if (cutout) {
        Nw = normalize(TBN[2]);
    }
    else {
        sss = texture(texture_subsurface, uvSss).r;
        if (sss < 0.001) {
            sss = texture(texture_subsurface, uvSss).a;
        }
        // The one place skin is guessed from colour; lighting and the SSS pass read the result from the normal's
        // length. Nothing that sways in the wind (bits 32 and 64) is skin, however brown its bark.
        if (sss < 0.05 && pc.clearcoat < 0.5 && (maps & 16u) != 0u && (maps & 96u) == 0u && !triplanar) {
            sss = max(sss, sfeSkinSss(linAlb, 0.0));
        }
    }
    gNormal.xyz = Nw * (1.0 - 0.5 * clamp(sss, 0.0, 1.0));

    gAlbedoSpec.rgb = linAlb;

    float roughness = pc.matRoughness;
    if ((maps & 1u) != 0u) {
        roughness = texture(texture_roughness, uvRough).g;
    } else if ((maps & 4u) != 0u) {
        roughness = 1.0 - texture(texture_specular1, uvSpec).r;
    }
    roughness = mix(roughness, max(roughness, 0.62), clamp(sss, 0.0, 1.0));
    if (pc.clearcoat > 0.5) {
        roughness = 0.48;
    }
    // Wet mud: a soft sheen, over the paint's coat; dull where it dried thin.
    roughness = mix(roughness, mix(0.6, 0.2, mud.y), muddy);
    float metallic = pc.matMetallic;
    if ((maps & 2u) != 0u) {
        metallic = texture(texture_metallic, uvMetal).b;
    }
    metallic *= 1.0 - clamp(pc.clearcoat, 0.0, 1.0);
    if ((maps & 8u) != 0u || cutout) {
        metallic = 0.0;
    }
    metallic *= 1.0 - muddy;

    float specF = pc.specFactor;
    if ((maps & 1u) != 0u && (maps & 4u) != 0u) {
        specF *= texture(texture_specular1, uvSpec).r;
    }
    if (cutout) {
        roughness = max(roughness, 0.78);
        specF = min(specF, 0.18);
    }

    gAlbedoSpec.a = clamp(sfeFilteredRoughness(Nw, roughness, normalLen), 0.04, 1.0);
    // Cut-out surfaces are leaves, needles and grass: they let the sun through.
    float lastChannel = cutout && pc.clearcoat < 0.01 ? kSfeTranslucentFoliage : clamp(coatRough, 0.04, 1.0);
    specF = mix(specF, 0.5, muddy);
    gMaterial = sfeGBufferMaterial(clamp(metallic, 0.0, 1.0), clamp(specF, 0.0, 1.0), clamp(coat * (1.0 - muddy), 0.0, 1.0), lastChannel);
    gVelocity = sfeGBufferVelocity(vClip, vPrevClip, 1.0 - texture(texture_occlusion, uvAo).r);
    gEmissive = sfeGBufferEmissive(vec3(pc.emissiveR, pc.emissiveG, pc.emissiveB), texture_emissive, uvOf(128u));
}

#endif
