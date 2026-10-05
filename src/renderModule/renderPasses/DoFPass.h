#pragma once
#include <vector>
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	class DoFPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		struct Params {
			float viewX = 0.f, viewY = 0.f, viewZ = 0.f;
			float focusDistance = 18.f;
			float focusRange = 28.f;
			float maxBlur = 3.5f;
			float resX = 1.f, resY = 1.f;
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
