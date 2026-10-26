// How a face reflects light (Cook-Torrance, GGX) and how much of the sun reaches it: the scene's (deferred_shading.fs),
// and whatever is lit as the scene is (particle_liquid_surface.fs).
const float PI = 3.14159265;
const float MAX_REFLECTION_LOD = 4.0;

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

// f90: how far the reflectance may rise at grazing angles. 1 for anything with a normal specular level; a surface that
// asks for less (grass, soil) keeps its grazing highlight down too instead of turning into a white sheen against the sun.
vec3 cookTorrance(vec3 N, vec3 V, vec3 L, vec3 albedo, vec3 F0, float roughness, float metallic, vec3 radiance, float f90) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = F0 + (max(vec3(f90), F0) - F0) * pow(clamp(1.0 - max(dot(H, V), 0.0), 0.0, 1.0), 5.0);
    vec3 specular = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 0.001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

// How much of the sun reaches a face: as much as the shadows let through (`shadow`), and a side turned away from it
// (`NdotL`, the face's normal on the way to the sun) as much out of it as one under a cast shadow, by
// `shadowIntensity`, over a wide range of angles: a normal map's bumps swing the normal across the terminator, and a
// narrow one cut hard black blotches into a sunlit trunk. What the light comes through, `NdotL` 1.
float sfeSunReaching(float shadow, float NdotL, float shadowIntensity) {
    return min(shadow, 1.0 - shadowIntensity * (1.0 - smoothstep(-0.35, 0.3, NdotL)));
}
