#pragma once
#include <utility>
#include "mathModule/Forward.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// The ground drawn dense near the camera (g_buffer_ground.vs, sfe_ground_patch.glsl): a square of a heightfield mesh's
	// vertices cut finer, at the mesh's heights raised or lowered by the application (sfeUserGroundOffset) and eased out
	// to the mesh at its edge; the mesh itself put out of sight under it. Where and how fine is its producer's; the
	// engine draws it (GeometryPass, with the first kTerrainFS group's textures, until step 6's DrawClass).
	struct TerrainPatch {
		TextureHandle heights{}; // R32F, a texel a mesh vertex, rows along z (AssetsModule::terrainHeightsTexture)
		Math::Vec4 grid{};       // xy the world x and z of the mesh's first vertex, z its vertices' spacing, w its base height
		Math::Vec4 square{};     // xy its least world x and z, on mesh vertices; z its side, whole mesh cells (0: none);
		                         // w its vertices' spacing, the mesh's divided by a whole number
		float deepest = 0.f;     // metres the application lowers the ground at most: the mesh goes max(1, 2 deepest) under
	};

	// The producer, in a pass's prepare(), main thread: this frame's patch, kept until published again.
	void publishTerrainPatch(const TerrainPatch& patch);
	const TerrainPatch& terrainPatch();
	// RenderSystem only: create (a nearest sampler and one R32F texel of 0) in its constructor; destroy in its destructor
	// frees those two alone and resets the published patch to {} -- never the published heights, which are the
	// producer's (AssetsModule::terrainHeightsTexture's cached asset, Terrain.cpp:554-568, GrassPass's binding 17 too);
	// heights() the published heights with the nearest sampler, or the texel of 0.
	void createTerrainPatch(RenderHardwareInterface& rhi);
	void destroyTerrainPatch();
	std::pair<TextureHandle, SamplerHandle> terrainPatchHeights();
}
