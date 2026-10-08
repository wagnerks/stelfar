#pragma once

#include <vector>

#include "rhi/RenderHardwareInterface.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	// The sun's shadow over what is drawn, for the lighting to read (deferred_shading.fs): looked up at every pixel with
	// the filter's disk turned by the pixel's place in a 4x4 block (sun_shadow.fs), then averaged over exactly one block
	// (sun_shadow_blur.fs), the same every frame. The water still looks its own up (water.fs).
	class SunShadowPass : public RenderPass {
	public:
		// The blurred shadow on the frame blackboard: R8 at the size the picture is drawn at, 0 lit, 1 in shadow.
		static constexpr const char* kMask = "sunShadow.mask";
		// Tuned by eye: the samples of the filter's disk at each pixel (4..16; a block has sixteen times as many), and
		// metres off the centre's plane, a metre from the eye, past which a texel counts for little in the blur.
		struct Settings {
			float samples;
			float tolerance;
		};
		static Settings settings;

		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		// sun_shadow_blur.fs's block.
		struct BlurParams {
			float tolerance = 0.f;
			float pad[3] = {};
		};
		// One set of each per frame slot: the G-buffer they read alternates with the slot (SSAOPass). Each keeps what it
		// was last written with.
		struct Slot {
			DescriptorSetHandle maskSet;
			DescriptorSetHandle blurSet;
			TextureHandle depth;
			TextureHandle normal;
			TextureHandle material;
			TextureHandle csmDepth;
			BufferHandle camera;
			BufferHandle shadows;
			BufferHandle matrices;
			BufferHandle blurParams;
		};
		std::vector<Slot> mSlots;

		DescriptorSetLayoutHandle mMaskSetLayout;
		DescriptorSetLayoutHandle mBlurSetLayout;
		RenderPassHandle mMaskPass;
		RenderPassHandle mBlurPass;
		PipelineHandle mMaskPipeline;
		PipelineHandle mBlurPipeline;

		TextureHandle mMask;
		FramebufferHandle mMaskFbo;
		TextureHandle mBlurred;
		FramebufferHandle mBlurredFbo;
		SamplerHandle mSampler;
	};
}
