#ifndef SFE_SUN_VISIBLE_GLSL
#define SFE_SUN_VISIBLE_GLSL
// How much of the sun reaches a point in the air, 0 to 1, by the shadow cascades: for what is drawn over the picture
// after it is lit (AirLifePass's specks, ParticleSpritePass's motes). The includer declares `shadowMap` (a
// sampler2DArrayShadow) and `lightSpaceMatrices` (the cascades' matrices) first; `cascades` is how many there are.
float sfeSunVisible(vec3 world, int cascades) {
    for (int layer = 0; layer < cascades; ++layer) {
        vec4 ls = lightSpaceMatrices[layer] * vec4(world, 1.0);
        vec3 proj = ls.xyz / ls.w * 0.5 + 0.5;
        if (all(greaterThan(proj, vec3(0.0))) && all(lessThan(proj, vec3(1.0)))) {
            return texture(shadowMap, vec4(proj.xy, float(layer), proj.z - 0.0005));
        }
    }
    return 1.0;
}
#endif
