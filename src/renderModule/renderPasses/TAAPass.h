#pragma once
#include <vector>
#include "mathModule/Forward.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	class TAAPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		struct Params {
			Math::Mat4 prevPV{ 1.f };
			Math::Mat4 curPV{ 1.f };
			float resX = 1.f;
			float resY = 1.f;
			float blend = 0.92f;
			int reset = 1;
		};

		Params mParams;
		TextureHandle mHistory[2]{};
		FramebufferHandle mFbo[2]{};
		int mWrite = 0;
		bool mHasHistory = false;
		SamplerHandle mLinear{};
		SamplerHandle mNearest{};
		PipelineHandle mPipeline{};
		RenderPassHandle mRenderPass{};
		DescriptorSetLayoutHandle mLayout{};
		std::vector<DescriptorSetHandle> mSets; // one per frame slot
	};
}
