#pragma once
#include <vector>
#include "mathModule/Forward.h"
#include "renderModule/Held.h"
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
		Held<TextureHandle> mHistory[2];
		Held<FramebufferHandle> mFbo[2];
		int mWrite = 0;
		// None in a TAA made again (its setting turned on): what it held before went with it.
		bool mHasHistory = false;
		Held<SamplerHandle> mLinear;
		Held<SamplerHandle> mNearest;
		Held<PipelineHandle> mPipeline;
		Held<RenderPassHandle> mRenderPass;
		Held<DescriptorSetLayoutHandle> mLayout;
		std::vector<Held<DescriptorSetHandle>> mSets; // one per frame slot
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};
}
