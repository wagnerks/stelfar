#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "renderModule/Held.h"
#include "renderModule/renderPasses/FinalDepthTargets.h"
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
		};
		static_assert(sizeof(Params) == 64, "sfe_glass.glsl's PushConstants, member for member");

		// The pipeline drawing with `vertexShader` and `fragmentShader`, made the first time it is asked for.
		// `depthOnly`: a solid group's (MaterialComponent::solidGlass) depth, laid before its colour, the colour kept.
		PipelineHandle pipelineFor(const std::string& vertexShader, const std::string& fragmentShader, bool depthOnly = false);

		Params mParams;
		// The picture's copy (Render::kSceneCopy), shared with every pass that copies the picture before it reads it.
		Held<TextureHandle> mSceneCopy;
		Held<SamplerHandle> mSampler;
		Held<FramebufferHandle> mSceneCopyFbo;
		// Over the picture and the scene's depth, which the glass is tested against and drawn into.
		FinalDepthTargets mTargets;
		GraphicsPipelineDesc mPipelineDesc; // all but the fragment shader
		std::unordered_map<std::string, Held<PipelineHandle>> mPipelines;
		Held<RenderPassHandle> mRenderPass;
		Held<DescriptorSetLayoutHandle> mLayout;
		// The modules of every pipeline made, the lazy ones among them.
		std::vector<Held<ShaderModuleHandle>> mShaders;
		// One a glass group (a fragment shader), sorted and drawn apart from the others.
		struct Group {
			GpuScene::View view;
			std::vector<Held<DescriptorSetHandle>> sets; // one per frame slot
		};
		std::vector<Group> mGroups;
	};
}
