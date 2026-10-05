#pragma once

#include <unordered_map>
#include <vector>

#include "componentsModule/MaterialComponent.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// The material texture bindings (DIFFUSE..OCCLUSION): the engine's default for every slot first,
	// then the material's own textures over them.
	std::vector<WriteDescriptor> materialTextureWrites(DescriptorSetHandle set, const ComponentsModule::Materials& materials);

	// Sets holding one texture at one binding, made once per texture. A missing texture binds white.
	class SingleTextureSets {
	public:
		DescriptorSetHandle get(DescriptorSetLayoutHandle layout, uint32_t binding, TextureHandle texture);

	private:
		std::unordered_map<uint32_t, DescriptorSetHandle> mSets;
	};
}
