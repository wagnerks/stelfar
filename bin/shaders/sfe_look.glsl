#ifndef SFE_LOOK_GLSL
#define SFE_LOOK_GLSL
// What stays put on a model as it moves, laid over its rest space (vRest), and a piece of litter's look worn, burnt,
// silted and dirtied (sfeLooked): for the G-buffer surface (sfe_gbuffer_surface.glsl) and for glass (glass.fs). Include after
// sfe.glsl and after FragPos, TBN and vRest are declared.
// A piece of litter's look (sfeLooked), all four words 0 for anything else.
layout(location = 13) flat in uvec4 vLook;
// The rest of it: the lake's silt and a fire's char on it (LitterLook::gpu's words 4 to 7), all 0 for anything else.
layout(location = 16) flat in uvec4 vLookMore;
// The model's normal at rest, as vRest its place: which of its sides the dirt is on.
layout(location = 15) in vec3 vRestNormal;

// Noise over the model's rest space at `scale` a metre, moved by `offset`: 0..1 round a half. Three planes of it, each
// taken as much as the surface lies in it (vRestNormal), so a flat face shows its own plane's and not a plaid of the
// other two's stripes.
float sfeRestNoise(vec3 offset, float scale, int octaves) {
    vec3 q = vRest * scale + offset;
    vec3 w = pow(abs(vRestNormal) + 1e-3, vec3(4.0));
    w /= w.x + w.y + w.z;
    float n = w.x * sfeFbm(q.yz + 17.0, octaves) + w.y * sfeFbm(q.zx + 31.0, octaves) + w.z * sfeFbm(q.xy, octaves);
    // Spread out again from the middle that blending planes brings it to, as far as it does here.
    return clamp((n - 0.5) * 1.45 / length(w) + 0.5, 0.0, 1.0);
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

// A piece of litter's look (vLook, the game's LitterLook::gpu): worn by its time lying about and dirtied by the ground
// it touched. x a seed of its own, never 0; y the dirt on its rest sides +x, -x, +y, -y, a byte each, 0 clean to 255
// caked; z the dirt on +z and -z, then how worn it is (0 new to 255), then its stuff (low four bits, kSfeStuff...) and
// how wet the dirt on it is (high four); w the dirt's colour, sRGB, a byte a channel. Then vLookMore: x the silt on it
// (0 none to 255 a whole coat), its waterline (a byte, 128 the model's origin, kSfeLevelSteps a metre along the way up)
// and the way up as it lay in the water (two bytes, sfeLookWay); y how burnt it is (0 whole to 255 charred through)
// and the way to the fire (two bytes, sfeLookWay).
const uint kSfeStuffPlastic = 0u;
const uint kSfeStuffMetal = 1u;
const uint kSfeStuffPaper = 2u;
const uint kSfeStuffWood = 3u;
const uint kSfeStuffCloth = 4u;
const uint kSfeStuffGlass = 5u;
const float kSfeStainGrain = 9.0;   // stains a metre
const float kSfeChipGrain = 120.0;  // chips a metre along an edge
const float kSfeRustGrain = 18.0;   // patches of rust a metre, their flakes kSfeRustGrain * 4
const float kSfeMouldGrain = 50.0;  // spots of mould a metre
const float kSfeDirtGrain = 14.0;   // clods of dirt a metre, and its specks kSfeDirtGrain * 4
const float kSfeDirtDepth = 0.002;  // metres the dirt stands out at its thickest, for its relief
const float kSfeWearGain = 1.8;     // how worn a piece looks for how worn it is (vLook), up to wholly
const vec3 kSfeGrime = vec3(0.42, 0.38, 0.32);  // what grime multiplies the colour by where it is thickest
const vec3 kSfeSteel = vec3(0.56, 0.57, 0.58);  // bare steel, linear
const vec3 kSfeRust = vec3(0.2, 0.065, 0.02);
const vec3 kSfeRustDark = vec3(0.08, 0.032, 0.014);
const vec3 kSfeMould = vec3(0.03, 0.04, 0.026);
// Burnt: how hot the fire made a surface (sfeLookHeat) is how burnt the piece is, give or take what faces the fire.
const float kSfeBurnGrain = 7.0;      // the char's ragged fronts a metre
const float kSfeBurnFacing = 0.25;    // how much hotter a face turned to the fire is, and cooler one turned away
const float kSfeBurnRagged = 0.5;     // how far the fronts wander, in heat
const float kSfeScorchAt = 0.08;      // heat it starts to brown and soot at, browned through at kSfeSingeAt
const float kSfeSingeAt = 0.3;        // heat it is singed dark brown at, and paint comes off metal
const float kSfeCharAt = 0.45;        // heat it is charred black at
const float kSfeOxideAt = 0.7;        // heat bare metal is dark with oxide at, past its temper colours
const float kSfeHoleAt = 0.9;         // heat paper, card and cloth are burnt through at
const float kSfeSootGrain = 22.0;     // smudges of soot a metre
const float kSfeSoot = 0.05;          // what soot multiplies the colour by where it lies thickest
const float kSfeCharGrain = 55.0;     // cells of charred wood's cracked crust a metre
const float kSfeCharCrack = 0.07;     // how wide the cracks between them are, of a cell
const float kSfeCharDepth = 0.0015;   // metres the cells stand out of their cracks
const float kSfeBlisterGrain = 90.0;  // blisters on burnt plastic a metre
const float kSfeBlisterDepth = 0.0008;
const vec3 kSfeBrowned = vec3(0.42, 0.32, 0.22);  // what scorching multiplies the colour by
const vec3 kSfeSinged = vec3(0.06, 0.03, 0.012);  // singed dark brown, linear
const vec3 kSfeChar = vec3(0.028, 0.025, 0.023);  // char, and black soot
const vec3 kSfeOxide = vec3(0.05, 0.045, 0.04);   // fire scale on steel
// Steel's temper colours, as hot as it went from straw to blue, metal's reflectance.
const vec3 kSfeStraw = vec3(0.6, 0.47, 0.24);
const vec3 kSfeBronze = vec3(0.45, 0.24, 0.1);
const vec3 kSfePurple = vec3(0.2, 0.1, 0.24);
const vec3 kSfeBlue = vec3(0.09, 0.14, 0.32);
// Silt: below the waterline, a film over all of it and thick patches more in hollows and on faces turned up.
const float kSfeLevelSteps = 64.0;    // a waterline's steps a metre (LitterLook kLevelMetres)
const float kSfeSiltGrain = 11.0;     // patches of silt a metre
const float kSfeSiltLineGrain = 6.0;  // waves of the waterline a metre
const float kSfeSiltWaver = 0.003;    // metres the waterline wavers up and down
const float kSfeSiltSoft = 0.004;     // metres the waterline fades over
const float kSfeScumBand = 0.005;     // metres of scum just under the waterline
const float kSfeSiltFilm = 0.85;      // how much of the surface a whole coat's thin film hides between its patches
const float kSfeSiltCrackGrain = 70.0; // cells a metre thick dry silt cracks into
const float kSfeSiltCrack = 0.06;     // how wide their cracks are, of a cell
const float kSfeSiltDepth = 0.0012;   // metres the silt stands out at its thickest
const vec3 kSfeSiltWet = vec3(0.065, 0.062, 0.04);  // wet silt, dark olive grey, linear
const vec3 kSfeSiltDry = vec3(0.36, 0.33, 0.25);    // dry silt, pale grey beige
const vec3 kSfeScumWet = vec3(0.028, 0.03, 0.017);
const vec3 kSfeScumDry = vec3(0.15, 0.13, 0.095);

struct SfeLooked {
    vec3 albedo;
    vec3 normal;
    float rough;
    float metal;
};

float sfeLookByte(uint word, uint i) {
    return float((word >> (8u * i)) & 255u) / 255.0;
}

// A way in the piece's rest space from the two low bytes of `bytes`, folded onto an octahedron (LitterLook::wayOf).
vec3 sfeLookWay(uint bytes) {
    float x = float(bytes & 255u) / 255.0 * 2.0 - 1.0;
    float y = float((bytes >> 8u) & 255u) / 255.0 * 2.0 - 1.0;
    float z = 1.0 - abs(x) - abs(y);
    if (z < 0.0) {
        float fx = (1.0 - abs(y)) * (x >= 0.0 ? 1.0 : -1.0);
        y = (1.0 - abs(x)) * (y >= 0.0 ? 1.0 : -1.0);
        x = fx;
    }
    return normalize(vec3(x, y, z));
}

// How worn the piece looks, 0..1.
float sfeLookWear() {
    return min(sfeLookByte(vLook.z, 2u) * kSfeWearGain, 1.0);
}

// The piece's own random number `k`, 0..1.
float sfeLookRandom(uint k) {
    return float(sfeCellHash(ivec2(int(vLook.x & 0x7fffffffu), int(k))) >> 8u) / 16777216.0;
}

// Rest-space noise (sfeRestNoise) evened out towards its middle as a pixel spans more of its grain, `px` rest metres a
// pixel, so far off it does not flicker from frame to frame.
float sfeLookNoise(vec3 offset, float scale, int octaves, float px) {
    return mix(sfeRestNoise(offset, scale, octaves), 0.5, clamp(px * scale * 2.0 - 0.5, 0.0, 1.0));
}

// 0 below `at` to 1 above, the edge sharp close up and widening as a pixel spans more of a grain `scale` a metre.
float sfeLookEdge(float at, float n, float scale, float px) {
    float w = max(0.02, px * scale);
    return smoothstep(at - w, at + w, n);
}

// How far `p` is from the border between its nearest two cells of a jittered lattice, of a cell: 0 on it.
float sfeCellBorder(vec2 p) {
    vec2 cell = floor(p);
    vec2 f = p - cell;
    float d1 = 8.0;
    float d2 = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            uint h = sfeCellHash(ivec2(cell) + ivec2(x, y));
            vec2 r = vec2(x, y) + 0.1 + 0.8 * vec2(float(h & 0xffffu), float(h >> 16u)) / 65535.0 - f;
            float d = dot(r, r);
            d2 = d < d1 ? d1 : min(d2, d);
            d1 = min(d1, d);
        }
    }
    return 0.5 * (sqrt(d2) - sqrt(d1));
}

// A crust crazed into cells over the rest space, `scale` a metre, as char or drying mud cracks: 0 on a cell to 1 in the
// crack round it, the cracks `width` of a cell wide. Three planes of it as sfeRestNoise takes them, and evened out to
// how much of a pixel the cracks cover as a pixel spans more of a cell, `px` rest metres a pixel.
float sfeLookCells(vec3 offset, float scale, float width, float px) {
    vec3 q = vRest * scale + offset;
    vec3 w = pow(abs(vRestNormal) + 1e-3, vec3(4.0));
    w /= w.x + w.y + w.z;
    float d = w.x * sfeCellBorder(q.yz + 17.0) + w.y * sfeCellBorder(q.zx + 31.0) + w.z * sfeCellBorder(q.xy);
    float aa = px * scale;
    float crack = 1.0 - smoothstep(width * 0.5, width * 0.5 + aa + 0.01, d);
    return mix(crack, width * 2.0, clamp(aa * 4.0 - 0.5, 0.0, 1.0));
}

// How sharply the surface bends here, per metre: over 0 bulging out (an edge, a rim), under 0 hollowed (a seam, an inner
// corner). How its smooth normal turns across the pixel against how far the pixel spans, so a bevelled edge shows it
// and a hard one, its normals split, does not.
float sfeCurvature() {
    vec3 n = normalize(TBN[2]);
    vec3 dpx = dFdx(FragPos);
    vec3 dpy = dFdy(FragPos);
    float x = dot(dFdx(n), dpx) / max(dot(dpx, dpx), 1e-12);
    float y = dot(dFdy(n), dpy) / max(dot(dpy, dpy), 1e-12);
    return 0.5 * (x + y);
}

// How much of the ground's dirt is here, on the sides of the piece that touched it (vLook), 0..1. A side's dirt covers
// what faces that way and creeps up round it to a little past the middle, as mud splashed up a thing lying in it: the
// most of any side's here.
const float kSfeDirtUp = 0.35; // how far past the middle it creeps, of the way to facing the other way
float sfeLookSide() {
    vec3 nr = normalize(vRestNormal);
    vec3 up = smoothstep(vec3(-kSfeDirtUp), vec3(0.75), nr);
    vec3 down = smoothstep(vec3(-kSfeDirtUp), vec3(0.75), -nr);
    vec3 byUp = up * vec3(sfeLookByte(vLook.y, 0u), sfeLookByte(vLook.y, 2u), sfeLookByte(vLook.z, 0u));
    vec3 byDown = down * vec3(sfeLookByte(vLook.y, 1u), sfeLookByte(vLook.y, 3u), sfeLookByte(vLook.z, 1u));
    return max(max(max(byUp.x, byUp.y), byUp.z), max(max(byDown.x, byDown.y), byDown.z));
}

// The ground's dirt here, `onSide` of it (sfeLookSide): x how much of the surface it hides, y how thick it lies (0 at its
// edge, 1 a full coat), in clods and specks and thicker in the hollows (`hollow`, 0..1); `soil` its colour, linear,
// darker while wet. `off` the piece's own place in the noise, `px` rest metres a pixel.
vec2 sfeLookDirt(vec3 off, float px, float hollow, float onSide, out vec3 soil) {
    float clods = 0.65 * sfeLookNoise(off + 51.0, kSfeDirtGrain, 3, px) + 0.35 * sfeLookNoise(off + 53.0, kSfeDirtGrain * 4.0, 2, px);
    float caked = clods + 1.4 * onSide - 0.7 + 0.35 * hollow * min(4.0 * onSide, 1.0);
    float wet = float(vLook.z >> 28u) / 15.0;
    soil = pow(vec3(sfeLookByte(vLook.w, 0u), sfeLookByte(vLook.w, 1u), sfeLookByte(vLook.w, 2u)), vec3(2.2));
    soil *= mix(0.8, 1.15, sfeLookNoise(off + 57.0, kSfeDirtGrain * 8.0, 1, px)) * mix(1.0, 0.3, wet);
    return vec2(onSide > 0.0 ? sfeLookEdge(0.5, caked, kSfeDirtGrain * 4.0, px) : 0.0, smoothstep(0.5, 1.0, caked));
}

// How hot the fire made the surface here, `burnt` how burnt the piece is (vLookMore.y): as much as that, more on the
// faces turned to the fire and less on those turned away, more on its edges (`edge`, 0..1), which heat through first,
// and its fronts ragged. 0 untouched; kSfeScorchAt browned, kSfeCharAt charred, kSfeHoleAt burnt through. Never more
// than a few times `burnt`, so a piece barely touched is barely browned on the side the fire was.
float sfeLookHeat(float burnt, vec3 off, float px, float edge) {
    vec3 from = sfeLookWay(vLookMore.y >> 8u);
    float facing = dot(normalize(vRestNormal), from);
    float ragged = sfeLookNoise(off + 61.0, kSfeBurnGrain, 3, px) - 0.5;
    return min(burnt * (1.0 + 0.2 * edge) + kSfeBurnFacing * facing + kSfeBurnRagged * ragged, burnt * 3.0);
}

// How much soot lies here, 0..1, `heat` sfeLookHeat's: as soon as the fire browns the surface, thickening and thinning
// smoothly in smudges as smoke lays it, never with an edge of its own, more in its hollows, and burnt off again where
// the fire went hottest.
float sfeLookSoot(float heat, vec3 off, float px, float hollow) {
    float smudge = sfeLookNoise(off + 67.0, kSfeSootGrain, 3, px);
    float soot = smoothstep(0.0, 1.0, smoothstep(-0.2, kSfeSingeAt, heat + 0.15 * hollow) + 0.6 * (smudge - 0.5));
    return clamp(soot * (1.0 - 0.7 * smoothstep(kSfeOxideAt, kSfeHoleAt + 0.2, heat)), 0.0, 1.0);
}

// The lake's silt here (vLookMore.x), only below the waterline the piece lay at: x how much of the surface it hides, a
// thin film the more silt there is and thick patches over it, more in the hollows (`hollow`, 0..1) and on faces turned
// up in the water, where it settles; y how thick it lies (0 a film, 1 a full coat), down to nothing in its cracks. The
// line wavers as the water lapped, a band of darker scum just under it; `silt` its colour, linear, dark olive grey and
// wet or pale grey beige and dry (vLook's wet), and cracked where it lies thick and dry; `below` 1 below the line, 0 above.
vec2 sfeLookSilt(vec3 off, float px, float hollow, out vec3 silt, out float below) {
    float amount = sfeLookByte(vLookMore.x, 0u);
    float level = (float((vLookMore.x >> 8u) & 255u) - 128.0) / kSfeLevelSteps;
    vec3 up = sfeLookWay(vLookMore.x >> 16u);
    float wet = float(vLook.z >> 28u) / 15.0;
    // How far under the waterline, metres.
    float under = level - dot(vRest, up) + kSfeSiltWaver * (sfeLookNoise(off + 71.0, kSfeSiltLineGrain, 2, px) - 0.5) * 2.0;
    float soft = max(px, kSfeSiltSoft);
    below = smoothstep(-soft, soft, under);
    float scum = below * (1.0 - smoothstep(kSfeScumBand * 0.4, kSfeScumBand + soft, under)) * min(amount * 3.0, 1.0);
    float settles = dot(normalize(vRestNormal), up);
    float clumps = sfeLookNoise(off + 73.0, kSfeSiltGrain, 3, px);
    float lies = amount * 0.85 + (0.35 * hollow + 0.15 * settles - 0.05) * min(amount * 4.0, 1.0);
    float thick = below * sfeLookEdge(clumps, lies, kSfeSiltGrain, px);
    float depth = thick * smoothstep(0.0, 0.25, lies - clumps);
    float crack = sfeLookCells(off + 79.0, kSfeSiltCrackGrain, kSfeSiltCrack, px) * smoothstep(0.3, 0.8, depth) * (1.0 - wet);
    silt = mix(kSfeSiltDry, kSfeSiltWet, wet) * mix(0.85, 1.15, sfeLookNoise(off + 77.0, kSfeSiltGrain * 6.0, 2, px));
    silt = mix(silt, mix(kSfeScumDry, kSfeScumWet, wet), scum) * (1.0 - 0.55 * crack);
    float cover = max(max(below * min(amount * 8.0, 1.0) * kSfeSiltFilm, thick), scum * 0.85);
    return vec2(cover, depth * (1.0 - crack));
}

// The piece worn and dirtied (vLook): `albedo` linear, `N` its world normal, `rough` and `metal` as its maps give them,
// `crevice` how sunk the surface is there (its occlusion map), 0..1. All of it laid on the piece as it rests (vRest) and
// from its own seed, so it stays put as the piece moves and is the same on every machine.
//  - Faded by sun and weather, each piece by its own amount: paint and plastic bleached, paper yellowed, wood silvered.
//  - Grimed in its crevices and hollows and stained in blots, a stuff that soaks with a darker tide line round each.
//  - Chipped and scuffed along its edges, the more the more worn: paint down to bare steel, plastic and glass scuffed
//    white and dull, paper, card, wood and cloth rubbed through to lighter fibres.
//  - Metal rusted where it is bare or chipped and out of its crevices; paper, wood and cloth spotted with mould where
//    damp, in its hollows and on the sides it lay on the ground with.
//  - Burnt (vLookMore.y), the more on the faces it turned to the fire, in ragged fronts: browned and sooted, then
//    charred; wood crazed into cells, paper, card and cloth singed and burnt through to holes (a discard), metal
//    stripped of its paint to tempered and then scaled steel, plastic blackened, blistered and glossy where molten.
//  - Silted below the waterline it lay at in the lake (vLookMore.x): a film, a darker scum just under the line, thick
//    patches in the hollows and on what faced up, dark and glossy while wet, pale, matte and cracked dry.
//  - Over all of it the ground's dirt on the sides that touched it, in clods and specks, thicker in the crevices, dark
//    and glossy while wet.
SfeLooked sfeLooked(vec3 albedo, vec3 N, float rough, float metal, float crevice) {
    SfeLooked o = SfeLooked(albedo, N, rough, metal);
    uint stuff = (vLook.z >> 24u) & 15u;
    float wear = sfeLookWear();
    float wet = float(vLook.z >> 28u) / 15.0;
    vec3 off = vec3(sfeLookRandom(2u), sfeLookRandom(3u), sfeLookRandom(4u)) * 512.0;
    float px = max(length(fwidth(vRest)), 1e-6);
    bool soaks = stuff == kSfeStuffPaper || stuff == kSfeStuffWood || stuff == kSfeStuffCloth;
    float bare = smoothstep(0.4, 0.6, metal);
    float curve = sfeCurvature();
    float hollow = max(crevice, smoothstep(-40.0, -200.0, curve));
    float luma = sfeLuma(albedo);
    float onSide = sfeLookSide();

    // Faded.
    float fade = clamp(wear * 1.4 * (0.6 + sfeLookNoise(off, 3.0, 2, px)), 0.0, 1.0);
    vec3 faded = stuff == kSfeStuffPaper ? luma * vec3(1.1, 0.98, 0.7) + 0.02
        : stuff == kSfeStuffWood ? vec3(luma * 0.8 + 0.04)
        : mix(albedo, vec3(luma), 0.7) * 1.2 + 0.01;
    o.albedo = mix(o.albedo, faded, fade * 0.9 * (1.0 - bare));

    // Grimed and stained.
    float blot = sfeLookNoise(off + 11.0, kSfeStainGrain, 3, px);
    float at = 0.68 - 0.3 * wear;
    float stain = sfeLookEdge(at, blot, kSfeStainGrain, px);
    float tide = soaks ? stain - sfeLookEdge(at + 0.04, blot, kSfeStainGrain, px) : 0.0;
    float grime = clamp(wear * (1.4 * hollow + 0.15 + stain * (soaks ? 0.8 : 0.5) + tide * 1.0), 0.0, 1.0);
    o.albedo *= mix(vec3(1.0), kSfeGrime, grime);
    o.rough = mix(o.rough, 0.9, grime * 0.5);

    // Chipped and scuffed.
    float edge = smoothstep(50.0, 250.0, curve);
    float chipping = edge * (0.4 + 0.8 * wear) + (sfeLookNoise(off + 23.0, kSfeChipGrain, 2, px) - 0.5) * 0.7 + 0.35 * wear - 0.12;
    float chip = wear > 0.0 ? sfeLookEdge(0.5, chipping, kSfeChipGrain, px) : 0.0;
    luma = sfeLuma(o.albedo);
    if (stuff == kSfeStuffMetal) {
        o.albedo = mix(o.albedo, kSfeSteel, chip * (1.0 - bare));
        o.metal = mix(o.metal, 1.0, chip);
        o.rough = mix(o.rough, 0.3, chip);
    } else if (soaks) {
        o.albedo = mix(o.albedo, vec3(luma * 1.5) + vec3(0.06, 0.05, 0.035), chip * 0.7);
        o.rough = mix(o.rough, 1.0, chip);
    } else {
        o.albedo = mix(o.albedo, vec3(luma * 0.7 + 0.2), chip * 0.6);
        o.rough = mix(o.rough, 0.85, chip);
    }

    // Rusted, or mouldy.
    if (stuff == kSfeStuffMetal) {
        float exposed = max(max(bare, chip), hollow);
        float rusting = sfeLookNoise(off + 37.0, kSfeRustGrain, 3, px) * 0.8 + wear * (0.3 + 0.5 * hollow) + 0.4 * chip - 0.42;
        float rust = wear > 0.0 ? sfeLookEdge(0.5, rusting, kSfeRustGrain, px) * exposed : 0.0;
        vec3 rustColour = mix(kSfeRust, kSfeRustDark, sfeLookNoise(off + 41.0, kSfeRustGrain * 4.0, 2, px));
        o.albedo = mix(o.albedo, rustColour, rust);
        o.metal = mix(o.metal, 0.0, rust);
        o.rough = mix(o.rough, 0.85, rust);
    } else if (soaks) {
        float spotting = sfeLookNoise(off + 43.0, kSfeMouldGrain, 2, px) + 0.3 * hollow + 0.3 * onSide + 0.1 * wear;
        float mould = sfeLookEdge(0.8, spotting, kSfeMouldGrain, px) * min(wear * 1.5, 1.0);
        o.albedo = mix(o.albedo, kSfeMould, mould * 0.8);
    }

    // Burnt.
    float burnt = sfeLookByte(vLookMore.y, 0u);
    bool holed = false;
    if (burnt > 0.0) {
        float heat = sfeLookHeat(burnt, off, px, edge);
        float scorch = smoothstep(kSfeScorchAt, kSfeSingeAt, heat);
        float singe = sfeLookEdge(kSfeSingeAt, heat, kSfeBurnGrain, px);
        float charred = sfeLookEdge(kSfeCharAt, heat, kSfeBurnGrain, px);
        float relief = 0.0;
        if (stuff == kSfeStuffMetal) {
            // Its paint browned and then burnt off to bare metal, the metal tempered from straw through bronze and
            // purple to blue the hotter it went, and past that dark with scale.
            o.albedo = mix(o.albedo, o.albedo * kSfeBrowned, scorch * (1.0 - bare));
            float t = (heat - kSfeSingeAt) / (kSfeOxideAt - kSfeSingeAt);
            vec3 tempered = mix(mix(kSfeSteel, o.albedo, bare), kSfeStraw, smoothstep(kSfeScorchAt + 0.1, kSfeSingeAt, heat));
            tempered = mix(tempered, kSfeBronze, smoothstep(0.0, 0.3, t));
            tempered = mix(tempered, kSfePurple, smoothstep(0.3, 0.6, t));
            tempered = mix(tempered, kSfeBlue, smoothstep(0.6, 0.9, t));
            float shows = max(bare, singe);
            o.albedo = mix(o.albedo, tempered, shows);
            o.metal = mix(o.metal, 1.0, shows);
            o.rough = mix(o.rough, 0.4, singe * (1.0 - bare));
            float scaled = sfeLookEdge(kSfeOxideAt, heat, kSfeBurnGrain, px);
            o.albedo = mix(o.albedo, kSfeOxide, scaled);
            o.metal = mix(o.metal, 0.2, scaled);
            o.rough = mix(o.rough, 0.7, scaled);
        } else if (soaks) {
            // Browned, singed dark brown and charred black, wood's char crazed into cells standing out of their cracks;
            // paper, card and cloth burnt through where the fire went hottest.
            o.albedo = mix(o.albedo, o.albedo * kSfeBrowned, scorch);
            o.albedo = mix(o.albedo, kSfeSinged, singe);
            o.rough = mix(o.rough, 0.95, scorch * 0.6);
            vec3 charColour = kSfeChar;
            float charRough = 1.0;
            if (stuff == kSfeStuffWood) {
                float cracks = sfeLookCells(off + 83.0, kSfeCharGrain, kSfeCharCrack, px);
                charColour = mix(kSfeChar, kSfeChar * 0.3, cracks);
                charRough = mix(0.7, 1.0, cracks);
                relief = kSfeCharDepth * (1.0 - cracks) * charred;
            }
            o.albedo = mix(o.albedo, charColour, charred);
            o.rough = mix(o.rough, charRough, charred);
            holed = stuff != kSfeStuffWood && heat > kSfeHoleAt;
        } else {
            // Plastic browned, then blackened and blistered, glossy where it ran molten.
            o.albedo = mix(o.albedo, o.albedo * kSfeBrowned, scorch);
            o.albedo = mix(o.albedo, kSfeSinged, singe * 0.7);
            o.albedo = mix(o.albedo, kSfeChar, charred);
            o.rough = mix(o.rough, 0.5, charred);
            relief = kSfeBlisterDepth * smoothstep(0.45, 0.75, sfeLookNoise(off + 89.0, kSfeBlisterGrain, 2, px)) * singe;
        }
        // Sooted over it, darker, rougher and less metal, but for where it is charred black itself.
        float soot = sfeLookSoot(heat, off, px, hollow) * (1.0 - charred);
        o.albedo *= mix(1.0, kSfeSoot, soot);
        o.rough = mix(o.rough, 0.95, soot);
        o.metal *= 1.0 - soot;
        o.normal = sfeBumped(o.normal, relief);
    }

    // Silted below the waterline it lay at.
    float silted = 0.0;
    if (sfeLookByte(vLookMore.x, 0u) > 0.0) {
        vec3 silt;
        vec2 coat = sfeLookSilt(off, px, hollow, silt, silted);
        o.albedo = mix(o.albedo, silt, coat.x);
        o.rough = mix(o.rough, mix(0.92, 0.22, wet), coat.x);
        o.metal *= 1.0 - min(coat.x * 2.0, 1.0);
        o.normal = normalize(mix(o.normal, sfeBumped(o.normal, kSfeSiltDepth * coat.y), coat.x));
    }

    // The ground's dirt on the sides that touched it, none below the waterline, where the silt is what lies on it.
    vec3 soil;
    vec2 dirt = sfeLookDirt(off, px, hollow, onSide, soil) * (1.0 - silted);
    o.albedo = mix(o.albedo, soil, dirt.x);
    o.rough = mix(o.rough, mix(0.95, 0.3, wet), dirt.x);
    o.metal *= 1.0 - dirt.x;
    o.normal = normalize(mix(o.normal, sfeBumped(o.normal, kSfeDirtDepth * dirt.y), dirt.x));
    // Burnt through: last, as everything above takes its neighbouring pixels' values.
    if (holed) {
        discard;
    }
    return o;
}

// A piece of litter's glass (glass.fs, the rest of its look being sfeLooked's): x how clouded it is, milky in patches
// the more worn and white along its scuffed edges and in its hollows; y how much of it is hidden under what lies on it,
// of `soil`: a fire's soot, over that the lake's silt below its waterline, over both the ground's dirt.
vec2 sfeLookedGlass(out vec3 soil) {
    float wear = sfeLookWear();
    vec3 off = vec3(sfeLookRandom(2u), sfeLookRandom(3u), sfeLookRandom(4u)) * 512.0;
    float px = max(length(fwidth(vRest)), 1e-6);
    float curve = sfeCurvature();
    float edge = smoothstep(50.0, 250.0, curve);
    float hollow = smoothstep(-40.0, -200.0, curve);
    float cloud = wear * (0.35 + 0.8 * sfeLookNoise(off + 11.0, kSfeStainGrain, 3, px) + 0.5 * edge + 0.5 * hollow);
    // Each layer over the ones under it: `cover` how much they hide together, `lain` their colour times that.
    float cover = 0.0;
    vec3 lain = vec3(0.0);
    float burnt = sfeLookByte(vLookMore.y, 0u);
    if (burnt > 0.0) {
        cover = sfeLookSoot(sfeLookHeat(burnt, off, px, edge), off, px, hollow);
        lain = kSfeChar * cover;
    }
    float silted = 0.0;
    if (sfeLookByte(vLookMore.x, 0u) > 0.0) {
        vec3 silt;
        float coat = sfeLookSilt(off, px, hollow, silt, silted).x;
        lain = mix(lain, silt, coat);
        cover = mix(cover, 1.0, coat);
    }
    float dirt = sfeLookDirt(off, px, hollow, sfeLookSide(), soil).x * (1.0 - silted);
    lain = mix(lain, soil, dirt);
    cover = mix(cover, 1.0, dirt);
    soil = lain / max(cover, 1e-4);
    return vec2(clamp(cloud, 0.0, 1.0), cover);
}

#endif
