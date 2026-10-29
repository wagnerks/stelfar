#pragma once

#include <vector>

#include "renderModule/Held.h"
#include "renderModule/renderPasses/FinalDepthTargets.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	// Mist lying on the water and a thin haze in the distance, for a scene with a SkyComponent, and the fade into that
	// haze where the view distance ends.
	// Both are fog of an exact thickness along each view ray -- the mist thinning out exponentially with height above
	// the water line, drifting in patches with the wind -- lit by the sky and the sun, and blended over the finished
	// image after the water.
	class MistPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;

	private:
		FinalDepthTargets mTargets;
		Held<RenderPassHandle> mRenderPass;
		Held<PipelineHandle> mPipeline;
		Held<DescriptorSetLayoutHandle> mLayout;
		Held<SamplerHandle> mNearest;
		std::vector<Held<DescriptorSetHandle>> mSets; // one per frame slot
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};
}
