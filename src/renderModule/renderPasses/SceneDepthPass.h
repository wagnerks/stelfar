#pragma once
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	// The G-buffer's depth, copied once it is complete into a texture the screen passes can read while the depth
	// buffer itself is bound to one of them: sfeScenePosition puts world positions back together from it.
	class SceneDepthPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		TextureHandle mDepth{};
		FramebufferHandle mFbo{};
		SamplerHandle mNearest{};
		PipelineHandle mPipeline{};
		RenderPassHandle mRenderPass{};
		DescriptorSetLayoutHandle mLayout{};
		std::vector<DescriptorSetHandle> mSets; // one per frame slot
	};
}
