#pragma once
#include "assetsModule/modelModule/BoundingVolume.h"
#include "renderModule/FrameBlackboard.h"
#include "renderModule/GpuScene.h"
#include "renderModule/MaterialTextures.h"
#include "rhi/RenderHardwareInterface.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	class PointLightPass : public RenderPass {
	public:
		using Data = Render::PointShadowFrame;

		PointLightPass();
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;

	private:
		void publish(SystemsModule::RenderData& renderDataHandle);
		void collectLights(const FrustumModule::Frustum& camFrustum, const Math::Vec3& cameraPos, float time, std::vector<Math::Mat4>& matrices, std::vector<Data::Light>& lights);
		void ensureGpu(SystemsModule::RenderData& renderDataHandle);

		FramebufferHandle lightFramebuffer;
		TextureHandle mLightDepthMaps;
		SamplerHandle mLightDepthMapsSampler;
		RenderPassHandle renderPass;
		PipelineHandle pipeline;
		// Opaque casters, depth only: a discard anywhere in a fragment shader takes its pipeline off the early depth test.
		PipelineHandle pipelineOpaque;
		Render::DescriptorSetLayoutHandle mBufferLayout{};
		Render::DescriptorSetLayoutHandle mTextureLayout{};
		SingleTextureSets mMaskSets;

		bool mInited = false;
		bool mGpuReady = false;
		Data data{};

		static constexpr int shadowResolution = 1024;
		static constexpr int maxLights = 12;
		static constexpr int facesPerLight = 6;
		static constexpr int maxShadowFaces = maxLights * facesPerLight;
		static_assert(maxShadowFaces == Data::kMaxShadowFaces);

		// A face's own casters, culled against its frustum alone: a caster goes only to the faces it shows in.
		struct FaceView {
			GpuScene::View view;
			Render::DescriptorSetHandle set{};
			uint32_t boundIds[5]{}; // transforms, bones, visible, materials, the transient matrices of binding 11
		};
		std::array<FaceView, maxShadowFaces> mViews;
		// This frame's faces to cull, kept so they do not allocate every frame.
		std::vector<GpuScene::View*> mFaceViews;
		std::vector<FrustumModule::Frustum> mFaceFrusta;
	};
}
