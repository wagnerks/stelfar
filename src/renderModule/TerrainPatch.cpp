#include "renderModule/TerrainPatch.h"

#include "logsModule/logger.h"

namespace SFE::Render {
	namespace {
		// The patch as last published, and RenderSystem's two stand-ins for its heights. Main thread.
		struct PatchState {
			TerrainPatch published{};
			RenderHardwareInterface* rhi = nullptr;
			SamplerHandle nearest{};
			TextureHandle flat{};
		};

		PatchState& state() {
			static PatchState patch;
			return patch;
		}
	}

	void publishTerrainPatch(const TerrainPatch& patch) {
		state().published = patch;
	}

	const TerrainPatch& terrainPatch() {
		return state().published;
	}

	void createTerrainPatch(RenderHardwareInterface& rhi) {
		auto& patch = state();
		if (patch.rhi) {
			LogsModule::Logger::LOG_ERROR("TerrainPatch: made a second time; the first stands");
			return;
		}
		patch.rhi = &rhi;
		// texelFetch reads a vertex's height as it is: nearest.
		SamplerDesc nearest;
		nearest.minFilter = FilterMode::Nearest;
		nearest.magFilter = FilterMode::Nearest;
		patch.nearest = rhi.createSampler(nearest);
		TextureDesc desc;
		desc.width = 1;
		desc.height = 1;
		desc.format = TextureFormat::R32F;
		desc.usage = TextureUsage::Sampled;
		const float height = 0.f;
		patch.flat = rhi.createTexture(desc, &height);
	}

	void destroyTerrainPatch() {
		auto& patch = state();
		if (patch.rhi) {
			// The two stand-ins alone: the heights published are their producer's.
			if (patch.nearest) {
				patch.rhi->destroySampler(patch.nearest);
			}
			if (patch.flat) {
				patch.rhi->destroyTexture(patch.flat);
			}
		}
		patch = {};
	}

	std::pair<TextureHandle, SamplerHandle> terrainPatchHeights() {
		const auto& patch = state();
		return { patch.published.heights ? patch.published.heights : patch.flat, patch.nearest };
	}
}
