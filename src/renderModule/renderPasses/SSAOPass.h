#pragma once

#include <vector>

#include "rhi/RenderHardwareInterface.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	class SSAOPass : public RenderPass {
	public:
		struct Data {
			std::vector<Math::Vec3> mSsaoKernel;

			TextureHandle mNoiseTexture;
			FramebufferHandle mSsaoFbo;
			FramebufferHandle mSsaoBlurFbo;
			TextureHandle mSsaoColorBuffer;
			TextureHandle mSsaoColorBufferBlur;

			SamplerHandle sampler;

			int mKernelSize = 16;
			float mRadius = 0.5f;
			float mBias = 0.7f;
			int samples = 16;
			float intencity = 1.5f;
			float scale = 0.5f;
			float sample_rad = 1.2f;
			float max_distance = 0.01f;

			float sigmaS = 1.0f;
			float sigmaL = 1.2f;
		};
		
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;
	private:
		// ssao.fs's: the power, the blades' floor, the radius and the thickness from GraphicsSettings each frame.
		struct SSAOParams
		{
			float intensity = 1.5f;
			float grassFloor = 0.85f;
			float radius = 1.5f;
			float thickness = 0.5f;
			// TAA jitter phase. Rotates the sample disc per frame so the static screen-space
			// pattern averages out in the TAA resolve instead of standing still as ribbing.
			float frameIndex = 0.f;
		};

		SSAOParams params;

		struct SSAOBlurParams
		{
			float sigmaS = 1.f;
			float sigmaL = 1.f;
			float facS = 1.f;
			float facL = 1.f;

			void updateFac()
			{
				facL = -1.f / (2.f * sigmaL * sigmaL);
				facS = -1.f / (2.f * sigmaS * sigmaS);
			}
		};

		SSAOBlurParams blurParams;
		// One set per frame slot: the G-buffer it samples alternates with the slot, and writing a set the previous
		// frame still binds costs a copy of it. Each keeps what it was last written with.
		struct Slot {
			Render::DescriptorSetHandle mainSet;
			Render::DescriptorSetHandle blurSet;
			uint32_t mainParamsBufferId = UINT32_MAX;
			uint32_t blurParamsBufferId = UINT32_MAX;
			Render::TextureHandle position;
			Render::TextureHandle normal;
			Render::TextureHandle material;
			Render::BufferHandle camera;
		};
		std::vector<Slot> mSlots;

		bool ssaoDebugWindow = false;
		Data mData{};

		PipelineHandle mainPipeline;
		PipelineHandle blurPipeline;

		RenderPassHandle main;
		RenderPassHandle blur;

		Render::DescriptorSetLayoutHandle mainTexSetLayout;

		Render::DescriptorSetLayoutHandle blurTexSetLayout;

		int mSsaoW = 0;
		int mSsaoH = 0;
	};
}
