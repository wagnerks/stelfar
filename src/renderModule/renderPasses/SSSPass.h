#pragma once
#include <vector>
#include "renderModule/Held.h"
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
		Held<TextureHandle> mColor;
		Held<FramebufferHandle> mFbo;
		Held<SamplerHandle> mLinear;
		Held<SamplerHandle> mNearest;
		Held<PipelineHandle> mPipeline;
		Held<RenderPassHandle> mRenderPass;
		Held<DescriptorSetLayoutHandle> mLayout;
		std::vector<Held<DescriptorSetHandle>> mSets; // one per frame slot
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};
}
