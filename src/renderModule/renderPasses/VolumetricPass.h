#pragma once
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	class VolumetricPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		struct Params {
			float viewX = 0.f, viewY = 0.f, viewZ = 0.f;
			float density = 0.022f;
			float lightX = 0.f, lightY = -1.f, lightZ = 0.f;
			float anisotropy = 0.68f;
			float colorX = 1.f, colorY = 0.96f, colorZ = 0.88f;
			float intensity = 2.4f;
			float resX = 1.f, resY = 1.f;
			int cascadeCount = 4;
			float maxDistance = 70.f;
			float cloudCoverage = 0.f;
		};

		Params mParams;
		TextureHandle mHalfTex{};
		FramebufferHandle mHalfFbo{};
		TextureHandle mColor{};
		FramebufferHandle mFbo{};
		SamplerHandle mLinear{};
		SamplerHandle mNearest{};
		PipelineHandle mMarchPipeline{};
		PipelineHandle mApplyPipeline{};
		RenderPassHandle mRenderPass{};
		DescriptorSetLayoutHandle mMarchLayout{};
		DescriptorSetHandle mMarchSet{};
		DescriptorSetLayoutHandle mApplyLayout{};
		DescriptorSetHandle mApplySet{};
	};
}
