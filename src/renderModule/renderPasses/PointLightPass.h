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
		void collectLights(const FrustumModule::Frustum& camFrustum, const Math::Vec3& cameraPos, float time, std::vector<Math::Mat4>& matrices, std::vector<Math::Vec4>& spheres, std::vector<ecss::SectorId>& entities);
		void ensureGpu(SystemsModule::RenderData& renderDataHandle);

		std::vector<Math::Vec4> mSpheres; // this frame's light spheres, kept so they do not allocate every frame
		FramebufferHandle lightFramebuffer;
		TextureHandle mLightDepthMaps;
		SamplerHandle mLightDepthMapsSampler;
		RenderPassHandle renderPass;
		PipelineHandle pipeline;
		Render::DescriptorSetLayoutHandle mBufferLayout{};
		Render::DescriptorSetLayoutHandle mTextureLayout{};
		Render::DescriptorSetHandle mSet{};
		uint32_t mBoundIds[4]{};
		uint32_t mMatricesId = UINT32_MAX; // transient buffer binding 11 was last written with
		GpuScene::View mView;
		SingleTextureSets mMaskSets;

		bool mInited = false;
		bool mGpuReady = false;
		Data data{};

		static constexpr int shadowResolution = 1024;
		static constexpr int maxLights = 12;
		static constexpr int facesPerLight = 6;
		static constexpr int maxShadowFaces = maxLights * facesPerLight;
		static_assert(maxShadowFaces == Data::kMaxShadowFaces);
	};
}
