#pragma once
#include <vector>
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	class SSSPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		struct Params {
			float strength = 1.f;
			float radius = 5.f;
			float resX = 1.f;
			float resY = 1.f;
		};

		Params mParams;
		TextureHandle mColor{};
		FramebufferHandle mFbo{};
		SamplerHandle mLinear{};
		SamplerHandle mNearest{};
		PipelineHandle mPipeline{};
		RenderPassHandle mRenderPass{};
		DescriptorSetLayoutHandle mLayout{};
		std::vector<DescriptorSetHandle> mSets; // one per frame slot
	};
}
