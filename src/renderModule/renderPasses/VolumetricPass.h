#pragma once
#include <vector>
#include "renderModule/Held.h"
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
		Held<TextureHandle> mMarchTex;
		Held<FramebufferHandle> mMarchFbo;
		Held<TextureHandle> mColor;
		Held<FramebufferHandle> mFbo;
		Held<SamplerHandle> mLinear;
		Held<SamplerHandle> mNearest;
		Held<PipelineHandle> mMarchPipeline;
		Held<PipelineHandle> mApplyPipeline;
		Held<RenderPassHandle> mRenderPass;
		Held<DescriptorSetLayoutHandle> mMarchLayout;
		Held<DescriptorSetHandle> mMarchSet;
		Held<DescriptorSetLayoutHandle> mApplyLayout;
		Held<DescriptorSetHandle> mApplySet;
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};
}
