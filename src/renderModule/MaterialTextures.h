#pragma once

#include <unordered_map>
#include <vector>

#include "componentsModule/MaterialComponent.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// The material texture bindings (DIFFUSE..EMISSIVE): the engine's default for every slot first,
	// then the material's own textures over them.
	std::vector<WriteDescriptor> materialTextureWrites(DescriptorSetHandle set, const ComponentsModule::Materials& materials);

	// Whether each of the material's textures is still on the device. One let go of (its model's,
	// AssetsModule::releaseModel) is not: nothing draws with it any more, and a set written with it is not bound again.
	bool texturesLive(const ComponentsModule::Materials& materials);

	// Sets holding one texture at one binding, made once per texture. A missing texture binds white, and so does one
	// let go of.
	class SingleTextureSets {
	public:
		DescriptorSetHandle get(DescriptorSetLayoutHandle layout, uint32_t binding, TextureHandle texture);

	private:
		std::unordered_map<uint32_t, DescriptorSetHandle> mSets;
	};
}
