#pragma once

#include <cstdint>
#include <vector>

#include "componentsModule/TerrainComponent.h"
#include "rhi/RenderHardwareInterface.h"

namespace AssetsModule {
	class Model;

	// The terrain mesh for these parameters, generated once per distinct set and kept as an asset.
	// texCoords carry world x/z in metres, texCoords1.x the height above the water line: the terrain shader
	// shades from those instead of textures.
	// Cells a side of one of the terrain mesh's chunks.
	inline constexpr int kTerrainChunkCells = 32;
	Model* getOrCreateTerrain(const SFE::ComponentsModule::TerrainComponent& terrain);

	// The heights the terrain mesh is built from, row by row along z: resolution x resolution samples spaced
	// size / (resolution - 1) apart, from -size/2 to +size/2 on both axes, in the terrain's local space.
	// Computed once per distinct terrain and kept, on disk too.
	const std::vector<float>& terrainHeights(const SFE::ComponentsModule::TerrainComponent& terrain);
	// terrainHeights on the GPU, R32F, a texel a sample, made once per distinct terrain.
	SFE::Render::TextureHandle terrainHeightsTexture(const SFE::ComponentsModule::TerrainComponent& terrain);
	// A hash of everything the terrain is built from, its footpaths and patches too, for caches of what is built on top
	// of it.
	uint64_t terrainCacheKey(const SFE::ComponentsModule::TerrainComponent& terrain);

	// The height of the terrain mesh at x/z in its local space, on the very triangle drawn there, so whatever is
	// stood on it neither floats nor sinks. Its gradient comes back through `slopeX` and `slopeZ` when asked for.
	// `heights` are terrainHeights(terrain).
	float terrainSurfaceHeight(const SFE::ComponentsModule::TerrainComponent& terrain, const std::vector<float>& heights, float x, float z,
		float* slopeX = nullptr, float* slopeZ = nullptr);

	// Metres a texel of terrainNoiseTexture covers; sfe_terrain.glsl has the same.
	constexpr float kTerrainNoiseTexelMetres = 4.f;
	// The terrain shader's slow noise, sfeFbm exactly, baked once per terrain instead of for every pixel: r the broad
	// variation, g where sandy coves are, b which of a material's two tilings shows, a the signed distance to the road
	// alone, clearings aside, / kRoadTextureRange + 0.5. Centred on the terrain, a texel every kTerrainNoiseTexelMetres.
	// The terrain's material and the grass map read it.
	constexpr float kRoadTextureRange = 16.f;
	SFE::Render::TextureHandle terrainNoiseTexture(const SFE::ComponentsModule::TerrainComponent& terrain);
	// Where along the road each place is, to lay a texture down it: r metres across it (signed as terrainRoadDistance,
	// clearings aside) and g metres along it from its first point, by the nearest point of it going on straight past its
	// ends, so its round ends have them too. RG32F, texels as terrainNoiseTexture's. The terrain's material reads it.
	SFE::Render::TextureHandle terrainRoadFrameTexture(const SFE::ComponentsModule::TerrainComponent& terrain);

	// Metres from the terrain's road's centreline, x/z in the terrain's own space, signed by the side of it; far from
	// any road (or with none) kRoadFar. The road itself is kRoadHalfWidth either side of the line. A clearing is
	// trodden ground as the road's sides are: in it kClearingDistance, past its edge that and how far past.
	constexpr float kClearingDistance = 1.6f;
	constexpr float kRoadFar = 1000.f;
	constexpr float kRoadHalfWidth = 1.9f;
	float terrainRoadDistance(const SFE::ComponentsModule::TerrainComponent& terrain, float x, float z);
	// Metres past the edge of the nearest clearing, x/z in the terrain's own space: below zero inside one, kRoadFar with
	// none.
	float terrainClearingDistance(const SFE::ComponentsModule::TerrainComponent& terrain, float x, float z);

	// The terrain's footpaths and bare patches made ready to be asked about over and over: built once per distinct set
	// and kept, so the pointer stays good. Null with none.
	struct TerrainPathSet;
	const TerrainPathSet* terrainPathSet(const SFE::ComponentsModule::TerrainComponent& terrain);
	// Metres past the edge of the nearest footpath or patch, x/z in the terrain's own space: below zero on one. The
	// edge is ragged, a few tenths in or out, and where two meet it runs round in a curve. Never more than kPathFar, which
	// is all it is further off, or with none.
	constexpr float kPathFar = 8.f;
	float terrainPathDistance(const TerrainPathSet* paths, float x, float z);
	float terrainPathDistance(const SFE::ComponentsModule::TerrainComponent& terrain, float x, float z);
	// How worn the ground is at x/z in the terrain's own space, 0..1: a patch's wear in its middle, less and less out to
	// a ragged metre and a half past its edge; a path's a little less than its wear says across its width and half a
	// metre past it. 0 with none. `core`, if given, is metres into the strip trodden bare down the middle of the nearest
	// path: below zero off it, -kPathFar with none.
	float terrainPathWear(const TerrainPathSet* paths, float x, float z, float* core = nullptr);
	// How far into bare earth ground worn `wear` with a path's strip `core` (terrainPathWear's) is at x/z: above zero
	// bare, below it grass, the two mixing a tenth or so either side. Spots that spread and join as the wear rises, all
	// of it bare at 1. sfeTerrainBare in sfe_terrain.glsl is the same; the grass grows by this.
	float terrainBareEarth(float wear, float core, float x, float z);
	// terrainPathDistance in r, terrainClearingDistance in g, both clamped to kPathFar either way, terrainPathWear in b
	// and its core in a, baked over just the box round the paths and patches and the clearings that reach into it:
	// RGBA32F, a texel every kPathTexelMetres. Its first row says where it lies: texel (0, 0) the x/z of the next row's
	// first texel, (1, 0).r the metres between two (none with no paths). sfeTerrainPath reads it so; the terrain's
	// material has it.
	constexpr float kPathTexelMetres = 0.25f;
	SFE::Render::TextureHandle terrainPathTexture(const SFE::ComponentsModule::TerrainComponent& terrain);

	// Smooth noise in [0, 1], the one the terrain is shaped with; for anything laid out over it.
	float terrainNoise(float x, float z, int seed, int octaves);

	constexpr const char* kTerrainFS = "shaders/g_buffer_terrain.fs";
}
