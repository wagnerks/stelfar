#pragma once
#include <string>
#include <unordered_map>

#include "renderModule/renderPasses/RenderPass.h"
#include "renderModule/GpuScene.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render::RenderPasses {
	class GlassPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		struct Params {
			float viewPosX = 0.f;
			float viewPosY = 0.f;
			float viewPosZ = 0.f;
			float _pad2 = 0.f;
			float resolutionX = 1.f;
			float resolutionY = 1.f;
			float _pad3 = 0.f;
			float roughness = 0.05f;
			float lightX = 0.f;
			float lightY = -1.f;
			float lightZ = 0.f;
			float _pad0 = 0.f;
			float colorX = 1.f;
			float colorY = 1.f;
			float colorZ = 1.f;
			float _pad1 = 0.f;
			float _pad4[3]{};
			float sphereSpacing = 8.f;
		};

		void ensureTargets(SystemsModule::RenderData& renderDataHandle);
		// The pipeline drawing with `fragmentShader`, made the first time it is asked for.
		// `depthOnly`: a solid group's (MaterialComponent::solidGlass) depth, laid before its colour, the colour kept.
		PipelineHandle pipelineFor(const std::string& fragmentShader, bool depthOnly = false);

		Params mParams;
		TextureHandle mSceneCopy{};
		SamplerHandle mSampler{};
		FramebufferHandle mSceneCopyFbo{};
		// Final and depth both alternate between frames, so there is one framebuffer per pair they come in.
		struct GlassTarget {
			TextureHandle finalTex;
			TextureHandle depthTex;
			FramebufferHandle fbo;
		};
		std::vector<GlassTarget> mGlassTargets;
		FramebufferHandle mGlassFbo{};
		GraphicsPipelineDesc mPipelineDesc; // all but the fragment shader
		std::unordered_map<std::string, PipelineHandle> mPipelines;
		RenderPassHandle mRenderPass{};
		DescriptorSetLayoutHandle mLayout{};
		// One a glass group (a fragment shader), sorted and drawn apart from the others.
		struct Group {
			GpuScene::View view;
			std::vector<DescriptorSetHandle> sets; // one per frame slot
		};
		std::vector<Group> mGroups;
	};
}
