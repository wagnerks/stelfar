#pragma once
#include <optional>
#include <vector>

#include "componentsModule/SkyComponent.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	struct IblMaps {
		TextureHandle irradiance;
		TextureHandle prefilter;
		TextureHandle brdfLut;
		SamplerHandle irradianceSampler;
		SamplerHandle prefilterSampler;
		SamplerHandle lutSampler;
	};

	// What the maps hold: the irradiance cube's faces, the prefiltered cube's by mip, and the BRDF table.
	struct IblTexels {
		std::vector<float> irradiance;
		std::vector<std::vector<float>> prefilter;
		std::vector<float> brdfLut;
	};
	// Worked out on any thread from `sky` (none: the studio environment), most of what a bake costs.
	IblTexels computeIbl(const std::optional<ComponentsModule::SkyComponent>& sky, float sunIntensity);
	// On the render thread: into `maps`, made the first time and written over after.
	void uploadIbl(RenderHardwareInterface* rhi, const IblTexels& texels, IblMaps& maps);
}
