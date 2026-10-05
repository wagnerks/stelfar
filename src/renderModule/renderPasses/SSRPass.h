#pragma once
#include <vector>
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	class SSRPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		struct Params {
			float viewPosX = 0.f;
			float viewPosY = 0.f;
			float viewPosZ = 0.f;
			float maxDistance = 12.f;
			float resolutionX = 1.f;
			float resolutionY = 1.f;
			float thickness = 0.4f;
			float stride = 0.35f;
		};

		Params mParams;
		TextureHandle mColor{};
		SamplerHandle mSampler{};
		FramebufferHandle mFbo{};
		PipelineHandle mPipeline{};
		RenderPassHandle mRenderPass{};
		DescriptorSetLayoutHandle mLayout{};
		std::vector<DescriptorSetHandle> mSets; // one per frame slot
	};
}
