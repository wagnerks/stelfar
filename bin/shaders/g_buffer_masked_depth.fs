#version 450 core
// sfe_gbuffer_masked.glsl's cut alone, nothing shaded: the depth GeometryPass lays before it shades the leaves.
#define SFE_GBUFFER_PREPASS
#include "sfe_gbuffer_masked.glsl"
