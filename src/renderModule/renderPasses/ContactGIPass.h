#pragma once
#include <vector>
#include "renderModule/Held.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	class ContactGIPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		struct Params {
			float lightDirX = 0.f, lightDirY = -1.f, lightDirZ = 0.f;
			float contactStrength = 0.9f;
			float lightColorX = 1.f, lightColorY = 0.96f, lightColorZ = 0.88f;
			float giStrength = 0.5f;
			float viewX = 0.f, viewY = 0.f, viewZ = 0.f;
			float maxDistance = 1.35f;
			float resX = 1.f, resY = 1.f;
			float thickness = 0.08f;
			float giRadius = 0.55f;
		};
		static_assert(sizeof(Params) == 64);

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
