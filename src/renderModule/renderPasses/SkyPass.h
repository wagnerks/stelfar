#pragma once

#include <vector>

#include "renderModule/Held.h"
#include "renderModule/renderPasses/FinalDepthTargets.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	// The atmosphere behind everything, for a scene with a SkyComponent: drawn on the far plane after lighting, so
	// only the pixels no surface covered take it.
	class SkyPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;

	private:
		FinalDepthTargets mTargets;
		Held<RenderPassHandle> mRenderPass;
		Held<PipelineHandle> mPipeline;
		Held<DescriptorSetLayoutHandle> mLayout;
		std::vector<Held<DescriptorSetHandle>> mSets; // one per frame slot
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};
}
